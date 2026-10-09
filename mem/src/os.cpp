/* rand_s 需要在包含任何 CRT 头之前定义（MSVC 与 MinGW-w64 皆然） */
#define _CRT_RAND_S
/* ----------------------------------------------------------------------------
 * os.cpp — OS 抽象层实现（Linux / macOS / Windows）
 *
 * 平台头文件只在本文件包含：os.h 仅有接口声明，不把 windows.h / sys/mman.h
 * 泄漏给其余 15 个源文件。编译器差异见 compiler.h。
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
/* ---------------------------------------------------------------- Windows */
#  define WIN32_LEAN_AND_MEAN
#  if !defined(_WIN32_WINNT)
#    define _WIN32_WINNT 0x0600   /* FlsAlloc 及其回调需要 Vista+ */
#  endif
#  define NOMINMAX
#  include <windows.h>
#  define PSAPI_VERSION 2
#  include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
/* ------------------------------------------------------------- POSIX/Unix */
#  include <sys/mman.h>
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
#  error "culib mem: unsupported platform (add a branch in src/os.cpp)"
#endif

#include "os.h"

#include <chrono>
#include <cstdlib>

#if defined(_WIN32)
/* ================================ Windows =============================== */

bool mem_os_reserve(size_t size, void** out) {
  /* MEM_RESERVE 天然按分配粒度（64KiB）对齐，正好等于 WAVVIX_MEM_OS_RESERVE_ALIGN */
  void* p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
  if (p == NULL) return false;
  *out = p;
  return true;
}

bool mem_os_commit(void* p, size_t size) {
  return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

bool mem_os_decommit(void* p, size_t size) {
  return VirtualFree(p, size, MEM_DECOMMIT) != 0;
}

bool mem_os_release(void* p, size_t size) {
  (void)size; /* MEM_RELEASE 要求 size 为 0，整片释放 */
  return VirtualFree(p, 0, MEM_RELEASE) != 0;
}

bool mem_os_protect(void* p, size_t size, bool rw) {
  DWORD old = 0;
  return VirtualProtect(p, size, rw ? PAGE_READWRITE : PAGE_NOACCESS, &old) != 0;
}

size_t mem_os_page_size(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}

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

bool mem_os_thread_exit_init(mem_os_thread_exit_fn fn) {
  /* FLS 回调在线程退出时执行，对 std::thread 创建的线程同样有效——这正是
   * DllMain 的 TLS 回调（静态库里根本没有）和 thread_local 析构做不到的。 */
  g_fls_index = FlsAlloc((PFLS_CALLBACK_FUNCTION)fn);
  return g_fls_index != FLS_OUT_OF_INDEXES;
}

bool mem_os_thread_exit_set(void* value) {
  if (g_fls_index == FLS_OUT_OF_INDEXES) return false;
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

bool mem_os_reserve(size_t size, void** out) {
  /* mmap 只保证页对齐：超额 WAVVIX_MEM_OS_RESERVE_ALIGN 后裁掉两端，得到 64KiB 对齐。 */
  const size_t over = size + WAVVIX_MEM_OS_RESERVE_ALIGN;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if !defined(__APPLE__)
  flags |= MAP_NORESERVE;
#endif
  uint8_t* p = (uint8_t*)mmap(NULL, over, PROT_NONE, flags, -1, 0);
  if (p == MAP_FAILED) return false;
  uintptr_t raw = (uintptr_t)p;
  uintptr_t aligned = (raw + WAVVIX_MEM_OS_RESERVE_ALIGN - 1) & ~(uintptr_t)(WAVVIX_MEM_OS_RESERVE_ALIGN - 1);
  size_t head = (size_t)(aligned - raw);
  size_t tail = over - head - size;
  if (head > 0) munmap(p, head);
  if (tail > 0) munmap((void*)(aligned + size), tail);
  *out = (void*)aligned;
  return true;
}

bool mem_os_commit(void* p, size_t size) {
  return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}

bool mem_os_decommit(void* p, size_t size) {
#if defined(__APPLE__)
  // 重新映射为不可访问的匿名页；下次 commit 后由内核保证内容为零。
  return mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
              -1, 0) == p;
#else
  /* madvise DONTNEED：内容作废、物理页归还内核，映射保持 RW，
   * 下次访问得到零页——这正是"免清零传递"（free_is_zero）的内核依据。 */
  if (madvise(p, size, MADV_DONTNEED) != 0) return false;
#if defined(WAVVIX_MEM_DEBUG)
  /* 契约是"再访问前必须 commit"，但 madvise 之后映射仍可读写、访问只会
   * 静默拿到零页。Windows 的 MEM_DECOMMIT 会让同样的契约违反直接崩溃——
   * 调试构建下补一道 PROT_NONE，使"忘记 commit 就访问"在两个平台上都是
   * 确定性崩溃，而不是只有 Windows CI 才暴露。 */
  return mprotect(p, size, PROT_NONE) == 0;
#else
  return true;
#endif
#endif
}

bool mem_os_release(void* p, size_t size) {
  return munmap(p, size) == 0;
}

bool mem_os_protect(void* p, size_t size, bool rw) {
  return mprotect(p, size, rw ? (PROT_READ | PROT_WRITE) : PROT_NONE) == 0;
}

size_t mem_os_page_size(void) {
  return (size_t)sysconf(_SC_PAGESIZE);
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
  size_t pages = 0;
  while (*c >= '0' && *c <= '9') { pages = pages * 10 + (size_t)(*c - '0'); c++; }
  return pages * mem_os_page_size();
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

bool mem_os_thread_exit_init(mem_os_thread_exit_fn fn) {
  if (pthread_key_create(&g_texit_key, fn) != 0) return false;
  g_texit_ready = true;
  return true;
}

bool mem_os_thread_exit_set(void* value) {
  if (!g_texit_ready) return false;
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
