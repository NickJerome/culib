/* rand_s 需要在包含任何 CRT 头之前定义（MSVC 与 MinGW-w64 皆然） */
#define _CRT_RAND_S
#if defined(_WIN32)
/* ---------------------------------------------------------------- Windows */
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  define PSAPI_VERSION 2
#  include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
/* ------------------------------------------------------------- POSIX/Unix */
#  include <fcntl.h>
#  include <errno.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <pthread.h>
#  include <time.h>
#  if defined(__APPLE__)
#    include <mach/mach.h>
#  endif
#else
#  error "culib mem: unsupported platform (add a branch in src/mem/allocator/os.cpp)"
#endif

#include "os.h"
#include "../pages.h"

#include <chrono>
#include <cstdlib>

namespace wavvix { namespace mem { namespace detail {

#if defined(_WIN32)
/* ================================ Windows =============================== */

size_t mem_os_process_rss(void) {
  PROCESS_MEMORY_COUNTERS pmc;
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return (size_t)pmc.WorkingSetSize;
}

bool mem_os_random_buf(void* buf, size_t n) {
  uint8_t* b = (uint8_t*)buf;
  while (n > 0) {
    unsigned int v = 0;
    if (rand_s(&v) != 0) return false;
    size_t k = n < sizeof(v) ? n : sizeof(v);
    for (size_t i = 0; i < k; i++) b[i] = (uint8_t)(v >> (8 * i));
    b += k; n -= k;
  }
  return true;
}

void mem_os_write_stderr(const char* msg) {
  if (msg == NULL) return;
  DWORD len = 0;
  while (msg[len] != '\0') len++;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == NULL || h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(h, msg, len, &written, NULL);
}

static DWORD g_fls_index = FLS_OUT_OF_INDEXES;
static DWORD g_exiting_index = TLS_OUT_OF_INDEXES;
static mem_os_thread_exit_fn g_exit_callback = NULL;

static VOID CALLBACK mem_thread_exit(PVOID value) {
  // MinGW may already have destroyed emulated C++ TLS; use native OS storage.
  if (!TlsSetValue(g_exiting_index, reinterpret_cast<void*>(1))) std::abort();
  g_exit_callback(value);
}

bool mem_os_thread_exit_init(mem_os_thread_exit_fn fn) {
  g_exiting_index = TlsAlloc();
  if (g_exiting_index == TLS_OUT_OF_INDEXES) return false;
  g_exit_callback = fn;
  g_fls_index = FlsAlloc(&mem_thread_exit);
  if (g_fls_index == FLS_OUT_OF_INDEXES) {
    TlsFree(g_exiting_index);
    g_exiting_index = TLS_OUT_OF_INDEXES;
  }
  return g_fls_index != FLS_OUT_OF_INDEXES;
}

bool mem_os_thread_exit_set(void* value) {
  if (g_fls_index == FLS_OUT_OF_INDEXES) return false;
  if (value != NULL && TlsGetValue(g_exiting_index) != NULL) return false;
  return FlsSetValue(g_fls_index, value) != 0;
}

void mem_os_atfork_register(mem_os_fork_handler_t prepare,
                                      mem_os_fork_handler_t parent,
                                      mem_os_fork_handler_t child) {
  /* Windows 没有 fork，无需钩子。 */
  (void)prepare; (void)parent; (void)child;
}

#else
/* ================================== POSIX =============================== */

static int mem_open_readonly(const char* path) {
#if defined(O_CLOEXEC)
  return open(path, O_RDONLY | O_CLOEXEC);
#else
  int fd = open(path, O_RDONLY);
  if (fd >= 0) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
#endif
}

size_t mem_os_process_rss(void) {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
    return 0;
  return static_cast<size_t>(info.resident_size);
#else
  /* /proc/self/statm 第二个字段是常驻页数。无分配读取：固定栈缓冲。 */
  int fd = mem_open_readonly("/proc/self/statm");
  if (fd < 0) return 0;
  char buf[64];
  ssize_t n;
  do { n = read(fd, buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = '\0';
  const char* c = buf;
  while (*c != ' ' && *c != '\0') c++;   /* 跳过第一个字段（总虚拟页数） */
  while (*c == ' ') c++;
  size_t resident_pages = 0;
  while (*c >= '0' && *c <= '9') { resident_pages = resident_pages * 10 + (size_t)(*c - '0'); c++; }
  return resident_pages * pages::page_size();
#endif
}

bool mem_os_random_buf(void* buf, size_t n) {
  uint8_t* b = (uint8_t*)buf;
  size_t got = 0;
  int fd = mem_open_readonly("/dev/urandom");
  if (fd < 0) return false;
  while (got < n) {
    ssize_t r = read(fd, b + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  return got == n;
}

void mem_os_write_stderr(const char* msg) {
  if (msg == NULL) return;
  size_t len = 0;
  while (msg[len] != '\0') len++;
  ssize_t r = write(STDERR_FILENO, msg, len);
  (void)r;
}

static pthread_key_t g_texit_key;
static bool          g_texit_ready = false;
static thread_local bool g_thread_exiting = false;
static mem_os_thread_exit_fn g_exit_callback = NULL;

static void mem_thread_exit(void* value) {
  g_thread_exiting = true;
  g_exit_callback(value);
}

bool mem_os_thread_exit_init(mem_os_thread_exit_fn fn) {
  g_exit_callback = fn;
  if (pthread_key_create(&g_texit_key, mem_thread_exit) != 0) return false;
  g_texit_ready = true;
  return true;
}

bool mem_os_thread_exit_set(void* value) {
  if (!g_texit_ready) return false;
  if (value != NULL && g_thread_exiting) return false;
  return pthread_setspecific(g_texit_key, value) == 0;
}

void mem_os_atfork_register(mem_os_fork_handler_t prepare,
                                      mem_os_fork_handler_t parent,
                                      mem_os_fork_handler_t child) {
  pthread_atfork(prepare, parent, child);
}

#endif /* _WIN32 */

/* ------------------------- 平台无关的公共部分 --------------------------- */

int64_t mem_os_now_ms(void) {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(
      steady_clock::now().time_since_epoch()).count();
}

void mem_os_fatal(const char* line) {
  /* 先解除线程退出钩子的武装：我们正因为检测到堆损坏而中止，abort() 之后
   * Windows 的 ExitProcess 会执行本线程的 FLS 回调，那个回调会去冲刷 tcache、
   * 归还页——在一个已知损坏的堆上做这些事，轻则把退出码从 3 改写成
   * 0xC0000005（实测），重则在诊断信息之后再叠一次崩溃。置空即让回调直接返回。 */
  mem_os_thread_exit_set(NULL);
  mem_os_write_stderr("culib mem: fatal: ");
  mem_os_write_stderr(line != NULL ? line : "(null)");
  mem_os_write_stderr("\n");
  std::abort();
}

} } } // namespace wavvix::mem::detail
