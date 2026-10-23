// cu::flag 的端到端测试。这个程序本身就是一个用 cu::flag 写的命令行程序：每条用例带不同的参数起一次子进程，
// 比对退出码、stdout 和 stderr。误用（打印诊断后终止）也各在一个子进程里跑，只查它终止了、stderr 里有那句诊断。
// 子进程的输出只在那一条失败时才打印：诊断以 "cu::" 开头，会被 CI 报告当成异常输出。
// 单独看某一条：设环境变量 CU_FLAG_TEST_CHILD=1 再带参数运行，比如 CU_FLAG_TEST_CHILD=1 flag_test admin --help。

#include <cu/flag.h>
#include <cu/string_view.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// ---- 被测的程序 ------------------------------------------------------------------------------------

cu::flag::boolean verbose(false, "print more", {"verbose", "v"});
cu::flag::int64 retries(3, "how many times to retry", {"retries"});

// 普通函数也能当命令：参数是这个命令自己的选项的值
void role_grant(const std::string& role) {
    for (cu::string_view user : cu::flag::args()) {
        std::printf("grant %s to %.*s\n", role.c_str(), static_cast<int>(user.size()), user.data());
    }
}

int admin_remove() {
    for (cu::string_view user : cu::flag::args()) {
        std::printf("remove %.*s\n", static_cast<int>(user.size()), user.data());
    }
    return 0;
}

cu::flag::command admin("admin", "manage users", {
    cu::flag::command("register", "register a user",
        cu::flag::string("", "user name", {"name", "n"}),
        cu::flag::string("", "email address", {"email", "e"}),
        cu::flag::boolean(false, "give the user admin rights", {"admin"}),
        [](const std::string& name, const std::string& email, bool grant) {
            std::printf("register name=%s email=%s admin=%d verbose=%d\n", name.c_str(), email.c_str(), grant ? 1 : 0,
                        verbose.value() ? 1 : 0);
        }),
    cu::flag::command("remove", "remove the users given as arguments", admin_remove),
    cu::flag::command("role", "change what a user may do", {
        cu::flag::command("grant", "give a user a role",
            cu::flag::string("reader", "the role to give", {"role", "r"}),
            role_grant),
    }),
});

cu::flag::command status("status", "show the service status", [] {
    std::printf("status verbose=%d retries=%s\n", verbose.value() ? 1 : 0, std::to_string(retries.value()).c_str());
});

cu::flag::command fail("fail", "exit with code 5", [] { return 5; });

// ---- 误用 ------------------------------------------------------------------------------------------

// 子进程：mode 是 "1" 时就是上面那个程序，否则照 mode 误用一次。误用的都该终止，返回了就是没拦住。
int child_main(const std::string& mode, int argc, char* argv[]) {
    if (mode == "read_before_init") {
        std::printf("%d\n", verbose.value() ? 1 : 0);
    } else if (mode == "args_before_init") {
        std::printf("%d\n", static_cast<int>(cu::flag::args().size()));
    } else if (mode == "duplicate_option") {
        cu::flag::boolean again(false, "print more again", {"verbose"});
        (void)again;
    } else if (mode == "reserved_name") {
        cu::flag::boolean help(false, "a second help", {"h"});
        (void)help;
    } else if (mode == "bad_option_name") {
        cu::flag::boolean dashed(false, "dashed", {"-x"});
        (void)dashed;
    } else if (mode == "repeated_name") {
        cu::flag::boolean twice(false, "named twice", {"x", "x"});
        (void)twice;
    } else if (mode == "duplicate_command") {
        cu::flag::command again("status", "a second status", [] {});
        (void)again;
        cu::flag::init(argc, argv);
    } else if (mode == "duplicate_subcommand") {
        cu::flag::command group("group", "two children with one name", {
            cu::flag::command("a", "first", [] {}),
            cu::flag::command("a", "second", [] {}),
        });
        (void)group;
    } else if (mode == "bad_command_name") {
        cu::flag::command spaced("bad name", "has a space", [] {});
        (void)spaced;
    } else if (mode == "declared_in_function") {
        cu::flag::command late("late", "declares an option in its function", [] {
            cu::flag::boolean too_late(false, "declared too late", {"late"});
            (void)too_late;
        });
        (void)late;
        cu::flag::init(argc, argv);
    } else if (mode == "option_named_like_global") {
        cu::flag::command shadow("shadow", "has an option named like a global one",
            cu::flag::boolean(false, "a second verbose", {"verbose"}),
            [](bool) {});
        (void)shadow;
    } else if (mode == "options_of_a_command_share_a_name") {
        cu::flag::command twice("twice", "has two options with one name",
            cu::flag::boolean(false, "first", {"x"}),
            cu::flag::boolean(false, "second", {"x"}),
            [](bool, bool) {});
        (void)twice;
    } else if (mode == "init_twice") {
        // lambda 可以捕获东西
        cu::flag::command again("again", "calls init again", [argc, argv] { cu::flag::init(argc, argv); });
        (void)again;
        cu::flag::init(argc, argv);
    } else if (mode == "command_after_init") {
        cu::flag::command later("later", "declares a command while running", [] {
            cu::flag::command inner("inner", "too late", [] {});
            (void)inner;
        });
        (void)later;
        cu::flag::init(argc, argv);
    } else {
        cu::flag::init(argc, argv);
    }
    return 0;
}

// ---- 用例 ------------------------------------------------------------------------------------------

struct flag_case {
    std::string name;
    std::string mode;                // 子进程的 CU_FLAG_TEST_CHILD
    std::vector<std::string> args;   // argv[0] 以后的
    bool aborts;
    int code;                        // 正常退出时的退出码
    std::string out;                 // 期望的 stdout；终止的用例不查
    std::string err;                 // 期望的 stderr；终止的用例是 stderr 里要有的那句诊断
};

flag_case exits(const char* name, std::initializer_list<const char*> args, int code, const char* out,
                const char* err) {
    flag_case item;
    item.name = name;
    item.mode = "1";
    item.args.assign(args.begin(), args.end());
    item.aborts = false;
    item.code = code;
    item.out = out;
    item.err = err;
    return item;
}

flag_case aborts(const char* mode, std::initializer_list<const char*> args, const char* diagnostic) {
    flag_case item;
    item.name = mode;
    item.mode = mode;
    item.args.assign(args.begin(), args.end());
    item.aborts = true;
    item.code = 0;
    item.err = diagnostic;
    return item;
}

const char root_help[] =
    "Usage: flag_test [options] <command>\n"
    "\n"
    "Commands:\n"
    "  admin   manage users\n"
    "  fail    exit with code 5\n"
    "  status  show the service status\n"
    "\n"
    "Options:\n"
    "      --retries <int>  how many times to retry (default 3)\n"
    "  -v, --verbose        print more\n"
    "  -h, --help           show this help\n";

const char admin_help[] =
    "Usage: flag_test admin [options] <command>\n"
    "\n"
    "manage users\n"
    "\n"
    "Commands:\n"
    "  register  register a user\n"
    "  remove    remove the users given as arguments\n"
    "  role      change what a user may do\n"
    "\n"
    "Options:\n"
    "      --retries <int>  how many times to retry (default 3)\n"
    "  -v, --verbose        print more\n"
    "  -h, --help           show this help\n";

const char register_help[] =
    "Usage: flag_test admin register [options]\n"
    "\n"
    "register a user\n"
    "\n"
    "Options:\n"
    "      --admin           give the user admin rights\n"
    "  -e, --email <string>  email address\n"
    "  -n, --name <string>   user name\n"
    "      --retries <int>   how many times to retry (default 3)\n"
    "  -v, --verbose         print more\n"
    "  -h, --help            show this help\n";

const char status_help[] =
    "Usage: flag_test status [options]\n"
    "\n"
    "show the service status\n"
    "\n"
    "Options:\n"
    "      --retries <int>  how many times to retry (default 3)\n"
    "  -v, --verbose        print more\n"
    "  -h, --help           show this help\n";

const char grant_help[] =
    "Usage: flag_test admin role grant [options]\n"
    "\n"
    "give a user a role\n"
    "\n"
    "Options:\n"
    "      --retries <int>  how many times to retry (default 3)\n"
    "  -r, --role <string>  the role to give (default \"reader\")\n"
    "  -v, --verbose        print more\n"
    "  -h, --help           show this help\n";

std::vector<flag_case> all_cases() {
    std::vector<flag_case> cases;

    // 帮助
    cases.push_back(exits("root_help", {"--help"}, 0, root_help, ""));
    cases.push_back(exits("root_help_short", {"-h"}, 0, root_help, ""));
    cases.push_back(exits("group_help", {"admin", "--help"}, 0, admin_help, ""));
    cases.push_back(exits("command_help", {"admin", "register", "--help"}, 0, register_help, ""));
    cases.push_back(exits("command_help_after_options", {"admin", "register", "-n", "bob", "-h"}, 0, register_help,
                          ""));
    cases.push_back(exits("nested_command_help", {"admin", "role", "grant", "--help"}, 0, grant_help, ""));
    // 命令的代码跑起来之前，帮助已经打印、错误已经报了：status 一行都没打印
    cases.push_back(exits("help_before_the_command_runs", {"status", "--help"}, 0, status_help, ""));
    cases.push_back(exits("error_before_the_command_runs", {"status", "--verbos"}, 2, "",
                          "flag_test status: unknown option --verbos\n"
                          "Run 'flag_test status --help' for usage.\n"));

    // 选中命令、读选项
    cases.push_back(exits("long_and_short", {"admin", "register", "--name=alice", "-e", "alice@example.com", "--admin"},
                          0, "register name=alice email=alice@example.com admin=1 verbose=0\n", ""));
    cases.push_back(exits("global_before_command", {"-v", "admin", "register", "-n", "bob"}, 0,
                          "register name=bob email= admin=0 verbose=1\n", ""));
    cases.push_back(exits("global_after_command", {"admin", "register", "--name", "bob", "--verbose"}, 0,
                          "register name=bob email= admin=0 verbose=1\n", ""));
    cases.push_back(exits("last_one_wins", {"admin", "register", "-n", "first", "--name=second"}, 0,
                          "register name=second email= admin=0 verbose=0\n", ""));
    cases.push_back(exits("positional", {"admin", "remove", "alice", "-v", "bob", "--", "--carol", "-"}, 0,
                          "remove alice\nremove bob\nremove --carol\nremove -\n", ""));
    cases.push_back(exits("nested_group", {"admin", "role", "grant", "-r", "writer", "alice", "bob"}, 0,
                          "grant writer to alice\ngrant writer to bob\n", ""));
    cases.push_back(exits("string_default", {"admin", "role", "grant", "alice"}, 0, "grant reader to alice\n", ""));
    cases.push_back(exits("defaults", {"status"}, 0, "status verbose=0 retries=3\n", ""));
    cases.push_back(exits("single_dash_long_name", {"status", "--retries=-4", "-verbose"}, 0,
                          "status verbose=1 retries=-4\n", ""));
    cases.push_back(exits("value_starting_with_dash", {"status", "--retries", "-5"}, 0,
                          "status verbose=0 retries=-5\n", ""));
    cases.push_back(exits("explicit_booleans", {"status", "-v=true", "--verbose=false", "--retries=+7"}, 0,
                          "status verbose=0 retries=7\n", ""));
    cases.push_back(exits("smallest_integer", {"status", "--retries=-9223372036854775808"}, 0,
                          "status verbose=0 retries=-9223372036854775808\n", ""));
    cases.push_back(exits("exit_code", {"fail"}, 5, "", ""));
    // UTF-8：Windows 上这一条走 GetCommandLineW，"张三" 不在 runner 的代码页里
    cases.push_back(exits("utf8_argument", {"admin", "register", "--name=\xe5\xbc\xa0\xe4\xb8\x89"}, 0,
                          "register name=\xe5\xbc\xa0\xe4\xb8\x89 email= admin=0 verbose=0\n", ""));
    // 空格、引号、结尾的反斜杠：Windows 上查拆命令行的规则
    cases.push_back(exits("quoting", {"admin", "remove", "a b", "c\"d", "e\\", ""}, 0,
                          "remove a b\nremove c\"d\nremove e\\\nremove \n", ""));

    // 参数不对
    cases.push_back(exits("missing_command", {}, 2, "",
                          "flag_test: missing command\nRun 'flag_test --help' for usage.\n"));
    cases.push_back(exits("missing_subcommand", {"admin"}, 2, "",
                          "flag_test admin: missing command\nRun 'flag_test admin --help' for usage.\n"));
    cases.push_back(exits("double_dash_instead_of_command", {"--", "admin"}, 2, "",
                          "flag_test: missing command\nRun 'flag_test --help' for usage.\n"));
    cases.push_back(exits("unknown_command", {"admin", "regster"}, 2, "",
                          "flag_test admin: unknown command regster\nRun 'flag_test admin --help' for usage.\n"));
    cases.push_back(exits("command_option_before_command", {"--name=alice", "admin", "register"}, 2, "",
                          "flag_test: unknown option --name\nRun 'flag_test --help' for usage.\n"));
    cases.push_back(exits("unknown_option", {"admin", "register", "--nmae=alice"}, 2, "",
                          "flag_test admin register: unknown option --nmae\n"
                          "Run 'flag_test admin register --help' for usage.\n"));
    cases.push_back(exits("missing_value", {"status", "--retries"}, 2, "",
                          "flag_test status: option --retries needs a value\n"
                          "Run 'flag_test status --help' for usage.\n"));
    cases.push_back(exits("not_an_integer", {"status", "--retries", "5x"}, 2, "",
                          "flag_test status: invalid value \"5x\" for option --retries: not an integer\n"
                          "Run 'flag_test status --help' for usage.\n"));
    cases.push_back(exits("integer_out_of_range", {"status", "--retries=9223372036854775808"}, 2, "",
                          "flag_test status: invalid value \"9223372036854775808\" for option --retries: out of range\n"
                          "Run 'flag_test status --help' for usage.\n"));
    cases.push_back(exits("not_a_boolean", {"status", "-v=yes"}, 2, "",
                          "flag_test status: invalid value \"yes\" for option -v: not true or false\n"
                          "Run 'flag_test status --help' for usage.\n"));
    cases.push_back(exits("help_with_value", {"status", "--help=me"}, 2, "",
                          "flag_test status: option --help does not take a value\n"
                          "Run 'flag_test status --help' for usage.\n"));

    // 误用
    cases.push_back(aborts("read_before_init", {}, "cu::flag: --verbose read before cu::flag::init()"));
    cases.push_back(aborts("args_before_init", {}, "cu::flag: args() called before cu::flag::init()"));
    cases.push_back(aborts("duplicate_option", {}, "cu::flag: two options are named \"verbose\""));
    cases.push_back(aborts("reserved_name", {}, "cu::flag: option name \"h\" is taken by -h"));
    cases.push_back(aborts("bad_option_name", {}, "cu::flag: option name \"-x\" must not start with '-'"));
    cases.push_back(aborts("repeated_name", {}, "cu::flag: option -x lists the name \"x\" twice"));
    cases.push_back(aborts("duplicate_command", {"status"}, "cu::flag: two commands are named \"status\""));
    cases.push_back(aborts("duplicate_subcommand", {},
                           "cu::flag: command \"group\" has two subcommands named \"a\""));
    cases.push_back(aborts("bad_command_name", {},
                           "cu::flag: command name \"bad name\" must not contain '=' or spaces"));
    cases.push_back(aborts("declared_in_function", {"late"},
                           "cu::flag: --late declared after the command line was parsed"));
    cases.push_back(aborts("option_named_like_global", {}, "cu::flag: two options are named \"verbose\""));
    cases.push_back(aborts("options_of_a_command_share_a_name", {}, "cu::flag: two options are named \"x\""));
    cases.push_back(aborts("init_twice", {"again"}, "cu::flag: init() called twice"));
    cases.push_back(aborts("command_after_init", {"later"},
                           "cu::flag: command \"inner\" declared after cu::flag::init()"));
    return cases;
}

// ---- 子进程 ----------------------------------------------------------------------------------------

struct child_outcome {
    bool aborted;
    bool exited;
    int code;
    std::string how;   // 既没正常退出也没终止时，子进程是怎么结束的
    std::string out;
    std::string err;
};

#if defined(_WIN32)

const wchar_t child_variable[] = L"CU_FLAG_TEST_CHILD";

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = static_cast<int>(text.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, NULL, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, &wide[0], needed);
    return wide;
}

// 给一个参数加引号，按 C 运行库拆命令行的规则能原样拆回来。
std::wstring quote(const std::wstring& word) {
    if (!word.empty() && word.find_first_of(L" \t\"") == std::wstring::npos) {
        return word;
    }
    std::wstring quoted = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < word.size() && word[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == word.size()) {
            quoted.append(backslashes * 2, L'\\');   // 后面跟着收尾的引号，反斜杠都要翻倍
            break;
        }
        if (word[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted += word[i];
    }
    quoted += L'"';
    return quoted;
}

std::string read_all(HANDLE pipe) {
    std::string text;
    char buffer[4096];
    DWORD got = 0;
    while (::ReadFile(pipe, buffer, sizeof buffer, &got, NULL) && got != 0) {
        text.append(buffer, got);
    }
    return text;
}

// 子进程的 stdout 是文本模式，换行是 \r\n。
std::string without_carriage_returns(const std::string& text) {
    std::string kept;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') {
            kept += text[i];
        }
    }
    return kept;
}

child_outcome run_child(const flag_case& item) {
    child_outcome outcome;
    outcome.aborted = false;
    outcome.exited = false;
    outcome.code = 0;

    SECURITY_ATTRIBUTES inherit;
    inherit.nLength = sizeof inherit;
    inherit.lpSecurityDescriptor = NULL;
    inherit.bInheritHandle = TRUE;
    // 管道开到 1 MiB：先读完 stdout 再读 stderr，子进程写 stderr 时不会因为管道满了卡住
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    HANDLE err_read = NULL;
    HANDLE err_write = NULL;
    if (!::CreatePipe(&out_read, &out_write, &inherit, 1 << 20)) {
        outcome.how = "CreatePipe failed";
        return outcome;
    }
    if (!::CreatePipe(&err_read, &err_write, &inherit, 1 << 20)) {
        ::CloseHandle(out_read);
        ::CloseHandle(out_write);
        outcome.how = "CreatePipe failed";
        return outcome;
    }
    ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring command = quote(std::wstring(exe, length));
    for (std::size_t i = 0; i < item.args.size(); ++i) {
        command += L' ';
        command += quote(widen(item.args[i]));
    }

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof process);
    ::SetEnvironmentVariableW(child_variable, widen(item.mode).c_str());
    const BOOL started = ::CreateProcessW(exe, &command[0], NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    ::SetEnvironmentVariableW(child_variable, NULL);
    ::CloseHandle(out_write);
    ::CloseHandle(err_write);
    if (!started) {
        ::CloseHandle(out_read);
        ::CloseHandle(err_read);
        outcome.how = "CreateProcessW failed";
        return outcome;
    }

    outcome.out = without_carriage_returns(read_all(out_read));
    outcome.err = without_carriage_returns(read_all(err_read));
    ::CloseHandle(out_read);
    ::CloseHandle(err_read);

    DWORD code = 0;
    if (::WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 99);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        outcome.how = "did not end within 10 s";
    } else if (!::GetExitCodeProcess(process.hProcess, &code)) {
        outcome.how = "no exit code";
    } else if (code == 3) {
        outcome.aborted = true;   // abort() 以 3 退出，见 prepare_child；所以被测的程序不用 3 当退出码
    } else {
        outcome.exited = true;
        outcome.code = static_cast<int>(code);
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return outcome;
}

void prepare_child() {
    // abort 不弹窗、不写错误报告
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // UCRT（MSVC 和 MinGW 的 UCRT64）的 abort 默认报告错误、以快速失败 0xC0000409 结束；关掉以后以 3 退出
#  if defined(_MSC_VER) || defined(_UCRT)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#  endif
}

// 子进程从环境变量知道自己是子进程，也知道要怎么误用。
std::string child_mode() {
    wchar_t value[64];
    const DWORD length = ::GetEnvironmentVariableW(child_variable, value, 64);
    if (length == 0 || length >= 64) {
        return std::string();
    }
    std::string mode;
    for (DWORD i = 0; i < length; ++i) {
        mode += static_cast<char>(value[i]);   // 模式名都是 ASCII
    }
    return mode;
}

#else

std::string read_all(int fd) {
    std::string text;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got > 0) {
            text.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return text;
    }
}

// fork 出来的子进程直接调 child_main，不再 exec；argv 照用例拼。输出都很短，管道装得下：先读完 stdout 再读 stderr
// 不会卡住，卡住了也有 alarm 兜底。
child_outcome run_child(const flag_case& item) {
    child_outcome outcome;
    outcome.aborted = false;
    outcome.exited = false;
    outcome.code = 0;

    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0) {
        outcome.how = "pipe failed";
        return outcome;
    }
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        outcome.how = "pipe failed";
        return outcome;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        outcome.how = "fork failed";
        return outcome;
    }
    if (child == 0) {
        // 核心转储的上限设成 1 字节：设成 0 拦不住交给管道程序的转储（core(5)）
        struct rlimit limit = {1, 1};
        ::setrlimit(RLIMIT_CORE, &limit);
        ::alarm(10);
        ::dup2(out_pipe[1], 1);
        ::dup2(err_pipe[1], 2);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("flag_test"));
        for (std::size_t i = 0; i < item.args.size(); ++i) {
            argv.push_back(const_cast<char*>(item.args[i].c_str()));
        }
        argv.push_back(0);
        std::exit(child_main(item.mode, static_cast<int>(argv.size() - 1), &argv[0]));
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    outcome.out = read_all(out_pipe[0]);
    outcome.err = read_all(err_pipe[0]);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int ended = 0;
    while (::waitpid(child, &ended, 0) < 0 && errno == EINTR) {
    }
    if (WIFSIGNALED(ended) && WTERMSIG(ended) == SIGABRT) {
        outcome.aborted = true;
    } else if (WIFEXITED(ended)) {
        outcome.exited = true;
        outcome.code = WEXITSTATUS(ended);
    } else {
        char text[64];
        std::snprintf(text, sizeof text, "killed by signal %d", WIFSIGNALED(ended) ? WTERMSIG(ended) : -1);
        outcome.how = text;
    }
    return outcome;
}

void prepare_child() {}

std::string child_mode() {
    const char* const value = std::getenv("CU_FLAG_TEST_CHILD");
    return value ? std::string(value) : std::string();
}

#endif

// ---- 比对 ------------------------------------------------------------------------------------------

std::string describe_code(const child_outcome& outcome) {
    if (outcome.aborted) {
        return "aborted";
    }
    if (outcome.exited) {
        return "exited with code " + std::to_string(outcome.code);
    }
    return outcome.how;
}

// 和预期一样时返回空串，否则说明哪里不一样。
std::string check(const flag_case& item, const child_outcome& outcome) {
    if (item.aborts) {
        if (!outcome.aborted) {
            return "should have aborted, but " + describe_code(outcome);
        }
        if (outcome.err.find(item.err) == std::string::npos) {
            return "aborted without the expected diagnostic: " + item.err;
        }
        return std::string();
    }
    if (!outcome.exited || outcome.code != item.code) {
        return "should have exited with code " + std::to_string(item.code) + ", but " + describe_code(outcome);
    }
    if (outcome.out != item.out) {
        return "stdout differs; expected:\n" + item.out;
    }
    if (outcome.err != item.err) {
        return "stderr differs; expected:\n" + item.err;
    }
    return std::string();
}

int run_all() {
    const std::vector<flag_case> cases = all_cases();
    int passed = 0;
    int failed = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const flag_case& item = cases[i];
        const child_outcome outcome = run_child(item);
        const std::string problem = check(item, outcome);
        if (problem.empty()) {
            std::printf("[       OK ] flag.%s\n", item.name.c_str());
            ++passed;
            continue;
        }
        ++failed;
        std::printf("[  FAILED  ] flag.%s: %s\n", item.name.c_str(), problem.c_str());
        std::printf("    child stdout:\n%s\n    child stderr:\n%s\n", outcome.out.c_str(), outcome.err.c_str());
    }
    if (failed != 0) {
        std::printf("[  FAILED  ] %d tests\n", failed);
        return 1;
    }
    std::printf("[  PASSED  ] %d tests.\n", passed);
    return 0;
}

int main(int argc, char* argv[]) {
    const std::string mode = child_mode();
    if (mode.empty()) {
        return run_all();
    }
    prepare_child();
    return child_main(mode, argc, argv);
}
