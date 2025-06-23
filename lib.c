/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE // 定义 _GNU_SOURCE 以启用 GNU 特有的函数和宏

// headers {{{ // 头文件区域
#include "lib.h"
#include "ctypes.h" // 自定义类型
#include <assert.h> // 断言宏
#include <execinfo.h> // 用于获取函数调用回溯信息
#include <math.h> // 数学函数
#include <netdb.h> // 网络数据库操作，如 gethostbyname
#include <sched.h> // 调度相关的函数，如 sched_setaffinity
#include <signal.h> // 信号处理
#include <sys/socket.h> // 套接字编程接口
#include <poll.h> // I/O 多路复用 poll 函数
#include <sys/ioctl.h> // I/O 控制操作
#include <time.h> // 时间和日期函数
#include <stdarg.h> // 可变参数列表宏 (va_start, va_end 等)

#if defined(__linux__) // Linux 特定头文件
#include <linux/fs.h> // 文件系统相关的定义，如 BLKGETSIZE64
#include <malloc.h>  // malloc_usable_size 函数
#elif defined(__APPLE__) && defined(__MACH__) // macOS 特定头文件
#include <sys/disk.h> // 磁盘操作，如 DKIOCGETBLOCKSIZE
#include <malloc/malloc.h> // malloc_size 函数
#elif defined(__FreeBSD__) // FreeBSD 特定头文件
#include <sys/disk.h> // 磁盘操作，如 DIOCGMEDIASIZE
#include <malloc_np.h> // malloc_usable_size 等非标准 malloc 函数
#endif // OS

#if defined(__FreeBSD__)
#include <pthread_np.h> // FreeBSD 线程非标准扩展，如 pthread_set_name_np
#endif
// }}} headers

// math {{{ // 数学运算相关函数
  inline u64
mhash64(const u64 v) // 64位乘法哈希函数
{
  return v * 11400714819323198485lu; // 使用一个大的素数作为乘数
}

  inline u32
mhash32(const u32 v) // 32位乘法哈希函数
{
  return v * 2654435761u; // 使用一个黄金分割相关的素数作为乘数
}

// From Daniel Lemire's blog (2013, lemire.me)
// 参考自 Daniel Lemire 的博客 (2013, lemire.me)
  u64
gcd64(u64 a, u64 b) // 计算两个64位无符号整数的最大公约数 (Stein算法/二分GCD算法)
{
  if (a == 0) // 如果 a 为 0，最大公约数是 b
    return b;
  if (b == 0) // 如果 b 为 0，最大公约数是 a
    return a;

  const u32 shift = (u32)__builtin_ctzl(a | b); // 计算 a 和 b 公共的因子2的幂次 (即末尾0的个数的较小者)
  a >>= __builtin_ctzl(a); // 移除 a 末尾所有的因子2
  do {
    b >>= __builtin_ctzl(b); // 移除 b 末尾所有的因子2
    if (a > b) { // 确保 a <= b
      const u64 t = b;
      b = a;
      a = t;
    }
    b = b - a; //核心步骤：gcd(a,b) = gcd(a, b-a)
  } while (b); // 直到 b 变为 0
  return a << shift; // 将结果乘回之前移除的公共因子2的幂
}
// }}} math

// random {{{ // 随机数生成器相关
// Lehmer's generator is 2x faster than xorshift
// Lehmer 随机数生成器比 xorshift 快约2倍
/**
 * D. H. Lehmer, Mathematical methods in large-scale computing units.
 * Proceedings of a Second Symposium on Large Scale Digital Calculating
 * Machinery;
 * Annals of the Computation Laboratory, Harvard Univ. 26 (1951), pp. 141-146.
 *
 * P L'Ecuyer,  Tables of linear congruential generators of different sizes and
 * good lattice structure. Mathematics of Computation of the American
 * Mathematical
 * Society 68.225 (1999): 249-260.
 */
struct lehmer_u64 { // Lehmer 64位随机数生成器的状态结构
  union {
    u128 v128; // 使用128位整数存储状态，以进行乘法运算
    u64 v64[2]; // 或者看作两个64位整数
  };
};

static __thread struct lehmer_u64 rseed_u128 = {.v64 = {4294967291, 1549556881}}; // 线程局部随机数种子，赋予初始值

  static inline u64
lehmer_u64_next(struct lehmer_u64 * const s) // 生成下一个64位 Lehmer 随机数
{
  const u64 r = s->v64[1]; // 当前状态的低64位（或高64位，取决于实现）作为本次随机数结果
  s->v128 *= 0xda942042e4dd58b5lu; // 状态乘以一个特定的128位乘数
  return r;
}

  static inline void
lehmer_u64_seed(struct lehmer_u64 * const s, const u64 seed) // 使用给定的64位种子初始化 Lehmer 生成器状态
{
  s->v128 = (((u128)(~seed)) << 64) | (seed | 1); // 构造128位初始状态，确保低位不为0
  (void)lehmer_u64_next(s); // 调用一次 next 来混合种子状态
}

  inline u64
random_u64(void) // 生成一个64位无符号随机整数
{
  return lehmer_u64_next(&rseed_u128); // 使用线程局部种子生成
}

  inline void
srandom_u64(const u64 seed) // 为当前线程的随机数生成器设置种子
{
  lehmer_u64_seed(&rseed_u128, seed);
}

  inline double
random_double(void) // 生成一个 [0.0, 1.0] 范围内的双精度随机浮点数
{
  // random between [0.0 - 1.0] // 生成 [0.0, 1.0] 之间的随机数
  const u64 r = random_u64(); // 获取一个64位随机整数
  return ((double)r) * (1.0 / ((double)(~0lu))); // 将其归一化到 [0.0, 1.0]
}
// }}} random

// timing {{{ // 时间相关函数
  inline u64
time_nsec(void) // 获取当前单调时间的纳秒级时间戳
{
  struct timespec ts;
  // MONO_RAW is 5x to 10x slower than MONO
  // CLOCK_MONOTONIC_RAW 比 CLOCK_MONOTONIC 慢5到10倍
  clock_gettime(CLOCK_MONOTONIC, &ts); // 使用 CLOCK_MONOTONIC 获取不受系统时间调整影响的时间
  return ((u64)ts.tv_sec) * 1000000000lu + ((u64)ts.tv_nsec); // 转换为纳秒
}

  inline double
time_sec(void) // 获取当前单调时间的秒级时间戳 (浮点数)
{
  const u64 nsec = time_nsec();
  return ((double)nsec) * 1.0e-9; // 纳秒转换为秒
}

  inline u64
time_diff_nsec(const u64 last) // 计算当前时间与上次时间的纳秒差
{
  return time_nsec() - last;
}

  inline double
time_diff_sec(const double last) // 计算当前时间与上次时间的秒差
{
  return time_sec() - last;
}

// need char str[64] // 调用者需要提供一个至少64字节的字符数组
  void
time_stamp(char * str, const size_t size) // 将当前本地时间格式化为字符串 "YYYY-MM-DD HH:MM:SS +ZZZZ"
{
  time_t now;
  struct tm nowtm;
  time(&now); // 获取当前日历时间
  localtime_r(&now, &nowtm); // 转换为本地时间结构 (线程安全)
  strftime(str, size, "%F %T %z", &nowtm); // 格式化时间字符串
}

  void
time_stamp2(char * str, const size_t size) // 将当前本地时间格式化为另一种字符串 "YYYY-MM-DD-HH-MM-SS+ZZZZ"
{
  time_t now;
  struct tm nowtm;
  time(&now);
  localtime_r(&now, &nowtm);
  strftime(str, size, "%F-%H-%M-%S%z", &nowtm); // 使用不同的格式化字符串
}
// }}} timing

// cpucache {{{ // CPU缓存及内存操作相关
  inline void
cpu_pause(void) // CPU暂停指令，用于自旋等待时降低功耗和总线竞争
{
#if defined(__x86_64__)
  _mm_pause(); // x86_64 平台使用 _mm_pause() (PAUSE 指令)
#elif defined(__aarch64__)
  // nop // AArch64 平台，通常是空操作或 YIELD 指令 (此处为空)
#endif
}

  inline void
cpu_mfence(void) // 内存屏障 (Memory Fence)，确保所有之前的内存写操作完成并对其他CPU可见
{
  atomic_thread_fence(MO_SEQ_CST); // 使用C11原子操作实现顺序一致性屏障
}

// compiler fence // 编译器屏障
  inline void
cpu_cfence(void) // 编译器屏障，防止编译器重排此点前后的内存访问指令
{
  atomic_thread_fence(MO_ACQ_REL); // 使用C11原子操作实现获取-释放屏障
}

  inline void
cpu_prefetch0(const void * const ptr) // 预取数据到缓存 (最低级别缓存，通常L1，用于读)
{
  __builtin_prefetch(ptr, 0, 0); // 0: 读操作, 0: 无特定时间局部性 (通常指最近的缓存)
}

  inline void
cpu_prefetch1(const void * const ptr) // 预取数据到缓存 (中等级别缓存，通常L2，用于读)
{
  __builtin_prefetch(ptr, 0, 1); // 0: 读操作, 1: 中等时间局部性
}

  inline void
cpu_prefetch2(const void * const ptr) // 预取数据到缓存 (较高级别缓存，通常L3，用于读)
{
  __builtin_prefetch(ptr, 0, 2); // 0: 读操作, 2: 较高时间局部性
}

  inline void
cpu_prefetch3(const void * const ptr) // 预取数据到缓存 (所有级别，用于读)
{
  __builtin_prefetch(ptr, 0, 3); // 0: 读操作, 3: 最高时间局部性
}

  inline void
cpu_prefetchw(const void * const ptr) // 预取数据到缓存 (用于写)
{
  __builtin_prefetch(ptr, 1, 0); // 1: 写操作, 0: 无特定时间局部性
}
// }}} cpucache

// crc32c {{{ // CRC32C 校验和计算 (通常使用硬件加速指令)
  inline u32
crc32c_u8(const u32 crc, const u8 v) // 计算单个字节的 CRC32C 增量值
{
#if defined(__x86_64__)
  return _mm_crc32_u8(crc, v); // x86_64 SSE4.2 指令
#elif defined(__aarch64__)
  return __crc32cb(crc, v); // AArch64 CRC32 指令 (byte)
#endif
}

  inline u32
crc32c_u16(const u32 crc, const u16 v) // 计算16位无符号整数的 CRC32C 增量值
{
#if defined(__x86_64__)
  return _mm_crc32_u16(crc, v); // x86_64 SSE4.2 指令
#elif defined(__aarch64__)
  return __crc32ch(crc, v); // AArch64 CRC32 指令 (halfword)
#endif
}

  inline u32
crc32c_u32(const u32 crc, const u32 v) // 计算32位无符号整数的 CRC32C 增量值
{
#if defined(__x86_64__)
  return _mm_crc32_u32(crc, v); // x86_64 SSE4.2 指令
#elif defined(__aarch64__)
  return __crc32cw(crc, v); // AArch64 CRC32 指令 (word)
#endif
}

  inline u32
crc32c_u64(const u32 crc, const u64 v) // 计算64位无符号整数的 CRC32C 增量值
{
#if defined(__x86_64__)
  return (u32)_mm_crc32_u64(crc, v); // x86_64 SSE4.2 指令
#elif defined(__aarch64__)
  return (u32)__crc32cd(crc, v); // AArch64 CRC32 指令 (doubleword)
#endif
}

  inline u32
crc32c_inc_123(const u8 * buf, u32 nr, u32 crc) // 增量计算1, 2或3字节数据的 CRC32C
{
  if (nr == 1) // 如果只有1字节
    return crc32c_u8(crc, buf[0]);

  crc = crc32c_u16(crc, *(u16 *)buf); // 先计算前2字节
  return (nr == 2) ? crc : crc32c_u8(crc, buf[2]); // 如果是2字节则返回，否则再计算第3字节
}

  inline u32
crc32c_inc_x4(const u8 * buf, u32 nr, u32 crc) // 增量计算数据块的 CRC32C，主要处理8字节和4字节对齐部分
{
  //debug_assert((nr & 3) == 0); // 假设 nr 是4的倍数 (注释掉了)
  const u32 nr8 = nr >> 3; // 计算有多少个8字节块
#pragma nounroll // 告诉编译器不要展开这个循环
  for (u32 i = 0; i < nr8; i++)
    crc = crc32c_u64(crc, ((u64*)buf)[i]); // 按64位处理

  if (nr & 4u) // 如果还有剩余的4字节 (即 nr 不是8的倍数，但可能是4的倍数)
    crc = crc32c_u32(crc, ((u32*)buf)[nr8<<1]); // 按32位处理剩余的4字节
  return crc;
}

  u32
crc32c_inc(const u8 * buf, u32 nr, u32 crc) // 增量计算任意长度数据块的 CRC32C
{
  crc = crc32c_inc_x4(buf, nr, crc); // 先处理对齐的部分 (8字节和4字节块)
  const u32 nr123 = nr & 3u; // 计算剩余的字节数 (0, 1, 2, 或 3)
  return nr123 ? crc32c_inc_123(buf + nr - nr123, nr123, crc) : crc; // 处理剩余的1, 2或3字节
}
// }}} crc32c

// debug {{{ // 调试相关功能
  void
debug_break(void) // 简单的调试断点函数，使程序暂停一小段时间
{
  usleep(100); // 休眠100微秒
}

static u64 * debug_watch_u64 = NULL; // 全局静态指针，用于在调试时监视一个u64变量的值

  static void
watch_u64_handler(const int sig) // SIGUSR1信号的处理函数，用于打印被监视的u64变量
{
  (void)sig; // 避免未使用参数警告
  const u64 v = debug_watch_u64 ? (*debug_watch_u64) : 0; // 获取被监视变量的值
  fprintf(stderr, "[USR1] %lu (0x%lx)\n", v, v); // 打印到标准错误输出
}

  void
watch_u64_usr1(u64 * const ptr) // 设置SIGUSR1信号处理，用于监视指定的u64变量
{
  debug_watch_u64 = ptr; // 将全局监视指针指向目标变量
  struct sigaction sa = {}; // sigaction结构体，用于设置信号处理方式
  sa.sa_handler = watch_u64_handler; // 设置信号处理函数
  sigemptyset(&(sa.sa_mask)); // 在处理函数执行期间不阻塞其他信号
  sa.sa_flags = SA_RESTART; // 如果信号中断了系统调用，则自动重启该系统调用
  if (sigaction(SIGUSR1, &sa, NULL) == -1) { // 注册信号处理函数
    fprintf(stderr, "Failed to set signal handler for SIGUSR1\n");
  } else {
    fprintf(stderr, "to watch> kill -s SIGUSR1 %d\n", getpid()); // 提示用户如何触发监视
  }
}

static void * debug_bt_state = NULL; // libbacktrace的状态指针
#if defined(BACKTRACE) && defined(__linux__) // 如果定义了BACKTRACE宏并且是Linux系统
// TODO: get exec path on MacOS and FreeBSD // TODO: 在MacOS和FreeBSD上获取可执行文件路径

#include <backtrace.h> // 包含libbacktrace库的头文件
static char debug_filepath[1024] = {}; // 存储当前可执行文件的路径

  static void
debug_bt_error_cb(void * const data, const char * const msg, const int errnum) // libbacktrace错误回调函数
{
  (void)data; // 避免未使用参数警告
  if (msg)
    dprintf(2, "libbacktrace: %s %s\n", msg, strerror(errnum)); // 打印错误信息到标准错误
}

  static int
debug_bt_print_cb(void * const data, const uintptr_t pc, // libbacktrace打印回调函数，用于打印每一帧回溯信息
    const char * const file, const int lineno, const char * const func)
{
  u32 * const plevel = (typeof(plevel))data; // data是指向堆栈级别的指针
  if (file || func || lineno) { // 如果有文件名、函数名或行号信息
    dprintf(2, "[%u]0x%012lx " TERMCLR(35) "%s" TERMCLR(31) ":" TERMCLR(34) "%d" TERMCLR(0)" %s\n", // 打印详细信息
        *plevel, pc, file ? file : "???", lineno, func ? func : "???");
  } else if (pc) { // 如果只有程序计数器地址
    dprintf(2, "[%u]0x%012lx ??\n", *plevel, pc); // 打印地址
  }
  (*plevel)++; // 堆栈级别加一
  return 0; // 返回0表示继续处理下一帧
}

__attribute__((constructor)) // 标记为构造函数，在main函数执行前调用
  static void
debug_backtrace_init(void) // 初始化调试回溯功能
{
  const ssize_t len = readlink("/proc/self/exe", debug_filepath, 1023); // 读取当前进程的可执行文件路径
  // disable backtrace // 如果读取失败或路径过长，则禁用回溯
  if (len < 0 || len >= 1023)
    return;

  debug_filepath[len] = '\0'; // 添加字符串结束符
  debug_bt_state = backtrace_create_state(debug_filepath, 1, debug_bt_error_cb, NULL); // 创建libbacktrace状态
}
#endif // BACKTRACE

  static void
debug_wait_gdb(void * const bt_state) // 打印回溯信息并等待GDB附加
{
  if (bt_state) { // 如果libbacktrace状态有效
#if defined(BACKTRACE) // 并且启用了BACKTRACE
    dprintf(2, "Backtrace :\n"); // 打印回溯信息头
    u32 level = 0; // 初始化堆栈级别
    backtrace_full(debug_bt_state, 1, debug_bt_print_cb, debug_bt_error_cb, &level); // 执行完整的回溯打印
#endif // BACKTRACE
  } else { // fallback to execinfo if no backtrace or initialization failed // 如果libbacktrace不可用或初始化失败，则回退到使用execinfo
    void *array[64]; // 存储回溯地址的数组
    const int size = backtrace(array, 64); // 获取回溯地址
    dprintf(2, "Backtrace (%d):\n", size - 1); // 打印回溯帧数 (排除当前函数)
    backtrace_symbols_fd(array + 1, size - 1, 2); // 将符号化的回溯信息输出到标准错误
  }

  abool v = true; // 原子布尔变量，控制等待循环，可被GDB修改以继续执行
  char timestamp[32];
  time_stamp(timestamp, 32); // 获取当前时间戳
  char threadname[32] = {};
  thread_get_name(pthread_self(), threadname, 32); // 获取当前线程名
  strcat(threadname, "(!!)"); // 在线程名后添加标记
  thread_set_name(pthread_self(), threadname); // 设置修改后的线程名 (便于GDB中识别)
  char hostname[32];
  gethostname(hostname, 32); // 获取主机名

  const char * const pattern = "[Waiting GDB] %1$s %2$s @ %3$s\n" // 提示信息格式
    "    Attach me: " TERMCLR(31) "sudo -Hi gdb -p %4$d" TERMCLR(0) "\n"; // 包含附加GDB的命令
  char buf[256];
  sprintf(buf, pattern, timestamp, threadname, hostname, getpid()); // 格式化提示信息
  write(2, buf, strlen(buf)); // 输出到标准错误

  // to continue: gdb> set var v = 0 // GDB中继续执行的命令提示
  // to kill from shell: $ kill %pid; kill -CONT %pid // 从shell中终止进程的命令提示

  // uncomment this line to surrender the shell on error
  // kill(getpid(), SIGSTOP); // stop burning cpu, once // 取消注释此行可以在出错时发送SIGSTOP信号暂停进程，避免CPU空转

  static au32 nr_waiting = 0; // 原子计数器，记录当前有多少线程在等待GDB
  const u32 seq = atomic_fetch_add_explicit(&nr_waiting, 1, MO_RELAXED); // 原子增加等待计数
  if (seq == 0) { // 如果是第一个进入等待的线程
    sprintf(buf, "/run/user/%u/.debug_wait_gdb_pid", getuid()); // 构造一个用于存储PID的文件路径
    const int pidfd = open(buf, O_CREAT|O_TRUNC|O_WRONLY, 00644); // 创建或打开该文件
    if (pidfd >= 0) {
      dprintf(pidfd, "%u", getpid()); // 将当前进程PID写入文件
      close(pidfd);
    }
  }

#pragma nounroll // 告诉编译器不要展开这个循环
  while (atomic_load_explicit(&v, MO_CONSUME)) // 循环等待，直到GDB将v修改为false
    sleep(1); // 每秒检查一次
}

#ifndef NDEBUG // 如果未定义NDEBUG (即调试模式开启)
  void
debug_assert(const bool v) // 自定义断言函数
{
  if (!v) // 如果断言条件为false
    debug_wait_gdb(debug_bt_state); // 调用debug_wait_gdb，打印回溯并等待GDB
}
#endif

__attribute__((noreturn)) // 声明此函数不会返回
  void
debug_die(void) // 致命错误处理函数
{
  debug_wait_gdb(debug_bt_state); // 打印回溯并等待GDB
  exit(0); // 退出程序
}

__attribute__((noreturn)) // 声明此函数不会返回
  void
debug_die_perror(void) // 打印errno对应的错误信息，然后调用debug_die
{
  perror(NULL); // 打印上一个系统调用失败的错误信息
  debug_die(); // 进入致命错误处理
}

#if !defined(NOSIGNAL) // 如果未定义NOSIGNAL (即启用信号处理)
// signal handler for wait_gdb on fatal errors // 用于处理致命错误的信号处理函数，它会调用debug_wait_gdb
  static void
wait_gdb_handler(const int sig, siginfo_t * const info, void * const context) // 信号处理函数原型
{
  (void)info; // 避免未使用参数警告
  (void)context; // 避免未使用参数警告
  char buf[64] = "[SIGNAL] ";
  strcat(buf, strsignal(sig)); // 获取信号名称字符串
  write(2, buf, strlen(buf)); // 打印接收到的信号信息
  debug_wait_gdb(NULL); // 调用debug_wait_gdb (不传递bt_state，将使用execinfo)
}

// setup hooks for catching fatal errors // 设置钩子以捕获致命错误
__attribute__((constructor)) // 构造函数，在main之前执行
  static void
debug_init(void) // 调试初始化，主要设置信号处理
{
  void * stack = pages_alloc_4kb(16); // 为信号处理函数分配备用栈 (16 * 4KB = 64KB)
  //fprintf(stderr, "altstack %p\n", stack);
  stack_t ss = {.ss_sp = stack, .ss_flags = 0, .ss_size = PGSZ*16}; // 定义备用栈结构
  if (sigaltstack(&ss, NULL)) // 设置备用信号栈
    fprintf(stderr, "sigaltstack failed\n"); // 如果失败则打印错误

  struct sigaction sa = {.sa_sigaction = wait_gdb_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK}; // 定义信号处理行为
                                                                                              // SA_SIGINFO: 使用sa_sigaction处理函数
                                                                                              // SA_ONSTACK: 在备用栈上执行处理函数
  sigemptyset(&(sa.sa_mask)); // 在处理函数执行期间不阻塞其他信号
  const int fatals[] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS, 0}; // 定义需要捕获的致命信号列表
  for (int i = 0; fatals[i]; i++) { // 遍历并为每个致命信号设置处理函数
    if (sigaction(fatals[i], &sa, NULL) == -1) {
      fprintf(stderr, "Failed to set signal handler for %s\n", strsignal(fatals[i]));
      fflush(stderr);
    }
  }
}

__attribute__((destructor)) // 析构函数，在程序退出时执行
  static void
debug_exit(void) // 调试退出清理函数
{
  // to get rid of valgrind warnings // 为了消除Valgrind关于备用栈未释放的警告
  stack_t ss = {.ss_flags = SS_DISABLE}; // 准备禁用备用栈
  stack_t oss = {}; // 用于接收旧的备用栈信息
  sigaltstack(&ss, &oss); // 禁用当前备用栈并获取其信息
  if (oss.ss_sp) // 如果之前设置了备用栈
    pages_unmap(oss.ss_sp, PGSZ * 16); // 解除映射并释放备用栈内存
}
#endif // !defined(NOSIGNAL)

  void
debug_dump_maps(FILE * const out) // 将当前进程的内存映射信息 (/proc/self/smaps) 转储到指定文件
{
  FILE * const in = fopen("/proc/self/smaps", "r"); // 只读方式打开smaps文件
  char * line0 = yalloc(1024); // 分配行缓冲区 (缓存行对齐)
  size_t size0 = 1024; // 缓冲区大小
  while (!feof(in)) { // 循环读取每一行
    const ssize_t r1 = getline(&line0, &size0, in); // 读取一行
    if (r1 < 0) break; // 读取失败或到文件末尾则跳出
    fprintf(out, "%s", line0); // 将行内容写入输出文件
  }
  fflush(out); // 刷新输出缓冲区
  fclose(in); // 关闭smaps文件
  // 注意：yalloc分配的内存 line0 在这里没有释放，可能导致内存泄漏。
  // 如果此函数会被多次调用，或者在长期运行的程序中，应考虑释放 line0。
  // 但通常调试函数可能不那么关注这点。
}

static pid_t perf_pid = 0; // 用于记录父进程中 perf record 的PID

#if defined(__linux__) // 仅Linux平台
__attribute__((constructor))
  static void
debug_perf_init(void) // 初始化perf检测，检查父进程是否为 "perf record"
{
  const pid_t ppid = getppid(); // 获取父进程PID
  char tmp[256] = {};
  sprintf(tmp, "/proc/%d/cmdline", ppid); // 构建父进程命令行文件路径
  FILE * const fc = fopen(tmp, "r"); // 打开文件
  if (!fc) return; // 打开失败则返回
  const size_t nr = fread(tmp, 1, sizeof(tmp) - 1, fc); // 读取命令行内容
  fclose(fc);
  // look for "perf record" // 查找 "perf record"
  if (nr < 12) // "perf record" 长度至少为11
    return;
  tmp[nr] = '\0'; // 添加字符串结束符
  for (u64 i = 0; i < nr; i++) // /proc/.../cmdline 中的参数是以'\0'分隔的
    if (tmp[i] == 0)
      tmp[i] = ' '; // 替换为 空格，便于strstr查找

  char * const perf = strstr(tmp, "perf record"); // 查找 "perf record"
  if (perf) { // 如果找到
    fprintf(stderr, "%s: perf detected\n", __func__);
    perf_pid = ppid; // 记录父进程 (perf) 的PID
  }
}
#endif // __linux__

  bool
debug_perf_switch(void) // 如果检测到perf，向perf进程发送SIGUSR2信号 (通常用于控制perf的开关)
{
  if (perf_pid > 0) { // 如果perf_pid有效
    kill(perf_pid, SIGUSR2); // 发送SIGUSR2信号
    return true;
  } else {
    return false;
  }
}
// }}} debug

// mm {{{ // 内存管理 (Memory Management)
#ifdef ALLOCFAIL // 如果定义了 ALLOCFAIL (用于模拟内存分配失败)
  bool
alloc_fail(void) // 随机决定是否模拟分配失败
{
#define ALLOCFAIL_RECP ((64lu)) // 失败概率的倒数 (即 1/64 的概率失败)
#define ALLOCFAIL_MAGIC ((ALLOCFAIL_RECP / 3lu)) // 一个特定的随机数结果，当匹配时触发失败
  return ((random_u64() % ALLOCFAIL_RECP) == ALLOCFAIL_MAGIC);
}

#ifdef MALLOCFAIL // 如果定义了 MALLOCFAIL (用于替换标准库的内存分配函数)
extern void * __libc_malloc(size_t size); // 声明libc的malloc函数
  void *
malloc(size_t size) // 自定义malloc，用于模拟失败
{
  if (alloc_fail()) // 如果模拟失败
    return NULL;
  return __libc_malloc(size); // 否则调用真正的malloc
}

extern void * __libc_calloc(size_t nmemb, size_t size); // 声明libc的calloc函数
  void *
calloc(size_t nmemb, size_t size) // 自定义calloc
{
  if (alloc_fail())
    return NULL;
  return __libc_calloc(nmemb, size);
}

extern void *__libc_realloc(void *ptr, size_t size); // 声明libc的realloc函数

  void *
realloc(void *ptr, size_t size) // 自定义realloc
{
  if (alloc_fail())
    return NULL;
  return __libc_realloc(ptr, size);
}
#endif // MALLOC_FAIL
#endif // ALLOC_FAIL

  void *
xalloc(const size_t align, const size_t size) // 分配指定对齐方式的内存
{
#ifdef ALLOCFAIL
  if (alloc_fail()) // 模拟分配失败
    return NULL;
#endif
  void * p;
  return (posix_memalign(&p, align, size) == 0) ? p : NULL; // 使用posix_memalign进行对齐分配
}

// alloc cache-line aligned address // 分配缓存行对齐的内存
  void *
yalloc(const size_t size) // 分配64字节对齐 (典型缓存行大小) 的内存
{
#ifdef ALLOCFAIL
  if (alloc_fail())
    return NULL;
#endif
  void * p;
  return (posix_memalign(&p, 64, size) == 0) ? p : NULL;
}

  void **
malloc_2d(const size_t nr, const size_t size) // 分配一个二维数组，行指针和数据区是连续的
{
  const size_t size1 = nr * sizeof(void *); // 行指针数组所需空间
  const size_t size2 = nr * size; // 所有数据行所需总空间
  void ** const mem = malloc(size1 + size2); // 一次性分配所有内存
  if (!mem) return NULL; // 分配失败检查
  u8 * const mem2 = ((u8 *)mem) + size1; // 数据区的起始地址
  for (size_t i = 0; i < nr; i++)
    mem[i] = mem2 + (i * size); // 设置每个行指针指向数据区中对应行的起始位置
  return mem;
}

  inline void **
calloc_2d(const size_t nr, const size_t size) // 分配并清零一个二维数组
{
  void ** const ret = malloc_2d(nr, size); // 先分配
  if (ret && ret[0]) // 确保分配成功且数据区有效 (ret[0] 指向数据区的开始)
    memset(ret[0], 0, nr * size); // 将整个数据区清零
  return ret;
}

  inline void
pages_unmap(void * const ptr, const size_t size) // 解除内存页映射或释放内存
{
#ifndef HEAPCHECKING // 如果不是在进行堆检查 (如Valgrind)
  munmap(ptr, size); // 使用munmap解除映射
#else // 如果在进行堆检查
  (void)size; // 避免未使用参数警告
  free(ptr); // 假设ptr是通过malloc等分配的，使用free释放
#endif
}

  void
pages_lock(void * const ptr, const size_t size) // 锁定内存页，防止被交换到磁盘 (mlock)
{
  static bool use_mlock = true; // 静态标志，表示是否尝试使用mlock
  if (use_mlock) {
    const int ret = mlock(ptr, size); // 尝试锁定内存
    if (ret != 0) { // 如果mlock失败 (通常因为权限不足或超出限制)
      use_mlock = false; // 不再尝试使用mlock
      fprintf(stderr, "%s: mlock disabled\n", __func__); // 打印禁用信息
    }
  }
}

#ifndef HEAPCHECKING // 正常模式 (非堆检查)
  static void *
pages_do_alloc(const size_t size, const int flags) // 执行页面分配的内部函数 (使用mmap)
{
  // vi /etc/security/limits.conf
  // * - memlock unlimited // 提示用户可能需要配置系统以允许mlock
  void * const p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0); // 使用mmap分配匿名私有内存
                                                                        // PROT_READ | PROT_WRITE: 可读可写
                                                                        // flags: 通常包含 MAP_PRIVATE | MAP_ANONYMOUS 及大页标志
                                                                        // -1, 0: 用于匿名映射
  if (p == MAP_FAILED) // 如果mmap失败
    return NULL;

  pages_lock(p, size); // 尝试锁定分配的页面
  return p;
}

#if defined(__linux__) && defined(MAP_HUGETLB) // Linux平台且定义了MAP_HUGETLB (支持大页)

#if defined(MAP_HUGE_SHIFT) // 如果定义了MAP_HUGE_SHIFT (用于指定大页大小)
#define PAGES_FLAGS_1G ((MAP_HUGETLB | (30 << MAP_HUGE_SHIFT))) // 1GB大页的mmap标志 (页面大小为 2^30)
#define PAGES_FLAGS_2M ((MAP_HUGETLB | (21 << MAP_HUGE_SHIFT))) // 2MB大页的mmap标志 (页面大小为 2^21)
#else // MAP_HUGE_SHIFT 未定义 (较旧的内核或不同的大页配置方式)
#define PAGES_FLAGS_1G ((MAP_HUGETLB)) // 通用大页标志 (依赖系统配置1GB大页)
#define PAGES_FLAGS_2M ((MAP_HUGETLB)) // 通用大页标志 (依赖系统配置2MB大页)
#endif // MAP_HUGE_SHIFT

#else // 非Linux或不支持大页
#define PAGES_FLAGS_1G ((0)) // 1GB大页标志为空 (不使用大页)
#define PAGES_FLAGS_2M ((0)) // 2MB大页标志为空
#endif // __linux__

#endif // HEAPCHECKING

  inline void *
pages_alloc_1gb(const size_t nr_1gb) // 分配指定数量的1GB大页
{
  const u64 sz = nr_1gb << 30; // 计算总大小 (nr_1gb * 1GB)
#ifndef HEAPCHECKING
  return pages_do_alloc(sz, MAP_PRIVATE | MAP_ANONYMOUS | PAGES_FLAGS_1G); // 使用mmap分配1GB大页
#else // HEAPCHECKING 模式
  // Valgrind 对齐要求可能与1GB大页冲突，这里使用2MB对齐的xalloc代替
  void * const p = xalloc(1lu << 21, sz); // 2MB对齐分配
  if (p)
    memset(p, 0, sz); // 清零内存 (mmap匿名页默认是清零的)
  return p;
#endif
}

  inline void *
pages_alloc_2mb(const size_t nr_2mb) // 分配指定数量的2MB大页
{
  const u64 sz = nr_2mb << 21; // 计算总大小 (nr_2mb * 2MB)
#ifndef HEAPCHECKING
  return pages_do_alloc(sz, MAP_PRIVATE | MAP_ANONYMOUS | PAGES_FLAGS_2M); // 使用mmap分配2MB大页
#else // HEAPCHECKING 模式
  void * const p = xalloc(1lu << 21, sz); // 2MB对齐分配
  if (p)
    memset(p, 0, sz);
  return p;
#endif
}

  inline void *
pages_alloc_4kb(const size_t nr_4kb) // 分配指定数量的4KB普通页
{
  const size_t sz = nr_4kb << 12; // 计算总大小 (nr_4kb * 4KB)
#ifndef HEAPCHECKING
  return pages_do_alloc(sz, MAP_PRIVATE | MAP_ANONYMOUS); // 使用mmap分配普通页 (无大页标志)
#else // HEAPCHECKING 模式
  void * const p = xalloc(1lu << 12, sz); // 4KB对齐分配
  if (p)
    memset(p, 0, sz);
  return p;
#endif
}

  void *
pages_alloc_best(const size_t size, const bool try_1gb, u64 * const size_out) // 尝试以最佳方式分配页面 (优先大页)
{
#ifdef ALLOCFAIL
  if (alloc_fail()) // 模拟分配失败
    return NULL;
#endif
  // 1gb huge page: at least 0.25GB // 尝试1GB大页：如果请求大小至少为0.25GB (2^28 bytes)
  if (try_1gb) { // 如果允许尝试1GB大页
    if (size >= (1lu << 28)) { // 请求大小 >= 256MB
      const size_t nr_1gb = bits_round_up(size, 30) >> 30; // 计算需要的1GB页数 (向上取整到1GB的倍数)
      void * const p1 = pages_alloc_1gb(nr_1gb); // 尝试分配1GB大页
      if (p1) { // 如果成功
        *size_out = nr_1gb << 30; // 返回实际分配的大小
        return p1;
      }
    }
  }

  // 2mb huge page: at least 0.5MB // 尝试2MB大页：如果请求大小至少为0.5MB (2^19 bytes)
  if (size >= (1lu << 19)) { // 请求大小 >= 512KB
    const size_t nr_2mb = bits_round_up(size, 21) >> 21; // 计算需要的2MB页数 (向上取整到2MB的倍数)
    void * const p2 = pages_alloc_2mb(nr_2mb); // 尝试分配2MB大页
    if (p2) { // 如果成功
      *size_out = nr_2mb << 21; // 返回实际分配的大小
      return p2;
    }
  }

  // 如果大页分配失败或不适用，则分配4KB普通页
  const size_t nr_4kb = bits_round_up(size, 12) >> 12; // 计算需要的4KB页数 (向上取整到4KB的倍数)
  void * const p3 = pages_alloc_4kb(nr_4kb); // 分配4KB普通页
  if (p3)
    *size_out = nr_4kb << 12; // 返回实际分配的大小
  return p3;
}
// }}} mm

// process/thread {{{ // 进程和线程相关功能
static u32 process_ncpu; // 系统CPU核心数
#if defined(__FreeBSD__) // FreeBSD特定定义
typedef cpuset_t cpu_set_t; // FreeBSD使用cpuset_t类型表示CPU集合
#elif defined(__APPLE__) && defined(__MACH__) // macOS特定定义
typedef u64 cpu_set_t; // macOS上用u64模拟cpu_set_t (最多64核)
#define CPU_SETSIZE ((64)) // 定义CPU集合的最大大小
#define CPU_COUNT(__cpu_ptr__) (__builtin_popcountl(*__cpu_ptr__)) // 计算集合中置位的CPU数量
#define CPU_ISSET(__cpu_idx__, __cpu_ptr__) (((*__cpu_ptr__) >> __cpu_idx__) & 1lu) // 检查指定CPU是否在集合中
#define CPU_ZERO(__cpu_ptr__) ((*__cpu_ptr__) = 0) // 清空CPU集合
#define CPU_SET(__cpu_idx__, __cpu_ptr__) ((*__cpu_ptr__) |= (1lu << __cpu_idx__)) // 将CPU添加到集合
#define CPU_CLR(__cpu_idx__, __cpu_ptr__) ((*__cpu_ptr__) &= ~(1lu << __cpu_idx__)) // 从集合中移除CPU
#define pthread_attr_setaffinity_np(...) ((void)0) // macOS没有pthread_attr_setaffinity_np，定义为空操作
#endif

__attribute__((constructor)) // 构造函数，在main之前执行
  static void
process_init(void) // 进程初始化函数
{
  // Linux's default is 1024 cpus // Linux默认的CPU_SETSIZE是1024
  process_ncpu = (u32)sysconf(_SC_NPROCESSORS_CONF); // 获取系统配置的CPU核心数
  if (process_ncpu > CPU_SETSIZE) { // 如果系统核心数超过了当前代码支持的CPU_SETSIZE
    fprintf(stderr, "%s: can use only %zu cores\n", // 打印警告
        __func__, (size_t)CPU_SETSIZE);
    process_ncpu = CPU_SETSIZE; // 将核心数限制为CPU_SETSIZE
  }
  thread_set_name(pthread_self(), "main"); // 将主线程的名称设置为"main"
}

  static inline int
thread_getaffinity_set(cpu_set_t * const cpuset) // 获取当前线程的CPU亲和性集合
{
#if defined(__linux__)
  return sched_getaffinity(0, sizeof(*cpuset), cpuset); // Linux: 使用sched_getaffinity (0表示当前线程)
#elif defined(__FreeBSD__)
  return cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(*cpuset), cpuset); // FreeBSD: 使用cpuset_getaffinity (-1表示当前线程)
#elif defined(__APPLE__) && defined(__MACH__)
  *cpuset = (1lu << process_ncpu) - 1; // macOS: 模拟，假设所有CPU都可用 (设置所有位为1)
  return (int)process_ncpu; // TODO: macOS的返回值和行为可能不完全符合预期
#endif // OS
}

  static inline int
thread_setaffinity_set(const cpu_set_t * const cpuset) // 设置当前线程的CPU亲和性集合
{
#if defined(__linux__)
  return sched_setaffinity(0, sizeof(*cpuset), cpuset); // Linux: 使用sched_setaffinity
#elif defined(__FreeBSD__)
  return cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(*cpuset), cpuset); // FreeBSD: 使用cpuset_setaffinity
#elif defined(__APPLE__) && defined(__MACH__)
  (void)cpuset; // macOS: 空操作，因为没有直接对应的函数
  return 0; // TODO: macOS实现不完整
#endif // OS
}

  void
thread_get_name(const pthread_t pt, char * const name, const size_t len) // 获取指定线程的名称
{
#if defined(__linux__)
  pthread_getname_np(pt, name, len); // Linux: 使用pthread_getname_np
#elif defined(__FreeBSD__)
  pthread_get_name_np(pt, name, len); // FreeBSD: 使用pthread_get_name_np (FreeBSD 9.0+)
#elif defined(__APPLE__) && defined(__MACH__)
  (void)pt; // macOS: pthread_getname_np可用，但这里未实现
  (void)len;
  strcpy(name, "unknown"); // TODO: macOS应使用pthread_getname_np
#endif // OS
}

  void
thread_set_name(const pthread_t pt, const char * const name) // 设置指定线程的名称
{
#if defined(__linux__)
  pthread_setname_np(pt, name); // Linux: 使用pthread_setname_np
#elif defined(__FreeBSD__)
  pthread_set_name_np(pt, name); // FreeBSD: 使用pthread_set_name_np
#elif defined(__APPLE__) && defined(__MACH__)
  // macOS 10.6+ 支持 pthread_setname_np(name) (注意参数不同)
  // 但这里是 (pt, name)，所以需要适配或忽略
  (void)pt; // macOS: 未适配或忽略
  (void)name; // TODO: macOS应使用pthread_setname_np(name)
#endif // OS
}

// kB // 返回值单位是KB
  long
process_get_rss(void) // 获取当前进程的常驻集大小 (Resident Set Size)
{
  struct rusage rs;
  getrusage(RUSAGE_SELF, &rs); // 获取当前进程的资源使用情况
  return rs.ru_maxrss; // ru_maxrss在Linux上单位是KB，macOS也是KB (BSD传统是pages，但macOS不同)
}

  u32
process_affinity_count(void) // 获取当前进程（或线程）允许运行的CPU核心数量
{
  cpu_set_t set;
  if (thread_getaffinity_set(&set) != 0) // 尝试获取当前亲和性设置
    return process_ncpu; // 如果失败，则返回系统总核心数

  const u32 nr = (u32)CPU_COUNT(&set); // 计算亲和性集合中的核心数
  return nr ? nr : process_ncpu; // 如果计算结果为0 (可能表示无限制或错误)，则返回总核心数
}

  u32
process_getaffinity_list(const u32 max, u32 * const cores) // 获取当前进程（或线程）的CPU亲和性核心列表
{
  memset(cores, 0, max * sizeof(cores[0])); // 初始化输出数组
  cpu_set_t set;
  if (thread_getaffinity_set(&set) != 0) // 获取亲和性设置
    return 0; // 失败返回0

  const u32 nr_affinity = (u32)CPU_COUNT(&set); // 亲和性集合中的核心总数
  const u32 nr = nr_affinity < max ? nr_affinity : max; // 实际要填充到cores数组的数量 (不超过max)
  u32 j = 0; // cores数组的索引
  for (u32 i = 0; i < process_ncpu; i++) { // 遍历所有可能的CPU核心
    if (CPU_ISSET(i, &set)) // 如果核心i在亲和性集合中
      cores[j++] = i; // 将核心号存入列表

    if (j >= nr) // 如果已填满cores数组 (或达到nr_affinity上限)
      break;
  }
  return j; // 返回实际获取到的核心数量
}

  void
thread_setaffinity_list(const u32 nr, const u32 * const list) // 根据给定的核心列表设置当前线程的CPU亲和性
{
  cpu_set_t set;
  CPU_ZERO(&set); // 清空CPU集合
  for (u32 i = 0; i < nr; i++) // 遍历列表中的核心号
    if (list[i] < process_ncpu) // 确保核心号有效
      CPU_SET(list[i], &set); // 将核心添加到集合中
  thread_setaffinity_set(&set); // 设置线程的亲和性
}

  void
thread_pin(const u32 cpu) // 将当前线程绑定到指定的CPU核心
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu % process_ncpu, &set); // 将指定的CPU核心 (取模确保有效) 添加到集合
  thread_setaffinity_set(&set); // 设置线程亲和性
}

  u64
process_cpu_time_usec(void) // 获取当前进程使用的CPU总时间 (用户态 + 内核态)，单位微秒
{
  struct rusage rs;
  getrusage(RUSAGE_SELF, &rs); // 获取资源使用情况
  const u64 usr = (((u64)rs.ru_utime.tv_sec) * 1000000lu) + ((u64)rs.ru_utime.tv_usec); // 用户态CPU时间 (微秒)
  const u64 sys = (((u64)rs.ru_stime.tv_sec) * 1000000lu) + ((u64)rs.ru_stime.tv_usec); // 内核态CPU时间 (微秒)
  return usr + sys; // 返回总CPU时间
}

struct fork_join_info { // fork-join模式的辅助信息结构体
  u32 total; // 需要执行的任务总数
  u32 ncores; // 可用的CPU核心数
  u32 * cores; // CPU核心列表 (用于分配任务)
  void *(*func)(void *); // 每个任务执行的函数指针
  bool args; // 标志参数类型：false为单个参数(arg1)，true为参数数组(argn)
  union {
    void * arg1; // 单个参数，所有任务共享
    void ** argn; // 参数数组，每个任务对应一个参数 (argn[rank])
  };
  union { // 用于记录错误数量的原子变量
    struct { volatile au32 ferr, jerr; }; // ferr: fork错误数, jerr: join错误数
    volatile au64 xerr; // 统一访问ferr和jerr (通过内存布局)
  };
};

// DON'T CHANGE! // 不要修改以下宏定义!
#define FORK_JOIN_RANK_BITS ((16)) // 定义任务秩 (rank) 的位数，决定了最大任务数
#define FORK_JOIN_MAX ((1u << FORK_JOIN_RANK_BITS)) // 最大任务数 (2^16 = 65536)

/*
 * fj(6):     T0        // 示例：6个任务的fork-join树形结构
 *         /      \
 *       T0        T4   // T0负责[0,3], T4负责[4,5] (假设)
 *     /   \      /
 *    T0   T2    T4     // T0负责[0,1], T2负责[2,3], T4负责[4,5]
 *   / \   / \   / \
 *  t0 t1 t2 t3 t4 t5   // 叶节点执行实际任务
 */

// recursive tree fork-join // 递归树形fork-join的工作线程函数
  static void *
thread_do_fork_join_worker(void * const ptr) // 参数ptr包含rank和fork_join_info指针
{
  struct entry13 fjp = {.ptr = ptr}; // entry13用于紧凑存储rank(e1)和指针(e3)
  // GCC: Without explicitly casting from fjp.fji (a 45-bit u64 value),
  // the high bits will get truncated, which is always CORRECT in gcc.
  // Don't use gcc. // 关于GCC编译器行为的注释，建议不要使用GCC (或注意此行为)
  struct fork_join_info * const fji = u64_to_ptr(fjp.e3); // 从entry13中提取fork_join_info指针
  const u32 rank = (u32)fjp.e1; // 从entry13中提取当前任务的秩 (rank)

  // 计算当前节点需要创建的子任务数量 (基于rank的二进制末尾0的个数，或总任务数)
  // nchild表示当前节点在fork树中的层级深度，或者说它负责的任务范围大小 (2^nchild)
  const u32 nchild = (u32)__builtin_ctz(rank ? rank : bits_p2_up_u32(fji->total));
  debug_assert(nchild <= FORK_JOIN_RANK_BITS); // 断言子任务数量不超过限制
  pthread_t tids[FORK_JOIN_RANK_BITS]; // 存储子线程的线程ID
  if (nchild) { // 如果当前节点不是叶节点 (即nchild > 0)，则需要fork子线程
    cpu_set_t set;
    CPU_ZERO(&set); // 初始化CPU亲和性集合
    pthread_attr_t attr;
    pthread_attr_init(&attr); // 初始化线程属性
    //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE); // Joinable by default // 线程默认是可join的
    // fork top-down // 自顶向下创建子线程
    for (u32 i = nchild - 1; i < nchild; i--) { // 循环创建 (nchild - 1) 到 0 的子任务
                                                // 注意：这里的循环条件 i < nchild 配合 i-- 会导致u32回绕，
                                                // 实际效果是从 (nchild-1) 递减到 0。
      const u32 cr = rank + (1u << i); // 计算子任务的秩 (child rank)
      if (cr >= fji->total) // 如果子任务秩超出总任务数，则跳过
        continue; // should not break // 不应中断，因为其他子任务可能仍然有效
      // 为子线程选择一个CPU核心
      const u32 core = fji->cores[(cr < fji->ncores) ? cr : (cr % fji->ncores)];
      CPU_SET(core, &set); // 设置子线程的CPU亲和性
      pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
      fjp.e1 = (u16)cr; // 更新传递给子线程的参数中的rank
      const int r = pthread_create(&tids[i], &attr, thread_do_fork_join_worker, fjp.ptr); // 创建子线程
      CPU_CLR(core, &set); // 清除刚用过的核心，为下一个子线程准备 (如果set是共享的)
      if (unlikely(r)) { // fork failed // 如果创建线程失败
        memset(&tids[0], 0, sizeof(tids[0]) * (i+1)); // 将已创建(或尝试创建)的tids清零
        u32 nmiss = (1u << (i + 1)) - 1; // 理论上这个分支及其所有子孙任务都失败了
        if ((rank + nmiss) >= fji->total) // 调整失败任务数，不超过总数
          nmiss = fji->total - 1 - rank;
        (void)atomic_fetch_add_explicit(&fji->ferr, nmiss, MO_RELAXED); // 原子增加fork错误计数
        break; // 中断当前节点的fork过程
      }
    }
    pthread_attr_destroy(&attr); // 销毁线程属性对象
  }

  char thname0[16]; // 存储原始线程名
  char thname1[16]; // 存储新线程名 (带rank)
  thread_get_name(pthread_self(), thname0, 16); // 获取当前线程名
  snprintf(thname1, 16, "%.8s_%u", thname0, rank); // 构造新线程名，如 "basename_rank"
  thread_set_name(pthread_self(), thname1); // 设置新线程名 (便于调试)

  // 执行实际的工作函数
  void * const ret = fji->func(fji->args ? fji->argn[rank] : fji->arg1);

  thread_set_name(pthread_self(), thname0); // 恢复原始线程名
  // join bottom-up // 自底向上等待子线程结束
  for (u32 i = 0; i < nchild; i++) {
    const u32 cr = rank + (1u << i); // 子任务秩
    if (cr >= fji->total) // 如果超出总任务数
      break; // safe to break // 可以安全中断 (因为更高秩的子任务不会被创建)
    if (tids[i]) { // 如果线程ID有效 (即线程已成功创建)
      const int r = pthread_join(tids[i], NULL); // 等待子线程结束
      if (unlikely(r)) { // error // 如果join失败
        //fprintf(stderr, "pthread_join %u..%u = %d: %s\n", rank, cr, r, strerror(r));
        (void)atomic_fetch_add_explicit(&fji->jerr, 1, MO_RELAXED); // 原子增加join错误计数
      }
    }
  }
  return ret; // 返回工作函数的结果 (对于非主线程，此返回值通常被忽略)
}

  u64
thread_fork_join(u32 nr, void *(*func) (void *), const bool args, void * const argx) // 执行fork-join并行模式
{
  if (unlikely(nr > FORK_JOIN_MAX)) { // 检查任务数是否超过最大限制
    fprintf(stderr, "%s reduce nr to %u\n", __func__, FORK_JOIN_MAX);
    nr = FORK_JOIN_MAX; // 如果超过，则限制为最大值
  }

  u32 cores[CPU_SETSIZE]; // 存储可用CPU核心列表
  u32 ncores = process_getaffinity_list(process_ncpu, cores); // 获取当前进程可用的核心列表
  if (unlikely(ncores == 0)) { // force to use all cores // 如果获取失败 (或返回0)，则强制使用所有系统核心
    ncores = process_ncpu;
    for (u32 i = 0; i < process_ncpu; i++)
      cores[i] = i;
  }
  if (unlikely(nr == 0)) // 如果任务数为0，则默认设置为可用核心数
    nr = ncores;

  // the compiler does not know fji can change since we cast &fji into fjp
  // 编译器不知道fji可能会改变，因为我们将&fji的地址转换为u64再转回指针
  struct fork_join_info fji = {.total = nr, .cores = cores, .ncores = ncores,
      .func = func, .args = args}; // 初始化fork_join_info结构
  if (args) fji.argn = argx; else fji.arg1 = argx; // 根据args标志设置参数
  const struct entry13 fjp = entry13(0, (u64)(&fji)); // 将rank=0和fji指针打包

  // save current affinity // 保存当前线程的CPU亲和性
  cpu_set_t set0;
  thread_getaffinity_set(&set0);

  // master thread shares thread0's core // 主线程 (调用此函数的线程) 将使用核心列表中的第一个核心
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(fji.cores[0], &set); // 设置亲和性到第一个可用核心
  thread_setaffinity_set(&set);

  const u64 t0 = time_nsec(); // 记录开始时间
  (void)thread_do_fork_join_worker(fjp.ptr); // 主线程作为rank 0开始执行任务
  const u64 dt = time_diff_nsec(t0); // 计算总耗时

  // restore original affinity // 恢复原始的CPU亲和性
  thread_setaffinity_set(&set0);

  // check and report errors (unlikely) // 检查并报告在fork或join过程中发生的错误
  if (atomic_load_explicit(&fji.xerr, MO_CONSUME)) // 如果错误计数非零
    fprintf(stderr, "%s errors: fork %u join %u\n", __func__, fji.ferr, fji.jerr);
  return dt; // 返回总执行时间 (纳秒)
}

  int
thread_create_at(const u32 cpu, pthread_t * const thread, // 在指定的CPU核心上创建新线程
    void *(*start_routine) (void *), void * const arg)
{
  const u32 cpu_id = (cpu < process_ncpu) ? cpu : (cpu % process_ncpu); // 确保cpu_id有效
  pthread_attr_t attr; // 线程属性
  pthread_attr_init(&attr); // 初始化线程属性
  //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE); // 默认是PTHREAD_CREATE_JOINABLE
  cpu_set_t set; // CPU亲和性集合

  CPU_ZERO(&set); // 清空集合
  CPU_SET(cpu_id, &set); // 将指定CPU添加到集合
  pthread_attr_setaffinity_np(&attr, sizeof(set), &set); // 设置线程属性中的CPU亲和性
  const int r = pthread_create(thread, &attr, start_routine, arg); // 创建线程
  pthread_attr_destroy(&attr); // 销毁线程属性对象
  return r; // 返回pthread_create的结果
}
// }}} process/thread

// locking {{{ // 锁机制

// spinlock {{{ // 自旋锁实现
#if defined(__linux__)
#define SPINLOCK_PTHREAD // 在Linux上，如果定义了此宏，则使用pthread_spinlock_t实现自旋锁
#endif // __linux__

#if defined(SPINLOCK_PTHREAD) // 使用pthread_spinlock_t的实现
static_assert(sizeof(pthread_spinlock_t) <= sizeof(spinlock), "spinlock size"); // 确保自定义spinlock类型足够大
#else // SPINLOCK_PTHREAD // 使用原子操作的自定义实现
static_assert(sizeof(au32) <= sizeof(spinlock), "spinlock size"); // 确保自定义spinlock类型基于原子u32
#endif // SPINLOCK_PTHREAD

  void
spinlock_init(spinlock * const lock) // 初始化自旋锁
{
#if defined(SPINLOCK_PTHREAD)
  pthread_spinlock_t * const p = (typeof(p))lock;
  pthread_spin_init(p, PTHREAD_PROCESS_PRIVATE); // 初始化pthread自旋锁 (进程内私有)
#else // SPINLOCK_PTHREAD
  au32 * const p = (typeof(p))lock;
  atomic_store_explicit(p, 0, MO_RELEASE); // 原子地将锁状态初始化为0 (未锁定)
                                          // MO_RELEASE确保初始化对其他线程可见
#endif // SPINLOCK_PTHREAD
}

  inline void
spinlock_lock(spinlock * const lock) // 获取自旋锁 (阻塞)
{
#if defined(CORR) // 如果在协程环境中使用
#pragma nounroll
  while (!spinlock_trylock(lock)) // 循环尝试获取锁
    corr_yield(); // 如果获取失败，则让出协程执行权
#else // CORR // 非协程环境
#if defined(SPINLOCK_PTHREAD)
  pthread_spinlock_t * const p = (typeof(p))lock;
  pthread_spin_lock(p); // return value ignored // 调用pthread_spin_lock，忽略返回值
#else // SPINLOCK_PTHREAD // 自定义原子操作实现的自旋锁
  au32 * const p = (typeof(p))lock; // p指向原子变量
#pragma nounroll // 提示编译器不要展开此循环
  do {
    // 尝试将锁值原子地从0交换为1 (或类似操作，取决于具体实现)。
    // 这里的实现是：0表示未锁定。获取锁时，尝试将值减1。如果原始值是0，则成功。
    // MO_ACQUIRE确保成功获取锁后，后续的临界区代码不会被重排到锁操作之前。
    if (atomic_fetch_sub_explicit(p, 1, MO_ACQUIRE) == 0) // 尝试获取锁，如果之前是0，则成功
      return; // 成功获取锁
#pragma nounroll
    do { // 自旋等待
      cpu_pause(); // 发出pause指令，降低CPU功耗并提示是自旋等待
      // MO_CONSUME确保对锁状态的读取不会被非法重排，并且后续依赖此值的操作能看到最新状态。
    } while (atomic_load_explicit(p, MO_CONSUME)); // 持续检查锁状态是否仍被持有 (非0)
                                                  // 注意：这里的条件 `while (atomic_load_explicit(p, MO_CONSUME))`
                                                  // 意味着只要锁的值不为0就继续pause。
                                                  // 结合获取锁的逻辑 (fetch_sub == 0)，解锁时应将值恢复为0。
  } while (true); // 持续尝试，直到获取锁
#endif // SPINLOCK_PTHREAD
#endif // CORR
}

  inline bool
spinlock_trylock(spinlock * const lock) // 尝试获取自旋锁 (非阻塞)
{
#if defined(SPINLOCK_PTHREAD)
  pthread_spinlock_t * const p = (typeof(p))lock;
  return !pthread_spin_trylock(p); // pthread_spin_trylock成功返回0，所以取反
#else // SPINLOCK_PTHREAD
  au32 * const p = (typeof(p))lock;
  // 尝试原子地将锁值减1。如果操作前的原始值是0，表示锁未被持有，则获取成功。
  // MO_ACQUIRE用于成功获取锁的情况。
  return atomic_fetch_sub_explicit(p, 1, MO_ACQUIRE) == 0;
#endif // SPINLOCK_PTHREAD
}

  inline void
spinlock_unlock(spinlock * const lock) // 释放自旋锁
{
#if defined(SPINLOCK_PTHREAD)
  pthread_spinlock_t * const p = (typeof(p))lock;
  pthread_spin_unlock(p); // return value ignored // 调用pthread_spin_unlock
#else // SPINLOCK_PTHREAD
  au32 * const p = (typeof(p))lock;
  // 原子地将锁状态设置为0 (未锁定)。
  // MO_RELEASE确保此解锁操作之前在临界区内的所有内存写操作，对之后获取此锁的线程可见。
  atomic_store_explicit(p, 0, MO_RELEASE);
#endif // SPINLOCK_PTHREAD
}
// }}} spinlock

// pthread mutex {{{ // POSIX互斥锁封装
static_assert(sizeof(pthread_mutex_t) <= sizeof(mutex), "mutexlock size"); // 确保自定义mutex类型大小足够
  inline void
mutex_init(mutex * const lock) // 初始化互斥锁
{
  pthread_mutex_t * const p = (typeof(p))lock;
  pthread_mutex_init(p, NULL); // 使用默认属性初始化POSIX互斥锁
}

  inline void
mutex_lock(mutex * const lock) // 获取互斥锁 (阻塞)
{
#if defined(CORR) // 协程环境
#pragma nounroll
  while (!mutex_trylock(lock)) // 循环尝试获取
    corr_yield(); // 失败则让出协程执行权
#else // 非协程环境
  pthread_mutex_t * const p = (typeof(p))lock;
  pthread_mutex_lock(p); // return value ignored // 调用pthread_mutex_lock
#endif
}

  inline bool
mutex_trylock(mutex * const lock) // 尝试获取互斥锁 (非阻塞)
{
  pthread_mutex_t * const p = (typeof(p))lock;
  return !pthread_mutex_trylock(p); // pthread_mutex_trylock成功返回0，所以取反
}

  inline void
mutex_unlock(mutex * const lock) // 释放互斥锁
{
  pthread_mutex_t * const p = (typeof(p))lock;
  pthread_mutex_unlock(p); // return value ignored // 调用pthread_mutex_unlock
}

  inline void
mutex_deinit(mutex * const lock) // 销毁互斥锁
{
  pthread_mutex_t * const p = (typeof(p))lock;
  pthread_mutex_destroy(p); // 销毁POSIX互斥锁，释放资源
}
// }}} pthread mutex

// rwdep {{{ // 读写锁依赖关系检查 (简易版 lockdep)
// poor man's lockdep for rwlock // 这是一个简易的、用于读写锁的锁依赖检查机制
// per-thread lock list // 每个线程维护一个已获取的锁的列表
// it calls debug_die() when local double-(un)locking is detected // 当检测到同一线程重复加锁或解锁未持有的锁时，会调用debug_die()
// cyclic dependencies can be manually identified by looking at the two lists below in gdb // 可以通过在GDB中查看这两个列表来手动识别潜在的锁循环依赖问题
#ifdef RWDEP // 如果定义了RWDEP宏 (即启用了读写锁依赖检查)
#define RWDEP_NR ((16)) // 每个线程最多可以记录16个读锁和16个写锁
__thread const rwlock * rwdep_readers[RWDEP_NR] = {}; // 线程局部存储：当前线程持有的读锁列表
__thread const rwlock * rwdep_writers[RWDEP_NR] = {}; // 线程局部存储：当前线程持有的写锁列表

  static void
rwdep_check(const rwlock * const lock) // 检查指定锁是否已被当前线程持有 (避免重复加锁)
{
  debug_assert(lock); // 断言传入的锁指针有效
  for (u64 i = 0; i < RWDEP_NR; i++) { // 遍历已持有的读锁和写锁列表
    if (rwdep_readers[i] == lock) // 如果已作为读锁持有
      debug_die(); // 错误：重复获取读锁或获取已被读锁定的锁作为写锁(取决于调用上下文)
    if (rwdep_writers[i] == lock) // 如果已作为写锁持有
      debug_die(); // 错误：重复获取写锁或获取已被写锁定的锁
  }
}
#endif // RWDEP

  static void
rwdep_lock_read(const rwlock * const lock) // 记录获取读锁操作 (用于依赖检查)
{
#ifdef RWDEP
  rwdep_check(lock); // 首先检查是否重复加锁
  for (u64 i = 0; i < RWDEP_NR; i++) { // 查找列表中的空位
    if (rwdep_readers[i] == NULL) {
      rwdep_readers[i] = lock; // 将锁记录到读锁列表中
      return;
    }
  }
  // 如果列表已满，这里没有处理，可能会导致无法跟踪更多锁，或者应该报错
#else
  (void)lock; // 如果未启用RWDEP，则此函数为空操作
#endif // RWDEP
}

  static void
rwdep_unlock_read(const rwlock * const lock) // 记录释放读锁操作 (用于依赖检查)
{
#ifdef RWDEP
  for (u64 i = 0; i < RWDEP_NR; i++) { // 查找列表中匹配的锁
    if (rwdep_readers[i] == lock) {
      rwdep_readers[i] = NULL; // 从读锁列表中移除
      return;
    }
  }
  debug_die(); // 如果未找到匹配的锁，表示尝试解锁一个未持有的读锁，调用debug_die
#else
  (void)lock; // 空操作
#endif // RWDEP
}

  static void
rwdep_lock_write(const rwlock * const lock) // 记录获取写锁操作 (用于依赖检查)
{
#ifdef RWDEP
  rwdep_check(lock); // 检查是否重复加锁
  for (u64 i = 0; i < RWDEP_NR; i++) { // 查找列表中的空位
    if (rwdep_writers[i] == NULL) {
      rwdep_writers[i] = lock; // 将锁记录到写锁列表中
      return;
    }
  }
  // 列表已满处理同上
#else
  (void)lock; // 空操作
#endif // RWDEP
}

  static void
rwdep_unlock_write(const rwlock * const lock) // 记录释放写锁操作 (用于依赖检查)
{
#ifdef RWDEP
  for (u64 i = 0; i < RWDEP_NR; i++) { // 查找列表中匹配的锁
    if (rwdep_writers[i] == lock) {
      rwdep_writers[i] = NULL; // 从写锁列表中移除
      return;
    }
  }
  debug_die(); // 如果未找到匹配的锁，表示尝试解锁一个未持有的写锁，调用debug_die
#else
  (void)lock; // 空操作
#endif // RWDEP
}
// }}} rwlockdep

// rwlock {{{ // 读写锁实现
typedef au32 lock_t; // 定义锁状态的原子类型 (通常是原子无符号32位整数)
typedef u32 lock_v; // 定义锁状态的普通非原子类型 (用于比较等)
static_assert(sizeof(lock_t) == sizeof(lock_v), "lock size"); // 确保原子和非原子类型大小一致
static_assert(sizeof(lock_t) <= sizeof(rwlock), "lock size"); // 确保自定义rwlock结构体大小足以容纳锁状态变量

#define RWLOCK_WSHIFT ((sizeof(lock_t) * 8 - 1)) // 写锁标志位在整数中的位移 (即最高位)
#define RWLOCK_WBIT ((((lock_v)1) << RWLOCK_WSHIFT)) // 写锁标志位的值 (例如，对于u32是0x80000000)
                                                    // 锁状态的其余位 (低位) 用于记录读锁的数量

  inline void
rwlock_init(rwlock * const lock) // 初始化读写锁
{
  lock_t * const pvar = (typeof(pvar))lock; // 将rwlock指针转换为底层原子类型指针
  atomic_store_explicit(pvar, 0, MO_RELEASE); // 原子地将锁状态初始化为0 (表示无读锁，无写锁)
                                              // MO_RELEASE确保此初始化对其他线程可见
}

  inline bool
rwlock_trylock_read(rwlock * const lock) // 尝试获取读锁 (非阻塞)
{
  lock_t * const pvar = (typeof(pvar))lock;
  // 原子地将读计数器加1。MO_ACQUIRE确保成功获取锁后，后续的临界区代码不会被重排到此操作之前。
  // 然后检查结果的最高位 (写锁位)。如果写锁位为0，表示没有写锁，则成功获取读锁。
  if ((atomic_fetch_add_explicit(pvar, 1, MO_ACQUIRE) >> RWLOCK_WSHIFT) == 0) {
    rwdep_lock_read(lock); // 如果启用了依赖检查，记录读锁获取
    return true; // 获取成功
  } else {
    // 如果写锁位为1 (表示当前有写锁或写者意图)，则获取读锁失败。
    // 需要将之前错误增加的读计数减回去。
    atomic_fetch_sub_explicit(pvar, 1, MO_RELAXED); // MO_RELAXED即可，因为这只是回滚操作
    return false; // 获取失败
  }
}

  inline bool
rwlock_trylock_read_lp(rwlock * const lock) // 尝试获取读锁 (低优先级版本，会先检查是否有写锁)
{
  lock_t * const pvar = (typeof(pvar))lock;
  // 首先加载当前锁状态，检查写锁位。MO_CONSUME确保后续依赖于此锁状态的读操作不会被非法重排。
  if (atomic_load_explicit(pvar, MO_CONSUME) >> RWLOCK_WSHIFT) { // 如果写锁位为1 (有写锁)
    cpu_pause(); // 短暂暂停，避免忙等消耗过多CPU，并给写锁机会完成
    return false; // 获取失败
  }
  return rwlock_trylock_read(lock); // 如果没有写锁，则尝试正常获取读锁
}

// actually nr + 1 // 实际尝试次数是 nr + 1 次
  inline bool
rwlock_trylock_read_nr(rwlock * const lock, u16 nr) // 尝试获取读锁，最多尝试 nr+1 次
{
  lock_t * const pvar = (typeof(pvar))lock;
  // 第一次尝试获取读锁
  if ((atomic_fetch_add_explicit(pvar, 1, MO_ACQUIRE) >> RWLOCK_WSHIFT) == 0) {
    rwdep_lock_read(lock);
    return true; // 成功
  }
  // 如果第一次失败 (因为有写锁)，此时读计数已被加1。
  // 进入循环继续尝试，如果循环结束仍未成功，则需要将这个1减掉。
#pragma nounroll
  do { // someone already locked; wait for a little while // 有其他锁持有者 (写锁)；稍作等待
    cpu_pause(); // 暂停
    // 再次检查写锁位。如果此时写锁已释放 (写锁位为0)，
    // 因为之前读计数已加1，所以现在可以直接认为获取成功。
    if ((atomic_load_explicit(pvar, MO_CONSUME) >> RWLOCK_WSHIFT) == 0) {
      rwdep_lock_read(lock);
      return true; // 获取成功
    }
  } while (nr--); // 循环 nr 次

  // 所有尝试都失败了，回滚第一次增加的读计数。
  atomic_fetch_sub_explicit(pvar, 1, MO_RELAXED);
  return false; // 获取失败
}

  inline void
rwlock_lock_read(rwlock * const lock) // 获取读锁 (阻塞)
{
  lock_t * const pvar = (typeof(pvar))lock;
#pragma nounroll
  do {
    if (rwlock_trylock_read(lock)) // 尝试获取读锁
      return; // 成功则返回
#pragma nounroll
    do { // 获取失败，表示有写锁，需要自旋等待直到写锁被释放
#if defined(CORR)
      corr_yield(); // 协程环境：让出执行权
#else
      cpu_pause(); // 非协程环境：CPU暂停
#endif
      // MO_CONSUME确保对锁状态的读取不会被非法重排。
    } while (atomic_load_explicit(pvar, MO_CONSUME) >> RWLOCK_WSHIFT); // 持续检查写锁位是否为0
  } while (true); // 持续尝试，直到获取读锁
}

  inline void
rwlock_unlock_read(rwlock * const lock) // 释放读锁
{
  rwdep_unlock_read(lock); // 如果启用了依赖检查，记录读锁释放
  lock_t * const pvar = (typeof(pvar))lock;
  // 原子地将读计数器减1。
  // MO_RELEASE确保此解锁操作之前在临界区内的所有内存读操作 (对于读者而言主要是读)，
  // 以及这个计数的更新，对之后可能获取写锁的线程可见。
  atomic_fetch_sub_explicit(pvar, 1, MO_RELEASE);
}

  inline bool
rwlock_trylock_write(rwlock * const lock) // 尝试获取写锁 (非阻塞，写者优先度较低)
{
  lock_t * const pvar = (typeof(pvar))lock;
  lock_v v0 = 0; // 期望的锁状态：0 (完全空闲，无读者也无写者)
                 // 注意：这里v0在CAS失败时会被更新为当前值，所以初始为0是正确的。
  // 尝试原子地将锁状态从0交换为RWLOCK_WBIT (写锁标记)。
  // MO_ACQUIRE确保成功获取锁后，后续的临界区代码不会被重排到此操作之前。
  // MO_RELAXED用于CAS失败时的加载，因为如果失败，v0会被更新为当前值，可以用于下一次尝试（如果循环的话）。
  if (atomic_compare_exchange_weak_explicit(pvar, &v0, RWLOCK_WBIT, MO_ACQUIRE, MO_RELAXED)) {
    // CAS成功仅当v0原始值为0时。
    rwdep_lock_write(lock); // 记录写锁获取
    return true; // 获取成功
  } else {
    return false; // 获取失败 (锁非空闲，或CAS因竞争失败)
  }
}

// actually nr + 1 // 实际尝试次数是 nr + 1 次
  inline bool
rwlock_trylock_write_nr(rwlock * const lock, u16 nr) // 尝试获取写锁，最多尝试 nr+1 次
{
#pragma nounroll
  do {
    if (rwlock_trylock_write(lock)) // 尝试获取写锁
      return true; // 成功则返回
    cpu_pause(); // 失败则暂停一下
  } while (nr--); // 循环 nr 次
  return false; // 所有尝试都失败
}

  inline void
rwlock_lock_write(rwlock * const lock) // 获取写锁 (阻塞，写者优先度较低)
{
  lock_t * const pvar = (typeof(pvar))lock;
#pragma nounroll
  do {
    if (rwlock_trylock_write(lock)) // 尝试获取写锁
      return; // 成功则返回
#pragma nounroll
    do { // 获取失败，自旋等待直到锁变为空闲状态 (值为0)
#if defined(CORR)
      corr_yield(); // 协程环境：让出执行权
#else
      cpu_pause(); // 非协程环境：CPU暂停
#endif
      // MO_CONSUME确保对锁状态的读取不会被非法重排。
    } while (atomic_load_explicit(pvar, MO_CONSUME)); // 持续检查锁状态是否为0 (完全空闲)
  } while (true); // 持续尝试，直到获取写锁
}

  inline bool
rwlock_trylock_write_hp(rwlock * const lock) // 尝试获取写锁 (高优先级写者：允许在有读者时标记写意图)
{
  lock_t * const pvar = (typeof(pvar))lock;
  lock_v v0 = atomic_load_explicit(pvar, MO_CONSUME); // 读取当前锁状态 (包含读计数和可能的写锁位)
  if (v0 >> RWLOCK_WSHIFT) // 如果已经有其他写者持有或意图持有写锁 (即写锁位已被设置)
    return false; // 则当前写者失败

  // 尝试原子地设置写锁位 (RWLOCK_WBIT)，同时保留已有的读锁计数。
  // 期望的原始值是 v0 (当前读计数，且写锁位为0)，新值是 v0 | RWLOCK_WBIT。
  // MO_ACQUIRE确保成功标记写意图后，后续操作 (包括等待读者离开的循环) 不会被重排。
  if (atomic_compare_exchange_weak_explicit(pvar, &v0, v0 | RWLOCK_WBIT, MO_ACQUIRE, MO_RELAXED)) {
    // CAS成功，表示写意图已标记。v0现在是标记前的读锁计数值。
    rwdep_lock_write(lock); // 记录写锁获取
    // WBIT successfully marked; must wait for readers to leave // 写锁位成功标记；现在必须等待所有读者离开
    if (v0) { // saw active readers // 如果v0 (CAS操作前的读锁数) 不为0，表示有活跃的读者
#pragma nounroll
      // 自旋等待，直到锁状态变为仅有写锁标记 (RWLOCK_WBIT)，即所有读者都已释放读锁。
      // (锁状态值等于RWLOCK_WBIT意味着读计数部分为0)
      while (atomic_load_explicit(pvar, MO_CONSUME) != RWLOCK_WBIT) {
#if defined(CORR)
        corr_yield();
#else
        cpu_pause();
#endif
      }
    }
    return true; // 成功获取写锁 (所有读者已离开，或者原本就没有读者)
  } else {
    // CAS失败，可能是因为：
    // 1. 在load和CAS之间，锁状态被其他线程改变 (例如其他写者获得了锁，或读者数量改变)。
    // 2. v0在load时就已经有写锁位了 (虽然前面有检查，但weak CAS可能因其他原因虚假失败后v0被更新)。
    return false;
  }
}

  inline bool
rwlock_trylock_write_hp_nr(rwlock * const lock, u16 nr) // 尝试获取高优先级写锁，最多尝试 nr+1 次
{
#pragma nounroll
  do {
    if (rwlock_trylock_write_hp(lock)) // 尝试获取高优先级写锁
      return true; // 成功则返回
    cpu_pause(); // 失败则暂停
  } while (nr--); // 循环 nr 次
  return false; // 所有尝试都失败
}

  inline void
rwlock_lock_write_hp(rwlock * const lock) // 获取高优先级写锁 (阻塞)
{
#pragma nounroll
  while (!rwlock_trylock_write_hp(lock)) { // 循环尝试获取高优先级写锁
#if defined(CORR)
    corr_yield(); // 协程环境：让出执行权
#else
    cpu_pause(); // 非协程环境：CPU暂停
#endif
  }
}

  inline void
rwlock_unlock_write(rwlock * const lock) // 释放写锁
{
  rwdep_unlock_write(lock); // 如果启用了依赖检查，记录写锁释放
  lock_t * const pvar = (typeof(pvar))lock;
  // 原子地清除写锁位 (通过减去RWLOCK_WBIT)。
  // MO_RELEASE确保此解锁操作之前在临界区内的所有内存写操作，对之后获取此锁的线程 (无论是读者还是写者) 可见。
  atomic_fetch_sub_explicit(pvar, RWLOCK_WBIT, MO_RELEASE);
}

  inline void
rwlock_write_to_read(rwlock * const lock) // 将持有的写锁降级为读锁
{
  rwdep_unlock_write(lock); // 首先，从依赖跟踪中移除写锁
  rwdep_lock_read(lock);    // 然后，添加读锁的依赖跟踪
  lock_t * const pvar = (typeof(pvar))lock;
  // +R -W // 操作：增加一个读计数 (原子加1)，同时清除写锁位 (原子减RWLOCK_WBIT)。
          // 这可以通过一次原子加法完成：加 (1 - RWLOCK_WBIT)。
          // 假设当前值为 WBIT (纯写锁)，则新值为 WBIT + 1 - WBIT = 1 (一个读锁)。
  // MO_ACQ_REL：
  //  Release语义：确保写锁期间的所有操作对后续看到这个新读锁状态的线程可见。
  //  Acquire语义：确保后续的读操作 (作为新读者) 不会被重排到此降级操作之前。
  atomic_fetch_add_explicit(pvar, ((lock_v)1) - RWLOCK_WBIT, MO_ACQ_REL);
}

#undef RWLOCK_WSHIFT // 取消宏定义 RWLOCK_WSHIFT
#undef RWLOCK_WBIT   // 取消宏定义 RWLOCK_WBIT
// }}} rwlock

// }}} locking

// coroutine {{{ // 协程实现

// asm {{{ // 汇编代码部分
#if defined(__x86_64__)
// co_switch_stack 中压栈的寄存器数量
#define CO_CONTEXT_SIZE ((6)) // x86_64下保存6个寄存器: rbp, rbx, r12, r13, r14, r15

// 用于切换/退出：将返回值传递给目标协程
asm (
    ".align 16;" // 16字节对齐
#if defined(__linux__) || defined(__FreeBSD__)
    ".global co_switch_stack;" // 声明全局符号 (Linux/FreeBSD)
    ".type co_switch_stack, @function;" // 定义符号类型为函数
    "co_switch_stack:"
#elif defined(__APPLE__) && defined(__MACH__)
    ".global _co_switch_stack;" // macOS下的全局符号
    "_co_switch_stack:"
#else
#error Supported platforms: Linux/FreeBSD/Apple // 不支持的平台
#endif // OS
    "push %rbp; push %rbx; push %r12;" // 保存调用者保存的寄存器
    "push %r13; push %r14; push %r15;"
    "mov  %rsp, (%rdi);" // rdi (第一个参数): 指向保存当前协程栈指针的地址，将当前rsp存入
    "mov  %rsi, %rsp;" // rsi (第二个参数): 新协程的栈指针，恢复到rsp
    "pop  %r15; pop  %r14; pop  %r13;" // 恢复之前保存的寄存器
    "pop  %r12; pop  %rbx; pop  %rbp;"
    "mov  %rdx, %rax;" // rdx (第三个参数): 返回值，放入rax (函数返回值通常在rax)
    "retq;" // 返回
    );

#elif defined(__aarch64__)
// co_switch_stack 中压栈/操作的寄存器数量相关的宏 (实际保存10个通用寄存器和8个浮点寄存器)
#define CO_CONTEXT_SIZE ((20)) // 估算上下文大小 (10 GPRs + 8 FPRs + LR + SP)

asm (
    ".align 16;" // 16字节对齐
#if defined(__linux__) || defined(__FreeBSD__)
    ".global co_switch_stack;" // 声明全局符号
    ".type co_switch_stack, @function;" // 定义符号类型为函数
    "co_switch_stack:"
#elif defined(__APPLE__) && defined(__MACH__)
    ".global _co_switch_stack;" // macOS下的全局符号
    "_co_switch_stack:"
#else
#error supported platforms: Linux/FreeBSD/Apple // 不支持的平台
#endif // OS
    // x0: &save_rsp (保存旧rsp的地址), x1: new_rsp (新rsp), x2: retval (返回值)
    "sub  x8, sp, 160;"      // 计算当前栈顶下方160字节处地址 (用于保存上下文)
    "str  x8, [x0];"         // 将计算出的地址 (旧rsp的保存位置) 保存到 *save_rsp (x0)
    // 保存调用者保存的通用寄存器 (x19-x28) 和链接寄存器 (x30)
    // stp: store pair of registers, ldp: load pair of registers
    // [x8] 是旧栈的保存区域, [x1] 是新栈的恢复区域 (new_rsp 指向的是已保存上下文的栈顶)
    "stp x30, x19, [x8];      ldp x30, x19, [x1];"      // 保存lr(x30), x19 到旧栈；从新栈加载 lr, x19
    "stp x20, x21, [x8, 16];  ldp x20, x21, [x1, 16];" // 保存x20, x21；加载x20, x21 (偏移16字节)
    "stp x22, x23, [x8, 32];  ldp x22, x23, [x1, 32];" // ...
    "stp x24, x25, [x8, 48];  ldp x24, x25, [x1, 48];"
    "stp x26, x27, [x8, 64];  ldp x26, x27, [x1, 64];"
    "stp x28, x29, [x8, 80];  ldp x28, x29, [x1, 80];" // x29是帧指针FP
    // 保存调用者保存的浮点寄存器 (d8-d15)
    "stp  d8,  d9, [x8, 96];  ldp  d8,  d9, [x1, 96];"
    "stp d10, d11, [x8, 112]; ldp d10, d11, [x1, 112];"
    "stp d12, d13, [x8, 128]; ldp d12, d13, [x1, 128];"
    "stp d14, d15, [x8, 144]; ldp d14, d15, [x1, 144];"
    "add  sp, x1, 160;"      // 设置新的栈指针 (sp = new_rsp + 160, new_rsp是上下文区域的基址)
    "mov  x0, x2;"           // 将retval (x2) 放入 x0 (函数返回值)
    "br  x30;"               // 跳转到新的链接寄存器 (lr), 即返回到调用 co_switch_stack 之后的位置，但在新协程的上下文中
    );

extern void co_entry_aarch64(void); // 声明AArch64协程入口函数
asm (
    ".align 16;" // 16字节对齐
#if defined(__linux__) || defined(__FreeBSD__)
    ".global co_entry_aarch64;" // 声明全局符号
    ".type co_entry_aarch64, @function;" // 定义符号类型为函数
    "co_entry_aarch64:"
#elif defined(__APPLE__) && defined(__MACH__)
    ".global _co_entry_aarch64;" // macOS下的全局符号
    "_co_entry_aarch64:"
#else
#error supported platforms: Linux/FreeBSD/Apple // 不支持的平台
#endif // OS
    // 协程首次启动时，栈上预设了函数指针
    // 此时 sp 指向 co_init 中设置的 rsp[0] (即 func) 的地址
    // rsp[0] = (u64)func;
    // rsp[1] = (u64)func_exit;
    // rsp[2] = (u64)debug_die;
    "ldr x8, [sp, 0];"  // 加载第一个函数指针 (实际执行的协程函数) 到 x8
    "blr x8;"           // 调用该函数 (branch with link to register)
    "ldr x8, [sp, 8];"  // 加载第二个函数指针 (通常是 co_exit0) 到 x8
    "blr x8;"           // 调用该函数
    "ldr x8, [sp, 16];" // 加载第三个函数指针 (通常是 debug_die, 以防万一)
    "blr x8;"           // 调用该函数
    );
#else
#error supported CPUs: x86_64 or AArch64 // 不支持的CPU架构
#endif // co_switch_stack x86_64 and aarch64
// }}} asm

// co {{{ // 协程核心结构和操作
struct co { // 协程控制块结构体
  u64 rsp; // 保存协程的栈指针 (RSP)
  void * priv; // 用户私有数据指针
  u64 * host; // 指向宿主栈指针的指针；如果为NULL，表示协程已退出
  size_t stksz; // 协程栈的大小
};

static __thread struct co * volatile co_curr = NULL; // 线程局部变量，指向当前正在运行的协程；在宿主中为NULL

// 协程栈位于 struct co 结构体之下 (内存地址较低处)
  static void
co_init(struct co * const co, void * func, void * priv, u64 * const host, // 初始化协程
    const u64 stksz, void * func_exit)
{
  debug_assert((stksz & 0x3f) == 0); // 栈大小必须是64字节的倍数
  u64 * rsp = ((u64 *)co) - 4; // 在struct co之上(内存地址较低处)分配空间用于初始函数指针
                               // -4 是因为栈向下生长，co指针指向栈顶，rsp指向栈中内容
                               // rsp[0] = func (协程主函数)
                               // rsp[1] = func_exit (协程退出函数)
                               // rsp[2] = debug_die (备用退出函数)
                               // rsp[3] = 0 (x86_64下，co_switch_stack最后retq的返回地址占位)
  rsp[0] = (u64)func;
  rsp[1] = (u64)func_exit;
  rsp[2] = (u64)debug_die;
  rsp[3] = 0; // for x86_64, this is the "return address" for the last "retq" in co_switch_stack

  rsp -= CO_CONTEXT_SIZE; // 为保存的寄存器上下文分配空间

#if defined(__aarch64__)
  // 对于AArch64，co_switch_stack恢复的第一个地址是lr (链接寄存器)
  // co_entry_aarch64是协程首次执行时的入口点
  rsp[0] = (u64)co_entry_aarch64; // AArch64的上下文保存区域的栈顶是lr (x30)
#endif

  co->rsp = (u64)rsp; // 保存计算好的栈指针
  co->priv = priv; // 设置私有数据
  co->host = host; // 设置宿主栈指针
  co->stksz = stksz; // 设置栈大小
}

  static void
co_exit0(void) // 默认的协程退出函数，调用co_exit并传递返回值0
{
  co_exit(0);
}

  struct co *
co_create(const u64 stacksize, void * func, void * priv, u64 * const host) // 创建一个新的协程
{
  const u64 stksz = bits_round_up(stacksize, 6); // 将栈大小向上取整到64字节的倍数 (2^6)
  const size_t alloc_size = stksz + sizeof(struct co); // 总分配大小 = 栈大小 + 控制块大小
  u8 * const mem = yalloc(alloc_size); // 分配缓存行对齐的内存
  if (mem == NULL) // 分配失败
    return NULL;

#ifdef CO_STACK_CHECK // 如果启用了栈检查
  memset(mem, 0x5c, stksz); // 用特定模式填充栈区域，用于后续检查栈溢出或使用情况
#endif // CO_STACK_CHECK

  struct co * const co = (typeof(co))(mem + stksz); // struct co位于分配内存的高地址部分 (栈在低地址)
  co_init(co, func, priv, host, stksz, co_exit0); // 初始化协程
  return co;
}

  inline void
co_reuse(struct co * const co, void * func, void * priv, u64 * const host) // 重用一个已存在的协程控制块
{
  co_init(co, func, priv, host, co->stksz, co_exit0); // 使用原有栈大小重新初始化
}

  inline struct co *
co_fork(void * func, void * priv) // 从当前协程派生一个新的协程
{
  // 如果当前在协程上下文中，则创建一个新协程，其栈大小和宿主与当前协程相同
  return co_curr ? co_create(co_curr->stksz, func, priv, co_curr->host) : NULL;
}

  inline void *
co_priv(void) // 获取当前协程的私有数据
{
  return co_curr ? co_curr->priv : NULL; // 如果在协程中，返回其priv，否则返回NULL
}

// 宿主调用此函数进入一个协程
  inline u64
co_enter(struct co * const to, const u64 retval) // 进入指定的协程
{
  debug_assert(co_curr == NULL); // 必须从宿主环境进入
  debug_assert(to && to->host); // 目标协程必须有效且有宿主
  u64 * const save = to->host; // 获取宿主栈指针的地址 (用于保存宿主rsp)
  co_curr = to; // 设置当前运行的协程为目标协程
  // 调用汇编函数切换栈：
  // save: 保存宿主rsp的地址
  // to->rsp: 目标协程的rsp
  // retval: 传递给目标协程的返回值 (协程首次启动时，此值会作为参数)
  const u64 ret = co_switch_stack(save, to->rsp, retval);
  co_curr = NULL; // 从协程返回后，当前运行环境变回宿主
  return ret; // 返回协程的退出值
}

// 从一个协程切换到另一个协程
// co_curr 必须有效
// 目标协程将恢复执行并接收 retval
  inline u64
co_switch_to(struct co * const to, const u64 retval) // 切换到另一个协程
{
  debug_assert(co_curr); // 必须在协程上下文中调用
  debug_assert(co_curr != to); // 不能切换到自身
  debug_assert(to && to->host); // 目标协程必须有效
  struct co * const save = co_curr; // 保存当前协程
  co_curr = to; // 设置当前运行协程为目标协程
  // 切换栈：
  // &(save->rsp): 保存当前协程rsp的地址
  // to->rsp: 目标协程的rsp
  // retval: 传递给目标协程的返回值
  return co_switch_stack(&(save->rsp), to->rsp, retval);
}

// 从协程切换回宿主程序
// co_yield 现在是 C++ 的关键字...所以用 co_back
  inline u64
co_back(const u64 retval) // 从当前协程返回到宿主
{
  debug_assert(co_curr); // 必须在协程上下文中
  struct co * const save = co_curr; // 保存当前协程
  co_curr = NULL; // 当前运行环境变回宿主
  // 切换栈：
  // &(save->rsp): 保存当前协程rsp的地址
  // *(save->host): 宿主的rsp (从之前保存的地址中取出)
  // retval: 传递给宿主的返回值
  return co_switch_stack(&(save->rsp), *(save->host), retval);
}

#ifdef CO_STACK_CHECK // 如果启用了栈检查
  static void
co_stack_check(const u8 * const mem, const u64 stksz) // 检查协程栈的使用情况
{
  const u64 * const mem64 = (typeof(mem64))mem; // 将栈内存视为u64数组
  const u64 size64 = stksz / sizeof(u64); // 栈中u64元素的数量
  for (u64 i = 0; i < size64; i++) { // 从栈底开始检查
    if (mem64[i] != 0x5c5c5c5c5c5c5c5clu) { // 如果值不是预设的填充模式
      fprintf(stderr, "%s co stack usage: %lu/%lu\n", __func__, stksz - (i * sizeof(u64)), stksz); // 打印栈使用量
      break; // 找到第一个被修改的位置，即为栈顶大致位置
    }
  }
}
#endif // CO_STACK_CHECK

// 返回到宿主并将host设置为NULL，表示协程已退出
__attribute__((noreturn)) // 此函数不会返回到调用者
  void
co_exit(const u64 retval) // 协程退出
{
  debug_assert(co_curr); // 必须在协程上下文中
#ifdef CO_STACK_CHECK
  const u64 stksz = co_curr->stksz; // 获取栈大小
  u8 * const mem = ((u8 *)co_curr) - stksz; // 计算栈底地址
  co_stack_check(mem, stksz); // 执行栈检查
#endif // CO_STACK_CHECK
  const u64 hostrsp = *(co_curr->host); // 获取宿主栈指针
  co_curr->host = NULL; // 将宿主指针设为NULL，标记协程已退出
  struct co * const save = co_curr; // 保存当前协程
  co_curr = NULL; // 当前运行环境变回宿主
  (void)co_switch_stack(&(save->rsp), hostrsp, retval); // 切换回宿主，并传递返回值
  // 理论上不会执行到这里，因为co_switch_stack会切换到宿主的co_enter函数返回点
  debug_die(); // 如果意外执行到此，则终止程序
}

// 协程退出时 host 被设为 NULL
  inline bool
co_valid(struct co * const co) // 检查协程是否仍然有效 (未退出)
{
  return co->host != NULL; // 通过检查host指针是否为NULL来判断
}

// 在宿主中返回 NULL
  inline struct co *
co_self(void) // 获取当前正在运行的协程的控制块指针
{
  return co_curr; // 返回线程局部的co_curr指针
}

  inline void
co_destroy(struct co * const co) // 销毁协程，释放其内存
{
  u8 * const mem = ((u8 *)co) - co->stksz; // 计算协程内存块的起始地址 (栈底)
  free(mem); // 释放整个内存块
}
// }}} co

// corr {{{ // 基于co的循环调度协程 (run-queue)
struct corr { // 循环协程结构体，包含一个co和一个双向链表指针
  struct co co; // 嵌入的协程控制块
  struct corr * next; // 指向下一个协程
  struct corr * prev; // 指向前一个协程
};

// 初始化并将协程链接到运行队列 (自身形成一个环)
  struct corr *
corr_create(const u64 stacksize, void * func, void * priv, u64 * const host) // 创建并初始化一个循环协程
{
  const u64 stksz = bits_round_up(stacksize, 6); // 栈大小向上取整
  const size_t alloc_size = stksz + sizeof(struct corr); // 总分配大小
  u8 * const mem = yalloc(alloc_size); // 分配内存
  if (mem == NULL)
    return NULL;

#ifdef CO_STACK_CHECK
  memset(mem, 0x5c, stksz); // 栈填充
#endif // CO_STACK_CHECK

  struct corr * const co = (typeof(co))(mem + stksz); // corr结构体位于内存高地址
  co_init(&(co->co), func, priv, host, stksz, corr_exit); // 初始化嵌入的co，退出函数为corr_exit
  co->next = co; // 初始时，next和prev都指向自身，形成一个单元素环
  co->prev = co;
  return co;
}

  struct corr *
corr_link(const u64 stacksize, void * func, void * priv, struct corr * const prev) // 创建新协程并链接到现有协程之后
{
  const u64 stksz = bits_round_up(stacksize, 6); // 栈大小向上取整
  const size_t alloc_size = stksz + sizeof(struct corr); // 总分配大小
  u8 * const mem = yalloc(alloc_size); // 分配内存
  if (mem == NULL)
    return NULL;

#ifdef CO_STACK_CHECK
  memset(mem, 0x5c, stksz); // 栈填充
#endif // CO_STACK_CHECK

  struct corr * const co = (typeof(co))(mem + stksz); // corr结构体位于内存高地址
  // 新协程的宿主与prev协程相同
  co_init(&(co->co), func, priv, prev->co.host, stksz, corr_exit);
  // 将新协程插入到prev和prev->next之间
  co->next = prev->next;
  co->prev = prev;
  co->prev->next = co;
  co->next->prev = co;
  return co;
}

  inline void
corr_reuse(struct corr * const co, void * func, void * priv, u64 * const host) // 重用一个循环协程
{
  co_init(&(co->co), func, priv, host, co->co.stksz, corr_exit); // 重新初始化嵌入的co
  co->next = co; // 重置链表指针，使其自身成环
  co->prev = co;
}

  inline void
corr_relink(struct corr * const co, void * func, void * priv, struct corr * const prev) // 重用并重新链接一个循环协程
{
  co_init(&(co->co), func, priv, prev->co.host, co->co.stksz, corr_exit); // 重新初始化
  // 重新链接到prev之后
  co->next = prev->next;
  co->prev = prev;
  co->prev->next = co;
  co->next->prev = co;
}

  inline void
corr_enter(struct corr * const co) // 从宿主进入指定的循环协程
{
  (void)co_enter(&(co->co), 0); // 调用co_enter，初始参数为0
}

  inline void
corr_yield(void) // 当前协程让出执行权，切换到运行队列中的下一个协程
{
  struct corr * const curr = (typeof(curr))co_curr; // 获取当前corr (co_curr指向嵌入的co)
  if (curr && (curr->next != curr)) // 如果当前在corr上下文中，并且队列中不止一个协程
    (void)co_switch_to(&(curr->next->co), 0); // 切换到下一个协程，传递参数0
}

__attribute__((noreturn)) // 此函数不会返回
  inline void
corr_exit(void) // 循环协程的退出函数
{
  debug_assert(co_curr); // 必须在协程上下文中
#ifdef CO_STACK_CHECK
  const u64 stksz = co_curr->stksz; // 获取栈大小
  const u8 * const mem = ((u8 *)(co_curr)) - stksz; // 计算栈底地址
  co_stack_check(mem, stksz); // 栈检查
#endif // CO_STACK_CHECK

  struct corr * const curr = (typeof(curr))co_curr; // 获取当前corr
  if (curr->next != curr) { // 如果运行队列中还有其他协程
    struct corr * const next = curr->next; // 获取下一个协程
    struct corr * const prev = curr->prev; // 获取前一个协程
    // 从双向链表中移除当前协程
    next->prev = prev;
    prev->next = next;
    curr->next = NULL; // 清理当前协程的链表指针
    curr->prev = NULL;
    curr->co.host = NULL; // 标记嵌入的co为无效 (已退出)
    (void)co_switch_to(&(next->co), 0); // 切换到下一个协程
  } else { // 这是运行队列中的最后一个协程
    co_exit0(); // 调用co_exit0正常退出到宿主
  }
  debug_die(); // 理论上不应执行到此
}

  inline void
corr_destroy(struct corr * const co) // 销毁一个循环协程
{
  co_destroy(&(co->co)); // 调用co_destroy销毁嵌入的co及其栈
}
// }}} corr

// }}} co

// bits {{{ // 位操作函数
  inline u32
bits_reverse_u32(const u32 v) // 反转32位无符号整数的比特位顺序
{
  const u32 v2 = __builtin_bswap32(v); // 字节反转 (e.g., ABCD -> DCBA)
  // 逐级进行位反转
  const u32 v3 = ((v2 & 0xf0f0f0f0u) >> 4) | ((v2 & 0x0f0f0f0fu) << 4); // 半字节内反转 (nibble swap)
  const u32 v4 = ((v3 & 0xccccccccu) >> 2) | ((v3 & 0x33333333u) << 2); // 每2位反转
  const u32 v5 = ((v4 & 0xaaaaaaaau) >> 1) | ((v4 & 0x55555555u) << 1); // 每1位反转 (相邻位交换)
  return v5;
}

  inline u64
bits_reverse_u64(const u64 v) // 反转64位无符号整数的比特位顺序
{
  const u64 v2 = __builtin_bswap64(v); // 字节反转
  const u64 v3 = ((v2 & 0xf0f0f0f0f0f0f0f0lu) >>  4) | ((v2 & 0x0f0f0f0f0f0f0f0flu) <<  4); // 半字节反转
  const u64 v4 = ((v3 & 0xcccccccccccccccclu) >>  2) | ((v3 & 0x3333333333333333lu) <<  2); // 每2位反转
  const u64 v5 = ((v4 & 0xaaaaaaaaaaaaaaaalu) >>  1) | ((v4 & 0x5555555555555555lu) <<  1); // 每1位反转
  return v5;
}

  inline u64
bits_rotl_u64(const u64 v, const u8 n) // 64位循环左移n位
{
  const u8 sh = n & 0x3f; // 移位数对64取模 (0-63)
  return (v << sh) | (v >> (64 - sh)); // 左移sh位，右边空出的位用原数右移(64-sh)位的数填充
}

  inline u64
bits_rotr_u64(const u64 v, const u8 n) // 64位循环右移n位
{
  const u8 sh = n & 0x3f; // 移位数对64取模
  return (v >> sh) | (v << (64 - sh)); // 右移sh位，左边空出的位用原数左移(64-sh)位的数填充
}

  inline u32
bits_rotl_u32(const u32 v, const u8 n) // 32位循环左移n位
{
  const u8 sh = n & 0x1f; // 移位数对32取模 (0-31)
  return (v << sh) | (v >> (32 - sh));
}

  inline u32
bits_rotr_u32(const u32 v, const u8 n) // 32位循环右移n位
{
  const u8 sh = n & 0x1f; // 移位数对32取模
  return (v >> sh) | (v << (32 - sh));
}

  inline u64
bits_p2_up_u64(const u64 v) // 找到大于等于v的最小的2的幂次方
{
  // __builtin_clzl(0) 是未定义的行为，所以v=0或v=1时特殊处理
  return (v > 1) ? (1lu << (64 - __builtin_clzl(v - 1lu))) : v;
  // v-1: 确保如果v本身是2的幂，结果是v。例如v=8, v-1=7 (0111), clzl(7)=61, 64-61=3, 1<<3=8.
  // 如果v=7, v-1=6 (0110), clzl(6)=61, 64-61=3, 1<<3=8.
}

  inline u32
bits_p2_up_u32(const u32 v) // 找到大于等于v的最小的2的幂次方 (32位)
{
  // clz(0) 是未定义的行为
  return (v > 1) ? (1u << (32 - __builtin_clz(v - 1u))) : v;
}

  inline u64
bits_p2_down_u64(const u64 v) // 找到小于等于v的最大的2的幂次方
{
  // 如果v是0，结果是0。否则，最高有效位即为所求。
  return v ? (1lu << (63 - __builtin_clzl(v))) : v;
  // clzl(v) 返回前导零的个数。63 - clzl(v) 是最高有效位的索引 (从0开始)。
}

  inline u32
bits_p2_down_u32(const u32 v) // 找到小于等于v的最大的2的幂次方 (32位)
{
  return v ? (1u << (31 - __builtin_clz(v))) : v;
}

  inline u64
bits_round_up(const u64 v, const u8 power) // 将v向上取整到最接近的 2^power 的倍数
{
  // (v + (1<<power) - 1) / (1<<power) * (1<<power)
  // 等价于 (v + mask) & ~mask  其中 mask = (1<<power) - 1
  return (v + (1lu << power) - 1lu) >> power << power;
}

  inline u64
bits_round_up_a(const u64 v, const u64 a) // 将v向上取整到最接近的 a 的倍数 (a不必是2的幂)
{
  return (v + a - 1) / a * a;
}

  inline u64
bits_round_down(const u64 v, const u8 power) // 将v向下取整到最接近的 2^power 的倍数
{
  return v >> power << power; // 直接清除低 power 位
}

  inline u64
bits_round_down_a(const u64 v, const u64 a) // 将v向下取整到最接近的 a 的倍数
{
  return v / a * a;
}
// }}} bits

// simd {{{ // SIMD (单指令多数据) 相关操作
// 从128位向量v中提取每个字节的最高位，组成一个16位掩码 (最多0xffff)
  u32
m128_movemask_u8(const m128 v)
{
#if defined(__x86_64__)
  return (u32)_mm_movemask_epi8(v); // 使用x86 SSE指令 _mm_movemask_epi8
#elif defined(__aarch64__)
  // AArch64 Neon 实现 movemask_epi8
  // vtbl 用于重新排列字节，使得可以高效地与位掩码进行与操作并求和
  static const m128 vtbl = {0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15}; // 查表指令的表，用于字节重排
  // mbits 用于提取每个字节的最高位并将其映射到结果掩码的不同位
  // 每个u16元素代表两个字节的最高位贡献 (0x0101 -> 第一个字节贡献1，第二个字节贡献256，这里只用低位)
  static const uint16x8_t mbits = {0x0101, 0x0202, 0x0404, 0x0808, 0x1010, 0x2020, 0x4040, 0x8080};
  const m128 perm = vqtbl1q_u8(v, vtbl); // 字节查表重排，v是输入向量，vtbl是索引表
                                         // 这一步将输入向量v的字节按vtbl的顺序重新排列
                                         // 目的是将每个原始字节的最高位提取出来并放到合适的位置，以便后续操作
  // 将perm重新解释为uint16x8_t，与mbits按位与，然后水平加和得到最终的掩码
  // vandq_u16: 按位与。vreinterpretq_u16_u8: 将u8x16向量重新解释为u16x8向量。
  // vaddvq_u16: 水平加和u16x8向量中的所有元素，得到一个u16结果。
  // 假设v的字节最高位是b_i (0或1)。
  // 经过vqtbl1q_u8和vandq_u16后，目标是让每个u16元素包含对应原始字节最高位的信息，并乘以相应的权重。
  // 例如，如果v的第0个字节最高位为1，则结果的第0位为1。
  // 如果v的第1个字节最高位为1，则结果的第1位为1。
  // 这个实现比较复杂，核心思想是通过SIMD操作高效提取所有字节的最高位并组合成一个整数。
  return (u32)vaddvq_u16(vandq_u16(vreinterpretq_u16_u8(perm), mbits));
#endif // __x86_64__
}

//// 从128位向量v中提取每个16位字的最高位，组成一个8位掩码 (最多0xff)
//  u32
//m128_movemask_u16(const m128 v)
//{
//#if defined(__x86_64__)
//  // _mm_movemask_epi8 提取所有字节的最高位，然后用 _pext_u32 提取所需位
//  // 0xaaaa = 1010101010101010, 提取偶数位字节(即每个u16的高字节)的最高位
//  return _pext_u32((u32)_mm_movemask_epi8(v), 0xaaaau);
//#elif defined(__aarch64__)
//  static const uint16x8_t mbits = {1, 2, 4, 8, 16, 32, 64, 128}; // 每个16位字的掩码位
//  // 将v重新解释为uint16x8_t，与mbits按位与，然后水平加和
//  // 这里假设v中每个u16的最高位已经被移到了某个固定位置，然后通过与mbits相与并求和来构造掩码
//  // 这段代码可能不正确，因为直接对v进行vreinterpret和vand可能无法正确提取每个u16的最高位
//  return (u32)vaddvq_u16(vandq_u16(vreinterpretq_u16_u8(v), mbits));
//#endif // __x86_64__
//}
//
//// 从128位向量v中提取每个32位字的最高位，组成一个4位掩码 (最多0xf)
//  u32
//m128_movemask_u32(const m128 v)
//{
//#if defined(__x86_64__)
//  // 0x8888 = 1000100010001000, 提取每4个字节中第一个字节(即每个u32的高字节)的最高位
//  return _pext_u32((u32)_mm_movemask_epi8(v), 0x8888u);
//#elif defined(__aarch64__)
//  static const uint32x4_t mbits = {1, 2, 4, 8}; // 每个32位字的掩码位
//  // 类似于u16版本，这段代码的正确性也存疑
//  return (u32)vaddvq_u32(vandq_u32(vreinterpretq_u32_u8(v), mbits));
//#endif // __x86_64__
//}
// }}} simd

// vi128 {{{ // 可变长度整数编码 (类似protobuf varint)
#if defined(__GNUC__) && __GNUC__ >= 7 // GCC 7及以上版本支持 __attribute__((fallthrough))
#define FALLTHROUGH __attribute__ ((fallthrough)) // 明确标记switch-case的穿透行为
#else
#define FALLTHROUGH ((void)0) // 其他编译器或旧版GCC，定义为空操作
#endif /* __GNUC__ >= 7 */

  inline u32
vi128_estimate_u32(const u32 v) // 估算u32整数v进行vi128编码后所需的字节数
{
  // t是一个查找表，索引是v的前导零个数 (clz)
  // clz(v) | 有效位数 | 编码长度 | t[]索引范围
  // --------|------------|------------|---------------
  // 0-3     | 29-32      | 5 bytes    | 0-3
  // 4-10    | 22-28      | 4 bytes    | 4-10
  // 11-17   | 15-21      | 3 bytes    | 11-17
  // 18-24   | 8-14       | 2 bytes    | 18-24
  // 25-31   | 1-7        | 1 byte     | 25-31
  // v == 0 (clz=32) | 0  | 2 bytes    | 特殊处理 (编码为0x80 0x00)
  static const u8 t[] = {5,5,5,5, // clz 0-3 (对应有效位数 32-29)
    4,4,4,4,4,4,4,             // clz 4-10 (对应有效位数 28-22)
    3,3,3,3,3,3,3,             // clz 11-17 (对应有效位数 21-15)
    2,2,2,2,2,2,2,             // clz 18-24 (对应有效位数 14-8)
    1,1,1,1,1,1,1};            // clz 25-31 (对应有效位数 7-1)
  return v ? t[__builtin_clz(v)] : 2; // 如果v不为0，查表；如果v为0，返回2 (编码为0x80 0x00)
  // 编码0时，v=0, __builtin_clz(0)未定义，所以特殊处理。
  // 编码为0x80 0x00: 第一个字节0x80 (v=0, v|0x80 = 0x80), v>>=7 (v=0). 第二个字节0x00 (v=0).
}

  u8 *
vi128_encode_u32(u8 * dst, u32 v) // 将u32整数v编码到dst，返回编码后的指针位置
{
  switch (vi128_estimate_u32(v)) { // 根据估算的长度进行编码
  case 5: // 需要5字节 (v >= 2^28)
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH; // 写入低7位，并设置最高位为1 (表示后面还有字节)，然后右移7位
  case 4: // 需要4字节 (v >= 2^21, 或从5字节穿透下来)
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 3: // 需要3字节 (v >= 2^14)
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 2: // 需要2字节 (v >= 2^7, 或者 v=0 时，第一次v=0, *(dst++)=0x80; 第二次v=0, *(dst++)=0x00)
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 1: // 需要1字节 (v < 2^7, 最后一个字节，最高位为0)
    *(dst++) = (u8)v;
    break;
  default: // 不应发生
    debug_die();
    break;
  }
  return dst;
}

  const u8 *
vi128_decode_u32(const u8 * src, u32 * const out) // 从src解码u32整数到out，返回解码后的指针位置
{
  debug_assert(*src); // 第一个字节不能是0 (除非要解码的数就是0，但0编码为0x80 0x00，所以第一个字节是0x80)
                      // 如果解码的数是0，那么src会是 [0x80, 0x00, ...]。
                      // 如果src指向的是单个0x00字节，这不符合vi128编码规则（除了0x00本身代表0）。
                      // 此断言可能假设src总是指向一个有效的vi128编码序列的开始。
  u32 r = 0; // 解码结果
  for (u32 shift = 0; shift < 32; shift += 7) { // 最多解码5个字节 (对应32位整数，5*7=35 > 32)
    const u8 byte = *(src++); // 读取一个字节
    r |= (((u32)(byte & 0x7f)) << shift); // 将低7位添加到结果中，并左移相应位数
    if ((byte & 0x80) == 0) { // 如果最高位为0，表示这是最后一个字节
      *out = r; // 保存结果
      return src; // 返回解码后的指针
    }
  }
  // 如果循环结束仍未遇到最高位为0的字节 (例如编码超过5字节)，则认为编码无效
  *out = 0; // 无效时输出0
  return NULL; // 返回NULL表示解码失败
}

  inline u32
vi128_estimate_u64(const u64 v) // 估算u64整数v进行vi128编码后所需的字节数
{
  // 类似于u32的估算表
  // clz(v) | 有效位数 | 编码长度 | t[]索引范围
  // --------|------------|------------|---------------
  // 0       | 64         | 10 bytes   | 0
  // 1-7     | 63-57      | 9 bytes    | 1-7
  // ...
  // 50-56   | 14-8       | 2 bytes    | 50-56
  // 57-63   | 7-1        | 1 byte     | 57-63
  // v == 0 (clz=64) | 0  | 2 bytes    | 特殊处理
  static const u8 t[] = {10, // clz 0
    9,9,9,9,9,9,9,       // clz 1-7
    8,8,8,8,8,8,8,       // clz 8-14
    7,7,7,7,7,7,7,       // clz 15-21
    6,6,6,6,6,6,6,       // clz 22-28
    5,5,5,5,5,5,5,       // clz 29-35
    4,4,4,4,4,4,4,       // clz 36-42
    3,3,3,3,3,3,3,       // clz 43-49
    2,2,2,2,2,2,2,       // clz 50-56
    1,1,1,1,1,1,1};      // clz 57-63
  return v ? t[__builtin_clzl(v)] : 2; // v为0时编码为0x80 0x00
}

// 返回编码后的指针位置
  u8 *
vi128_encode_u64(u8 * dst, u64 v) // 将u64整数v编码到dst
{
  switch (vi128_estimate_u64(v)) { // 根据估算长度进行编码
  case 10: // v >= 2^63
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 9: // v >= 2^56
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 8: // v >= 2^49
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 7: // v >= 2^42
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 6: // v >= 2^35
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 5: // v >= 2^28
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 4: // v >= 2^21
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 3: // v >= 2^14
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 2: // v >= 2^7, 或者 v=0
    *(dst++) = (u8)(v | 0x80); v >>= 7; FALLTHROUGH;
  case 1: // v < 2^7
    *(dst++) = (u8)v;
    break;
  default:
    debug_die();
    break;
  }
  return dst;
}

// 返回解码后的指针位置
  const u8 *
vi128_decode_u64(const u8 * src, u64 * const out) // 从src解码u64整数到out
{
  u64 r = 0; // 解码结果
  for (u32 shift = 0; shift < 64; shift += 7) { // 最多解码10个字节 (10*7=70 > 64)
    const u8 byte = *(src++); // 读取一个字节
    r |= (((u64)(byte & 0x7f)) << shift); // 将低7位添加到结果
    if ((byte & 0x80) == 0) { // 如果是最后一个字节
      *out = r; // 保存结果
      return src; // 返回解码后指针
    }
  }
  // 编码无效 (例如超过10字节)
  *out = 0;
  return NULL;
}

#undef FALLTHROUGH // 取消FALLTHROUGH宏定义
// }}} vi128

// misc {{{ // 其他杂项工具函数
  inline struct entry13
entry13(const u16 e1, const u64 e3) // 创建一个entry13结构体 (将u16和48位u64压缩到64位)
{
  debug_assert((e3 >> 48) == 0); // 确保e3的高16位为0 (即e3只使用低48位)
  // 将e3左移16位，然后用或操作将e1放入低16位
  return (struct entry13){.v64 = (e3 << 16) | e1};
}

  inline void
entry13_update_e3(struct entry13 * const e, const u64 e3) // 更新entry13结构中的e3部分
{
  debug_assert((e3 >> 48) == 0); // 同样检查e3的范围
  *e = entry13(e->e1, e3); // 保留原有的e1，更新e3
}

  inline void *
u64_to_ptr(const u64 v) // 将u64整数转换为指针
{
  return (void *)v;
}

  inline u64
ptr_to_u64(const void * const ptr) // 将指针转换回u64整数
{
  return (u64)ptr;
}

// 可移植的 malloc_usable_size 实现
  inline size_t
m_usable_size(void * const ptr) // 获取ptr指向的内存块的实际可用大小
{
#if defined(__linux__) || defined(__FreeBSD__)
  const size_t sz = malloc_usable_size(ptr); // Linux/FreeBSD 使用 malloc_usable_size
#elif defined(__APPLE__) && defined(__MACH__)
  const size_t sz = malloc_size(ptr); // macOS 使用 malloc_size
#endif // OS

#ifndef HEAPCHECKING // 如果不是在进行堆检查 (如Valgrind, ASan)
  // valgrind 和 asan 可能返回未对齐的可用大小
  debug_assert((sz & 0x7lu) == 0); // 正常情况下，可用大小应该是8字节对齐的
#endif // HEAPCHECKING

  return sz;
}

  inline size_t
fdsize(const int fd) // 获取文件描述符对应的文件或设备的大小
{
  struct stat st; // 文件状态结构体
  st.st_size = 0; // 初始化大小
  if (fstat(fd, &st) != 0) // 获取文件状态
    return 0; // 获取失败返回0

  if (S_ISBLK(st.st_mode)) { // 如果是块设备
#if defined(__linux__)
    ioctl(fd, BLKGETSIZE64, &st.st_size); // Linux: 使用BLKGETSIZE64 ioctl获取64位大小 (字节)
#elif defined(__APPLE__) && defined(__MACH__)
    u64 blksz = 0; // 块大小
    u64 nblks = 0; // 块数量
    ioctl(fd, DKIOCGETBLOCKSIZE, &blksz); // macOS: 获取块大小
    ioctl(fd, DKIOCGETBLOCKCOUNT, &nblks); // macOS: 获取块数量
    st.st_size = (ssize_t)(blksz * nblks); // 计算总大小
#elif defined(__FreeBSD__)
    ioctl(fd, DIOCGMEDIASIZE, &st.st_size); // FreeBSD: 使用DIOCGMEDIASIZE ioctl获取介质大小 (字节)
#endif // OS
  }
  // 如果是普通文件，st.st_size已由fstat填充
  return (size_t)st.st_size;
}

  u32
memlcp(const u8 * const p1, const u8 * const p2, const u32 max) // 计算两个内存区域p1和p2的最长公共前缀 (Longest Common Prefix)
{
  const u32 max64 = max & (~7u); // 最多可以按64位比较的字节数 (8字节对齐)
  u32 clen = 0; // 公共前缀长度
  while (clen < max64) { // 按8字节块比较
    const u64 v1 = *(const u64 *)(p1+clen);
    const u64 v2 = *(const u64 *)(p2+clen);
    const u64 x = v1 ^ v2; // 异或：相同为0，不同为1
    if (x) // 如果x不为0，说明这8字节中有不同
      // __builtin_ctzl(x) 返回x末尾0的个数 (即第一个不同比特位的位置)
      // 右移3位 (除以8) 得到第一个不同字节的偏移量
      return clen + (u32)(__builtin_ctzl(x) >> 3);
    clen += sizeof(u64); // 相同，继续比较下一个8字节块
  }

  // 比较剩余的不足8字节的部分 (尝试按4字节比较)
  if ((clen + sizeof(u32)) <= max) {
    const u32 v1 = *(const u32 *)(p1+clen);
    const u32 v2 = *(const u32 *)(p2+clen);
    const u32 x = v1 ^ v2;
    if (x)
      return clen + (u32)(__builtin_ctz(x) >> 3); // ctz用于32位
    clen += sizeof(u32);
  }

  // 逐字节比较剩余部分
  while ((clen < max) && (p1[clen] == p2[clen]))
    clen++;
  return clen; // 返回总的公共前缀长度
}

static double logger_t0 = 0.0; // 日志记录的起始时间

__attribute__((constructor)) // 构造函数，在main之前执行
  static void
logger_init(void) // 初始化日志记录器
{
  logger_t0 = time_sec(); // 记录程序启动时的秒级时间戳
}

__attribute__ ((format (printf, 2, 3))) // 告诉编译器此函数参数类似printf (格式字符串在第2个参数，可变参数从第3个开始)
  void
logger_printf(const int fd, const char * const fmt, ...) // 带时间戳和线程ID的日志打印函数
{
  char buf[4096]; // 格式化缓冲区
  va_list ap; // 可变参数列表
  va_start(ap, fmt); // 初始化可变参数
  vsnprintf(buf, sizeof(buf), fmt, ap); // 将可变参数格式化到buf中
  va_end(ap); // 结束可变参数处理
  // 输出格式: 时间差(秒.毫秒) 线程ID的CRC32C校验和 日志内容
  dprintf(fd, "%010.3lf %08x %s", time_diff_sec(logger_t0), crc32c_u64(0x12345678, (u64)pthread_self()), buf);
}
// }}} misc

// bitmap {{{ // 位图实现
// 部分线程安全的位图；可称之为最终一致性？
struct bitmap {
  u64 nbits; // 位图中总的比特数
  u64 nbytes; // 位图占用的字节数 (必须是8的倍数)
  union {
    u64 ones; // 非原子访问的1的个数 (用于单线程或外部同步场景)
    au64 ones_a; // 原子访问的1的个数 (用于并发更新场景，如bitmap_set1_safe64)
  };
  u64 bm[0]; // 柔性数组成员，实际的位图数据存储区域 (u64数组)
};

  inline void
bitmap_init(struct bitmap * const bm, const u64 nbits) // 初始化位图结构
{
  bm->nbits = nbits; // 设置总比特数
  bm->nbytes = bits_round_up(nbits, 6) >> 3; // 计算所需字节数 (向上取整到64位边界，即8字节的倍数)
                                             // (nbits + 63) / 64 * 8
  bm->ones = 0; // 初始时1的个数为0
  bitmap_set_all0(bm); // 将所有位设置为0
}

  inline struct bitmap *
bitmap_create(const u64 nbits) // 创建并初始化一个新的位图
{
  const u64 nbytes = bits_round_up(nbits, 6) >> 3; // 计算所需字节数
  struct bitmap * const bm = malloc(sizeof(*bm) + nbytes); // 分配内存 (结构体大小 + 位图数据大小)
  if (bm) // 如果分配成功
    bitmap_init(bm, nbits); // 初始化位图
  return bm;
}

  static inline bool
bitmap_test_internal(const struct bitmap * const bm, const u64 idx) // 内部函数：测试指定索引的位是否为1 (不检查idx范围)
{
  // idx >> 6 定位到包含该位的u64元素
  // 1lu << (idx & 0x3flu) 创建一个掩码，目标位为1，其余为0 (idx & 0x3f 即 idx % 64)
  return (bm->bm[idx >> 6] & (1lu << (idx & 0x3flu))) != 0; // 按位与并检查结果是否非0
}

  inline bool
bitmap_test(const struct bitmap * const bm, const u64 idx) // 测试指定索引的位是否为1 (检查idx范围)
{
  return (idx < bm->nbits) && bitmap_test_internal(bm, idx); // 必须在范围内才测试
}

  inline bool
bitmap_test_all1(struct bitmap * const bm) // 测试位图中的所有位是否都为1
{
  return bm->ones == bm->nbits; // 通过比较1的个数和总比特数来判断
}

  inline bool
bitmap_test_all0(struct bitmap * const bm) // 测试位图中的所有位是否都为0
{
  return bm->ones == 0; // 通过检查1的个数是否为0来判断
}

  inline void
bitmap_set1(struct bitmap * const bm, const u64 idx) // 将指定索引的位设置为1 (非线程安全更新ones计数)
{
  if ((idx < bm->nbits) && !bitmap_test_internal(bm, idx)) { // 如果在范围内且当前位为0
    debug_assert(bm->ones < bm->nbits); // ones计数不应超过总位数
    bm->bm[idx >> 6] |= (1lu << (idx & 0x3flu)); // 设置位为1
    bm->ones++; // 增加1的计数
  }
}

  inline void
bitmap_set0(struct bitmap * const bm, const u64 idx) // 将指定索引的位设置为0 (非线程安全更新ones计数)
{
  if ((idx < bm->nbits) && bitmap_test_internal(bm, idx)) { // 如果在范围内且当前位为1
    debug_assert(bm->ones && (bm->ones <= bm->nbits)); // ones计数应大于0且不超限
    bm->bm[idx >> 6] &= ~(1lu << (idx & 0x3flu)); // 设置位为0 (与目标位的反码进行与操作)
    bm->ones--; // 减少1的计数
  }
}

// 用于哈希表等场景：每个线程对一个64位范围有独占访问权，但并发更新bm->ones_a
// 使用原子 +/- 来更新 bm->ones_a
  inline void
bitmap_set1_safe64(struct bitmap * const bm, const u64 idx) // 线程安全地将指定索引的位设置为1 (假设对该64位块独占，但ones_a原子更新)
{
  if ((idx < bm->nbits) && !bitmap_test_internal(bm, idx)) { // 如果在范围内且当前位为0
    debug_assert(bm->ones < bm->nbits); // ones_a的原子性保证了这里ones的读取是近似的
    bm->bm[idx >> 6] |= (1lu << (idx & 0x3flu)); // 设置位 (非原子，依赖外部同步或独占访问假设)
    (void)atomic_fetch_add_explicit(&bm->ones_a, 1, MO_RELAXED); // 原子增加ones_a计数 (松散内存序即可)
  }
}

  inline void
bitmap_set0_safe64(struct bitmap * const bm, const u64 idx) // 线程安全地将指定索引的位设置为0
{
  if ((idx < bm->nbits) && bitmap_test_internal(bm, idx)) { // 如果在范围内且当前位为1
    debug_assert(bm->ones && (bm->ones <= bm->nbits)); // ones_a的原子性保证了这里ones的读取是近似的
    bm->bm[idx >> 6] &= ~(1lu << (idx & 0x3flu)); // 设置位 (非原子)
    (void)atomic_fetch_sub_explicit(&bm->ones_a, 1, MO_RELAXED); // 原子减少ones_a计数
  }
}

  inline u64
bitmap_count(struct bitmap * const bm) // 获取位图中1的个数 (读取非原子ones成员)
{
  return bm->ones;
}

  inline u64
bitmap_first(struct bitmap * const bm) // 查找并返回位图中第一个为1的位的索引
{
  for (u64 i = 0; (i << 6) < bm->nbits; i++) { // 遍历每个u64元素
    if (bm->bm[i]) // 如果当前u64元素不为0 (即包含至少一个1)
      // (i << 6) 是当前u64块的起始索引
      // __builtin_ctzl(bm->bm[i]) 返回该u64元素末尾0的个数，即第一个1在块内的偏移
      return (i << 6) + (u32)__builtin_ctzl(bm->bm[i]);
  }
  debug_die(); // 如果没有找到1 (位图全0)，则程序异常终止 (不应发生，除非逻辑错误)
}

  inline void
bitmap_set_all1(struct bitmap * const bm) // 将位图中的所有位设置为1
{
  memset(bm->bm, 0xff, bm->nbytes); // 将所有字节设置为0xff
  bm->ones = bm->nbits; // 更新1的计数
}

  inline void
bitmap_set_all0(struct bitmap * const bm) // 将位图中的所有位设置为0
{
  memset(bm->bm, 0, bm->nbytes); // 将所有字节设置为0
  bm->ones = 0; // 更新1的计数
}
// }}} bitmap

// astk {{{ // 原子栈 (Atomic Stack) 实现，用于无锁的生产者-消费者场景
// 原子栈中的单元结构
struct acell { struct acell * next; }; // 简单的链表节点，包含指向下一个节点的指针

// 从magic值m中提取指针部分
  static inline struct acell *
astk_ptr(const u64 m) // magic值的低16位是序列号，高48位是指针
{
  return (struct acell *)(m >> 16); // 右移16位得到指针
}

// 计算新的magic值 (序列号加1，指针更新)
  static inline u64
astk_m1(const u64 m0, struct acell * const ptr) // m0是旧magic值，ptr是新栈顶指针
{
  // (m0 + 1) & 0xfffflu : 序列号加1并对2^16取模 (防止溢出到指针部分)
  // (((u64)ptr) << 16) : 新指针左移16位
  // 两者或运算得到新magic值
  return ((m0 + 1) & 0xfffflu) | (((u64)ptr) << 16);
}

// 计算新的magic值 (仅更新指针，序列号保持不变或不关心，用于非ABA保护场景)
  static inline u64
astk_m1_unsafe(struct acell * const ptr) // ptr是新栈顶指针
{
  return ((u64)ptr) << 16; // 直接将指针左移16位作为magic值 (序列号部分为0)
}

  static bool
astk_try_push(au64 * const pmagic, struct acell * const first, struct acell * const last) // 尝试将一个链表 (first到last) 压入原子栈
{
  // pmagic: 指向原子magic值的指针
  // first: 要压入链表的头节点
  // last: 要压入链表的尾节点
  u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic值 (MO_CONSUME确保后续依赖此值的操作正确)
  last->next = astk_ptr(m0); // 将新链表的尾节点的next指向原子栈原有的栈顶
  const u64 m1 = astk_m1(m0, first); // 计算新的magic值 (新栈顶是first，序列号加1)
  // 尝试用CAS原子更新magic值，如果pmagic当前值仍为m0，则更新为m1
  // MO_RELEASE: 确保push操作之前对链表内容的修改对其他线程可见
  // MO_RELAXED: CAS失败时加载内存序 (因为如果失败，m0会被更新为当前值，用于下次重试)
  return atomic_compare_exchange_weak_explicit(pmagic, &m0, m1, MO_RELEASE, MO_RELAXED);
}

  static void
astk_push_safe(au64 * const pmagic, struct acell * const first, struct acell * const last) // 安全地将链表压入原子栈 (循环直到成功)
{
  while (!astk_try_push(pmagic, first, last)); // 持续尝试直到push成功
}

  static void
astk_push_unsafe(au64 * const pmagic, struct acell * const first, // 非线程安全地将链表压入栈 (用于单生产者或外部同步场景)
    struct acell * const last)
{
  const u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic值
  last->next = astk_ptr(m0); // 设置链表尾的next
  const u64 m1 = astk_m1_unsafe(first); // 计算新magic值 (不增加序列号) first指针的前48位
  atomic_store_explicit(pmagic, m1, MO_RELAXED); // 直接存储新magic值 (松散内存序)
}

//// 尝试从原子栈弹出一个元素
//// 可能因两种原因失败：(1) 返回NULL: 栈空；(2) 返回~0lu: CAS竞争失败
//  static void *
//astk_try_pop(au64 * const pmagic)
//{
//  u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic
//  struct acell * const ret = astk_ptr(m0); // 获取当前栈顶指针
//  if (ret == NULL) // 如果栈空
//    return NULL;
//
//  const u64 m1 = astk_m1(m0, ret->next); // 计算新magic (新栈顶是ret->next，序列号加1)
//  // 尝试CAS更新magic值
//  // MO_ACQUIRE: 确保成功pop后，后续对弹出元素的操作能看到其最新状态
//  if (atomic_compare_exchange_weak_explicit(pmagic, &m0, m1, MO_ACQUIRE, MO_RELAXED))
//    return ret; // CAS成功，返回弹出的元素
//  else
//    return (void *)(~0lu); // CAS失败 (通常因为竞争)，返回特殊值表示重试
//}

  static void *
astk_pop_safe(au64 * const pmagic) // 安全地从原子栈弹出一个元素 (循环直到成功或栈空)
{
  do {
    u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic
    struct acell * const ret = astk_ptr(m0); // 获取当前栈顶
    if (ret == NULL) // 如果栈空
      return NULL;

    const u64 m1 = astk_m1(m0, ret->next); // 计算新magic
    // 尝试CAS更新
    if (atomic_compare_exchange_weak_explicit(pmagic, &m0, m1, MO_ACQUIRE, MO_RELAXED))
      return ret; // 成功，返回元素
    // CAS失败则循环重试
  } while (true);
}

  static void *
astk_pop_unsafe(au64 * const pmagic) // 非线程安全地从栈弹出一个元素
{
  const u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic
  struct acell * const ret = astk_ptr(m0); // 获取栈顶
  if (ret == NULL) // 栈空
    return NULL;

  const u64 m1 = astk_m1_unsafe(ret->next); // 计算新magic (不增加序列号)
  atomic_store_explicit(pmagic, m1, MO_RELAXED); // 直接存储
  return (void *)ret;
}

  static void *
astk_peek_unsafe(au64 * const pmagic) // 非线程安全地查看栈顶元素 (不弹出)
{
  const u64 m0 = atomic_load_explicit(pmagic, MO_CONSUME); // 读取当前magic
  return astk_ptr(m0); // 返回栈顶指针
}
// }}} astk

// slab {{{ // Slab分配器实现
#define SLAB_OBJ0_OFFSET ((64)) // slab块中第一个对象的默认偏移量 (64字节，通常用于缓存行对齐)
struct slab { // Slab分配器控制结构
  au64 magic; // 原子magic值，用于无锁弹出对象 (高48位是指向空闲对象链表的指针，低16位是序列号)
  u64 padding1[7]; // 填充到缓存行边界 (64字节)

  // 第二个缓存行
  struct acell * head_active; // 活跃块链表头：包含正在使用或其对象在magic空闲链表中的块
  struct acell * head_backup; // 备份块链表头：包含未使用的、已满的空闲对象块 (等待复用)
  u64 nr_ready; // UNSAFE模式下可用！magic空闲链表中的对象数量 (非原子，仅用于非安全模式)
  u64 padding2[5]; // 填充

  // 第三个缓存行 (常量数据)
  u64 obj_size; // const: 每个对象的大小 (对齐后)
  u64 blk_size; // const: 每个内存块 (slab) 的大小
  u64 objs_per_slab; // const: 每个slab块中包含的对象数量
  u64 obj0_offset; // const: slab块中第一个对象的偏移量
  u64 padding3[4]; // 填充

  // 第四个缓存行
  union { // 用于扩展slab时加锁
    mutex lock; // 互斥锁
    u64 padding4[8]; // 填充确保mutex在自己的缓存行
  };
};
static_assert(sizeof(struct slab) == 256, "sizeof(struct slab) != 256"); // 确保slab结构大小为256字节 (4个缓存行)

  static void
slab_add(struct slab * const slab, struct acell * const blk, const bool is_safe) // 将一个新分配的块(blk)加入slab，并将其中的对象加入空闲链表
{
  // 将块插入到活跃块链表头部
  blk->next = slab->head_active;
  slab->head_active = blk;

  // 获取块中第一个对象的起始地址
  u8 * const base = ((u8 *)blk) + slab->obj0_offset;
  struct acell * iter = (typeof(iter))base; // iter指向第一个对象
  // 将块内所有对象串成一个链表
  for (u64 i = 1; i < slab->objs_per_slab; i++) {
    struct acell * const next = (typeof(next))(base + (i * slab->obj_size)); // 计算下一个对象的地址
    iter->next = next; // 当前对象的next指向下一个对象
    iter = next; // iter移动到下一个对象
  }
  // 循环结束后，base指向链表头 (第一个对象)，iter指向链表尾 (最后一个对象)

  if (is_safe) { // 如果是线程安全模式 (其他线程可能同时从magic中pop)
    astk_push_safe(&slab->magic, (struct acell *)base, iter); // 安全地将对象链表压入原子栈
  } else { // 非安全模式 (单线程或外部同步)
    astk_push_unsafe(&slab->magic, (struct acell *)base, iter); // 非安全地压入
    slab->nr_ready += slab->objs_per_slab; // 更新可用对象计数 (非原子)
  }
}

// 临界区；调用时需持有锁
  static bool
slab_expand(struct slab * const slab, const bool is_safe) // 扩展slab，增加更多可用对象 (加锁执行)
{
  struct acell * const old = slab->head_backup; // 尝试从备份链表中获取一个已分配但未使用的块
  if (old) { // 如果备份链表不为空
    slab->head_backup = old->next; // 从备份链表中移除一个块
    slab_add(slab, old, is_safe); // 将该块加入slab (其对象加入空闲链表)
  } else { // 备份链表为空，需要分配新的内存块
    size_t blk_size_actual; // 实际分配到的块大小 (可能因大页对齐而变化)
    // 尝试以最佳方式分配一个新块 (优先大页)
    struct acell * const new_blk = pages_alloc_best(slab->blk_size, true, &blk_size_actual);
    (void)blk_size_actual; // 暂时未使用实际分配大小 (假设与请求大小一致或兼容) //（void 这是一个C语言中非常常见的编程技巧，其唯一目的是告诉编译器：“我知道这个变量存在，但我故意不使用它，请不要因此产生编译警告。”
    if (new_blk == NULL) // 分配失败
      return false;

    slab_add(slab, new_blk, is_safe); // 将新分配的块加入slab
  }
  return true; // 扩展成功
}

// 成功返回obj0_offset，失败返回0
  static u64
slab_check_sizes(const u64 obj_size, const u64 blk_size) // 检查对象大小和块大小的有效性
{
  // 对象大小必须非0且8字节对齐
  // 块大小必须至少为页大小(4KB)且是2的幂次方
  if ((!obj_size) || (obj_size % 8lu) || (blk_size < 4096lu) || (blk_size & (blk_size - 1)))
    return 0; // 无效参数

  // 计算第一个对象的偏移量：如果对象大小不是2的幂，则使用默认偏移SLAB_OBJ0_OFFSET (64)
  // 否则，如果对象大小是2的幂，可以使用对象大小自身作为偏移 (例如，对象大小为64，偏移也为64)
  // 目的是确保对象至少是缓存行对齐的，或者如果对象本身很大且对齐，则直接使用其大小。
  const u64 obj0_offset = (obj_size & (obj_size - 1)) ? SLAB_OBJ0_OFFSET : obj_size;
  // 确保偏移量在块内，并且块内至少能容纳一个对象
  if (obj0_offset >= blk_size || (blk_size - obj0_offset) < obj_size)
    return 0;

  return obj0_offset; // 返回计算出的有效对象起始偏移
}

  static void
slab_init_internal(struct slab * const slab, const u64 obj_size, const u64 blk_size, const u64 obj0_offset) // 内部初始化slab结构体
{
  memset(slab, 0, sizeof(*slab)); // 清零结构体
  slab->obj_size = obj_size; // 设置对象大小
  slab->blk_size = blk_size; // 设置块大小
  // 计算每个块能容纳的对象数量
  slab->objs_per_slab = (blk_size - obj0_offset) / obj_size;
  debug_assert(slab->objs_per_slab); // 每个块至少能放一个对象
  slab->obj0_offset = obj0_offset; // 设置对象起始偏移
  mutex_init(&(slab->lock)); // 初始化互斥锁
}

  struct slab *
slab_create(const u64 obj_size, const u64 blk_size) // 创建并初始化一个slab分配器
{
  const u64 obj0_offset = slab_check_sizes(obj_size, blk_size); // 检查参数并获取对象偏移
  if (!obj0_offset) // 参数无效
    return NULL;

  struct slab * const slab = yalloc(sizeof(*slab)); // 分配slab控制结构内存 (缓存行对齐) sizeof(*slab) = 256
  if (slab == NULL) // 分配失败
    return NULL;

  slab_init_internal(slab, obj_size, blk_size, obj0_offset); // 初始化slab
  return slab;
}

// 非安全模式：预留至少nr个对象 (如果不足则尝试扩展)
  bool
slab_reserve_unsafe(struct slab * const slab, const u64 nr)
{
  while (slab->nr_ready < nr) { // 如果当前可用对象数小于所需数量
    if (!slab_expand(slab, false)) // 尝试扩展 (非安全模式，不加锁，依赖外部同步)
      return false; // 扩展失败
  }
  return true; // 预留成功
}

  void *
slab_alloc_unsafe(struct slab * const slab) // 非安全模式下分配一个对象
{
  void * ret = astk_pop_unsafe(&slab->magic); // 尝试从原子栈(magic)中非安全地弹出一个对象
  if (ret == NULL) { // 如果栈空 (没有可用对象)
    if (!slab_expand(slab, false)) // 尝试扩展 (非安全)
      return NULL; // 扩展失败，返回NULL
    ret = astk_pop_unsafe(&slab->magic); // 再次尝试弹出 (扩展后应该有对象了)
  }
  debug_assert(ret); // 此时应该成功获取到对象
  slab->nr_ready--; // 可用对象计数减1 (非原子)
  return ret;
}

  void *
slab_alloc_safe(struct slab * const slab) // 线程安全模式下分配一个对象
{
  void * ret = astk_pop_safe(&slab->magic); // 尝试从原子栈(magic)中安全地弹出一个对象
  if (ret) // 如果成功弹出
    return ret; // 直接返回

  // 如果栈空，需要加锁并尝试扩展
  mutex_lock(&slab->lock); // 加锁
  do {
    ret = astk_pop_safe(&slab->magic); // 再次尝试弹出 (可能其他线程已扩展并加入对象)
    if (ret) // 如果这次成功
      break; // 跳出循环
    if (!slab_expand(slab, true)) // 尝试扩展 (安全模式，在锁内进行)
      break; // 扩展失败，跳出循环 (ret此时为NULL)
    // 扩展成功后，循环会再次尝试pop
  } while (true);
  mutex_unlock(&slab->lock); // 解锁
  return ret; // 返回获取到的对象 (可能为NULL如果扩展失败)
}

  void
slab_free_unsafe(struct slab * const slab, void * const ptr) // 非安全模式下释放一个对象
{
  debug_assert(ptr); // 确保指针有效
  astk_push_unsafe(&slab->magic, ptr, ptr); // 非安全地将对象压回原子栈 (ptr作为单元素链表)
  slab->nr_ready++; // 可用对象计数加1 (非原子)
}

  void
slab_free_safe(struct slab * const slab, void * const ptr) // 线程安全模式下释放一个对象
{
  astk_push_safe(&slab->magic, ptr, ptr); // 安全地将对象压回原子栈
}

// 非安全模式：释放所有已分配的对象，将所有块移至备份链表
  void
slab_free_all(struct slab * const slab)
{
  slab->magic = 0; // 清空原子栈 (magic值设为0，表示空栈，序列号也为0)
  slab->nr_ready = 0; // 可用对象计数清零 (备份链表中的对象不计入nr_ready)

  if (slab->head_active) { // 如果活跃块链表不为空
    struct acell * iter = slab->head_active; // iter指向活跃块链表头
    while (iter->next) // 找到活跃块链表的尾部
      iter = iter->next;
    // 此处iter指向活跃链表的最后一个块
    iter->next = slab->head_backup; // 将活跃链表的尾部链接到备份链表的头部
    slab->head_backup = slab->head_active; // 将整个活跃链表移动到备份链表
    slab->head_active = NULL; // 清空活跃链表
  }
}

// 非安全模式：获取当前已分配出去的对象数量 (不在空闲链表中的对象)
  u64
slab_get_nalloc(struct slab * const slab)
{
  struct acell * iter = slab->head_active; // 遍历活跃块链表
  u64 n = 0; // 总对象数 (基于活跃块计算)
  while (iter) {
    n++; // 块数量
    iter = iter->next;
  }
  n *= slab->objs_per_slab; // 总对象数 = 块数 * 每块对象数

  // 从总对象数中减去当前在magic空闲链表中的对象数
  iter = astk_peek_unsafe(&slab->magic); // 查看magic栈顶
  while (iter) { // 遍历magic空闲链表
    n--; // 每有一个空闲对象，已分配数减1
    iter = iter->next;
  }
  return n; // 返回已分配的对象数量
}

  static void
slab_deinit(struct slab * const slab) // 内部函数：释放slab分配器占用的所有内存块
{
  debug_assert(slab); // 确保slab指针有效
  struct acell * iter = slab->head_active; // 遍历活跃块链表
  while (iter) {
    struct acell * const next = iter->next; // 保存下一个块的指针
    pages_unmap(iter, slab->blk_size); // 解除映射并释放当前块
    iter = next; // 移动到下一个块
  }
  iter = slab->head_backup; // 遍历备份块链表
  while (iter) {
    struct acell * const next = iter->next;
    pages_unmap(iter, slab->blk_size);
    iter = next;
  }
}

  void
slab_destroy(struct slab * const slab) // 销毁slab分配器
{
  slab_deinit(slab); // 释放所有内存块
  free(slab); // 释放slab控制结构本身
}
// }}} slab

// qsort {{{ // 快速排序和相关工具函数
  int
compare_u16(const void * const p1, const void * const p2) // u16比较函数 (用于qsort)
{
  const u16 v1 = *((const u16 *)p1);
  const u16 v2 = *((const u16 *)p2);
  if (v1 < v2)
    return -1;
  else if (v1 > v2)
    return 1;
  else
    return 0;
}

  inline void
qsort_u16(u16 * const array, const size_t nr) // 对u16数组进行快速排序
{
  qsort(array, nr, sizeof(array[0]), compare_u16);
}

  inline u16 *
bsearch_u16(const u16 v, const u16 * const array, const size_t nr) // 在已排序的u16数组中二分查找v
{
  return (u16 *)bsearch(&v, array, nr, sizeof(u16), compare_u16);
}

  void
shuffle_u16(u16 * const array, const u64 nr) // Fisher-Yates洗牌算法打乱u16数组
{
  // 注意：如果 nr 为 0 或 1，此实现可能存在问题。
  // nr = 0: i = -1 (u64), random_u64() % i 是未定义行为。
  // nr = 1: i = 0 (u64), random_u64() % i 是除零错误。
  // 应该像 shuffle_u32/u64 一样添加 if (nr <= 1) return;
  u64 i = nr - 1; // i 从 nr-1 向下到 1
  do {
    const u64 j = random_u64() % i; // j 是 [0, i-1] 范围内的随机数
    // 交换 array[i] 和 array[j]
    const u16 t = array[j];
    array[j] = array[i];
    array[i] = t;
  } while (--i); // i递减，直到i为0 (循环条件i>0)
}

  int
compare_u32(const void * const p1, const void * const p2) // u32比较函数
{
  const u32 v1 = *((const u32 *)p1);
  const u32 v2 = *((const u32 *)p2);
  if (v1 < v2)
    return -1;
  else if (v1 > v2)
    return 1;
  else
    return 0;
}

  inline void
qsort_u32(u32 * const array, const size_t nr) // 对u32数组进行快速排序
{
  qsort(array, nr, sizeof(array[0]), compare_u32);
}

  inline u32 *
bsearch_u32(const u32 v, const u32 * const array, const size_t nr) // 在已排序的u32数组中二分查找v
{
  return (u32 *)bsearch(&v, array, nr, sizeof(u32), compare_u32);
}

  void
shuffle_u32(u32 * const array, const u64 nr) // 打乱u32数组
{
  if (nr <= 1) return; // 处理nr为0或1的情况，避免模0或越界
  u64 i = nr - 1; // i 从 nr-1 向下到 1
  do {
    const u64 j = random_u64() % i; // j 是 [0, i-1] 范围内的随机数
    const u32 t = array[j];
    array[j] = array[i];
    array[i] = t;
  } while (--i);
}

  int
compare_u64(const void * const p1, const void * const p2) // u64比较函数
{
  const u64 v1 = *((const u64 *)p1);
  const u64 v2 = *((const u64 *)p2);

  if (v1 < v2)
    return -1;
  else if (v1 > v2)
    return 1;
  else
    return 0;
}

  inline void
qsort_u64(u64 * const array, const size_t nr) // 对u64数组进行快速排序
{
  qsort(array, nr, sizeof(array[0]), compare_u64);
}

  inline u64 *
bsearch_u64(const u64 v, const u64 * const array, const size_t nr) // 在已排序的u64数组中二分查找v
{
  return (u64 *)bsearch(&v, array, nr, sizeof(u64), compare_u64);
}

  void
shuffle_u64(u64 * const array, const u64 nr) // 打乱u64数组
{
  if (nr <= 1) return; // 处理nr为0或1的情况
  u64 i = nr - 1; // i 从 nr-1 向下到 1
  do {
    const u64 j = random_u64() % i; // j 是 [0, i-1] 范围内的随机数
    const u64 t = array[j];
    array[j] = array[i];
    array[i] = t;
  } while (--i);
}

  int
compare_double(const void * const p1, const void * const p2) // double比较函数
{
  const double v1 = *((const double *)p1);
  const double v2 = *((const double *)p2);
  if (v1 < v2)
    return -1;
  else if (v1 > v2)
    return 1;
  else
    return 0;
}

  inline void
qsort_double(double * const array, const size_t nr) // 对double数组进行快速排序
{
  qsort(array, nr, sizeof(array[0]), compare_double);
}

  void
qsort_u64_sample(const u64 * const array0, const u64 nr, const u64 res, FILE * const out) // 对u64数组排序并采样输出到文件
{
  // array0: 原始数组
  // nr: 数组元素个数
  // res: 采样分辨率 (大致采样点数，0表示默认64)
  // out: 输出文件指针
  const u64 datasize = nr * sizeof(array0[0]); // 计算数据总大小
  u64 * const array = malloc(datasize); // 分配临时数组用于排序
  debug_assert(array); // 确保分配成功
  memcpy(array, array0, datasize); // 复制数据到临时数组
  qsort_u64(array, nr); // 排序

  const double sized = (double)nr; // 数组大小的浮点表示
  const u64 srate = res ? res : 64; // 采样率/目标采样点数
  // xstep: 按索引的采样步长 (至少为1)
  const u64 xstep = ({u64 step = nr / srate; step ? step : 1; });
  // ystep: 按值的采样步长 (至少为1)
  const u64 ystep = ({u64 step = (array[nr - 1] - array[0]) / srate; step ? step : 1; });
  u64 i = 0; // 上一个采样点的索引
  // 输出第一个采样点 (数组第一个元素)
  fprintf(out, "%lu %06.2lf %lu\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
  for (u64 j = 1; j < nr; j++) { // 遍历数组
    // 如果当前点与上一个采样点的索引差达到xstep，或者值差达到ystep，则采样当前点
    if (((j - i) >= xstep) || (array[j] - array[i]) >= ystep) {
      i = j; // 更新上一个采样点索引
      fprintf(out, "%lu %06.2lf %lu\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
    }
  }
  if (i != (nr - 1)) { // 确保最后一个元素被采样
    i = nr - 1;
    fprintf(out, "%lu %06.2lf %lu\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
  }
  free(array); // 释放临时数组
}

  void
qsort_double_sample(const double * const array0, const u64 nr, const u64 res, FILE * const out) // 对double数组排序并采样输出
{
  const u64 datasize = nr * sizeof(double); // 计算数据总大小
  double * const array = malloc(datasize); // 分配临时数组
  debug_assert(array); // 确保分配成功
  memcpy(array, array0, datasize); // 复制数据
  qsort_double(array, nr); // 排序

  const u64 srate = res ? res : 64; // 目标采样点数
  const double srate_d = (double)srate; // 采样点数的浮点表示
  const double sized = (double)nr; // 数组大小的浮点表示
  // xstep: 按索引的采样步长
  const u64 xstep = ({u64 step = nr / srate; step ? step : 1; });
  // ystep: 按值的采样步长
  const double ystep = ({ double step = fabs((array[nr - 1] - array[0]) / srate_d); step != 0.0 ? step : 1.0; });
  u64 i = 0; // 上一个采样点的索引
  // 输出第一个采样点
  fprintf(out, "%lu %06.2lf %020.9lf\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
  for (u64 j = 1; j < nr; j++) { // 遍历数组
    // 如果索引差或值差达到步长，则采样
    if (((j - i) >= xstep) || (array[j] - array[i]) >= ystep) {
      i = j; // 更新采样点
      fprintf(out, "%lu %06.2lf %020.9lf\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
    }
  }
  if (i != (nr - 1)) { // 确保最后一个元素被采样
    i = nr - 1;
    fprintf(out, "%lu %06.2lf %020.9lf\n", i, ((double)(i + 1)) * 100.0 / sized, array[i]);
  }
  free(array); // 释放临时数组
}
// }}} qsort

// string {{{ // 字符串处理函数
// 用于快速将0-99的数字转换为两位十进制字符的查找表
static union { u16 v16; u8 v8[2]; } strdec_table[100];

__attribute__((constructor)) // 构造函数，在main之前执行
  static void
strdec_init(void) // 初始化strdec_table
{
  for (u8 i = 0; i < 100; i++) {
    const u8 hi = (typeof(hi))('0' + (i / 10)); // 计算十位字符
    const u8 lo = (typeof(lo))('0' + (i % 10)); // 计算个位字符
    // 存储顺序取决于字节序和后续如何读取v16
    // 如果v16被当作小端u16读取，则v8[0]是低字节，v8[1]是高字节
    // 这里赋值给v8数组，然后通过v16读取，依赖于联合体的内存布局
    strdec_table[i].v8[0] = hi; // 假设v8[0]是字符对的第一个字符 (高位数字)
    strdec_table[i].v8[1] = lo; // 假设v8[1]是字符对的第二个字符 (低位数字)
  }
}

// 将u32整数v转换为10字节的十进制字符串 (不足补前导零)
// 输出到out指针，out必须指向至少10字节的内存
  void
strdec_32(void * const out, const u32 v)
{
  u32 vv = v; // 临时变量
  u16 * const ptr = (typeof(ptr))out; // 将输出指针视为u16数组 (每次处理两位数字)
  // u32最大值 4,294,967,295 (10位)
  // 循环5次，每次处理两位数字 (vv % 100)
  for (u64 i = 4; i <= 4; i--) { // 循环从4到0 (共5次)
                                 // i=4: ptr[4] 对应最低两位 (v % 100)
                                 // i=0: ptr[0] 对应最高两位
    ptr[i] = strdec_table[vv % 100u].v16; // 从查找表获取两位字符(u16)，存入ptr[i]
    vv /= 100u; // v除以100，处理下两位
  }
}

// 将u64整数v转换为20字节的十进制字符串 (不足补前导零)
// 输出到out指针，out必须指向至少20字节的内存
  void
strdec_64(void * const out, const u64 v)
{
  u64 vv = v; // 临时变量
  u16 * const ptr = (typeof(ptr))out; // 输出指针视为u16数组
  // u64最大值约 1.8e19 (20位)
  // 循环10次，每次处理两位数字
  for (u64 i = 9; i <= 9; i--) { // 循环从9到0 (共10次)
                                 // i=9: ptr[9] 对应最低两位
                                 // i=0: ptr[0] 对应最高两位
    ptr[i] = strdec_table[vv % 100].v16; // 获取两位字符
    vv /= 100; // 处理下两位
  }
}

// 0-15到十六进制字符 '0'-'9', 'a'-'f' 的映射表
static const u8 strhex_table_16[16] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};

#if defined(__x86_64__) // x86_64 SIMD实现
  static inline m128
strhex_helper(const u64 v) // 将u64整数v的8个字节转换为16个十六进制字符 (m128向量)
{
  // mask1用于pshufb指令，将字节内的高4位和低4位分离并交错排列，以形成nibble序列
  // 原始字节: H L (高4位H, 低4位L)
  // 目标顺序: L0 H0 L1 H1 ... L7 H7 (0对应最低字节的nibble)
  // 这里的mask1用于从 (v & 0xf) 和 (v>>4 & 0xf) 交错的字节中提取正确的nibble
  static const u8 mask1[16] = {15,7,14,6,13,5,12,4,11,3,10,2,9,1,8,0}; // PSHUFB的掩码

  const m128 tmp = _mm_set_epi64x((s64)(v>>4), (s64)v); // 将v和v>>4放入128位向量的两个64位部分
                                                      // tmp = [ (v>>4) | v ] (高64位是v>>4, 低64位是v)
  const m128 hilo = _mm_and_si128(tmp, _mm_set1_epi8(0xf)); // 取每个字节的低4位 (得到nibble值)
                                                           // hilo = [ (v>>4)&0xf | v&0xf ]
  const m128 bin = _mm_shuffle_epi8(hilo, _mm_load_si128((void *)mask1)); // 使用mask1重排并提取nibble序列
                                                                        // bin现在包含16个字节，每个字节是0-15的nibble值，顺序是v的十六进制表示从高到低
  const m128 str = _mm_shuffle_epi8(_mm_load_si128((const void *)strhex_table_16), bin); // 查表将nibble转换为ASCII字符
  return str; // 返回包含16个十六进制字符的向量
}
#elif defined(__aarch64__) // AArch64 Neon实现
  static inline m128
strhex_helper(const u64 v) // 将u64整数v的8个字节转换为16个十六进制字符
{
  // Neon的实现思路类似：
  // 1. 准备v和v>>4的数据。
  // 2. 取每个字节的低4位得到nibble。
  // 3. 使用查表指令(vqtbl1q_u8)和合适的掩码/索引表将nibble转换为字符。
  static const u8 mask1[16] = {15,7,14,6,13,5,12,4,11,3,10,2,9,1,8,0}; // 查表指令的索引表
  u64 v2[2] = {v, v>>4}; // 将v和v>>4放入数组，以便加载到Neon向量
                         // 假设小端，v2[0]是v, v2[1]是v>>4
  const m128 tmp = vld1q_u8((u8 *)v2); // 加载到128位向量 tmp = [ v | v>>4 ] (字节顺序)
                                       // 具体是 [v0..v7 (v>>4)_0 .. (v>>4)_7] 或相反，取决于vld1q_u8和数组布局
                                       // 目标是得到类似x86的 [ (v>>4) | v ] 字节排列效果，以便mask1工作
  const m128 hilo = vandq_u8(tmp, vdupq_n_u8(0xf)); // 取每个字节低4位，得到nibble
  const m128 bin = vqtbl1q_u8(hilo, vld1q_u8(mask1)); // 使用mask1作为索引表，从hilo中查表形成nibble序列
  const m128 str = vqtbl1q_u8(vld1q_u8(strhex_table_16), bin); // 再查strhex_table_16将nibble转为字符
  return str;
}
#else // 非SIMD的通用实现
// 预计算0-255每个字节对应的两个十六进制字符 (u16形式)
static u16 strhex_table_256[256];

__attribute__((constructor))
  static void
strhex_init(void) // 初始化strhex_table_256
{
  for (u64 i = 0; i < 256; i++) {
    // strhex_table_16[i & 0xf] 是低4位的字符 (L)
    // strhex_table_16[i >> 4]  是高4位的字符 (H)
    // 将两个字符合并为一个u16。存储顺序会影响memcpy时的结果。
    // 如果目标是 "HL" 顺序 (高位nibble字符在前)，则 (H << 8) | L (大端u16)
    // 或者 (L << 8) | H (小端u16，内存中H在前)
    // 这里是 (L << 8) | H，所以内存中是 [H, L]
    strhex_table_256[i] = (((u16)strhex_table_16[i & 0xf]) << 8) | (strhex_table_16[i>>4]);
  }
}
#endif // __x86_64__

// 将u32整数v转换为8字节的十六进制字符串，输出到out
  void
strhex_32(void * const out, u32 v)
{
#if defined(__x86_64__)
  const m128 str = strhex_helper((u64)v); // 调用辅助函数得到v的16个十六进制字符 (高8字节为0)
  // str = [c15,c14,...,c8, c7,c6,...,c0] (c0是v最低nibble的字符)
  // 我们需要v的8个字符，对应str的高8个字符 (c15..c8)，因为strhex_helper处理的是u64的低8字节
  // v作为u64时，其高4字节为0。strhex_helper(v)的结果，前8个字符是0，后8个字符是v的十六进制。
  // _mm_srli_si128(str, 8) 将str逻辑右移8字节，高位补0。
  // 结果的低8字节是原str的高8字节，即v的十六进制表示。
  _mm_storel_epi64(out, _mm_srli_si128(str, 8)); // 存储右移后向量的低64位 (即原str的高8字节)
#elif defined(__aarch64__)
  const m128 str = strhex_helper((u64)v); // str包含v的16个十六进制字符 (高8字节为0的字符)
  // vreinterpretq_u64_u8: 将u8x16向量重新解释为u64x2向量
  // lane 1 对应向量的高64位，即v的十六进制表示
  vst1q_lane_u64(out, vreinterpretq_u64_u8(str), 1); // 存储向量的高64位
#else // 通用实现
  u16 * const ptr = (typeof(ptr))out; // 输出视为u16数组 (每次写2个字符)
  for (u64 i = 0; i < 4; i++) { // 循环4次，处理4个字节
    // ptr[3-i] 写入顺序是从高地址到低地址 (对应v的从低字节到高字节)
    // v & 0xff 取最低字节
    // strhex_table_256[v & 0xff] 是该字节对应的两个十六进制字符 (u16), 内存顺序是 [高nibble字符, 低nibble字符]
    ptr[3-i] = strhex_table_256[v & 0xff];
    v >>= 8; // 处理下一个字节
  }
#endif
}

// 将u64整数v转换为16字节的十六进制字符串，输出到out
// SIMD版本通常要求out对齐，但这里使用非对齐存储指令
  void
strhex_64(void * const out, u64 v)
{
#if defined(__x86_64__)
  const m128 str = strhex_helper(v); // 得到16个字符的向量
  _mm_storeu_si128(out, str); // 非对齐存储整个128位向量
#elif defined(__aarch64__)
  const m128 str = strhex_helper(v); // 得到16个字符的向量
  vst1q_u8(out, str); // 存储整个128位向量 (Neon的vst1q_u8通常不要求严格对齐)
#else // 通用实现
  u16 * const ptr = (typeof(ptr))out; // 输出视为u16数组
  for (u64 i = 0; i < 8; i++) { // 循环8次，处理8个字节
    // ptr[7-i] 写入顺序从高地址到低地址
    ptr[7-i] = strhex_table_256[v & 0xff]; // 获取字节对应的两个十六进制字符
    v >>= 8; // 处理下一个字节
  }
#endif
}

// 字符串转u64 (十进制)
  inline u64
a2u64(const void * const str)
{
  return strtoull(str, NULL, 10); // 使用标准库函数
}

// 字符串转u32 (十进制)
  inline u32
a2u32(const void * const str)
{
  return (u32)strtoull(str, NULL, 10); // 先转为u64再强转
}

// 字符串转s64 (十进制)
  inline s64
a2s64(const void * const str)
{
  return strtoll(str, NULL, 10); // 使用标准库函数
}

// 字符串转s32 (十进制)
  inline s32
a2s32(const void * const str)
{
  return (s32)strtoll(str, NULL, 10); // 先转为s64再强转
}

  void
str_print_hex(FILE * const out, const void * const data, const u32 len) // 将data区域的内容以十六进制形式打印到文件out
{
  const u8 * const ptr = data; // 数据指针
  const u32 strsz = len * 3; // 每个字节输出3个字符 (空格+两位十六进制)，计算总输出字符串长度
  u8 * const buf = malloc(strsz); // 分配临时缓冲区
  if (!buf) return; // 分配失败则返回
  for (u32 i = 0; i < len; i++) {
    buf[i*3] = ' '; // 分隔符空格
    buf[i*3+1] = strhex_table_16[ptr[i]>>4]; // 高4位字符
    buf[i*3+2] = strhex_table_16[ptr[i] & 0xf]; // 低4位字符
  }
  fwrite(buf, strsz, 1, out); // 一次性写入缓冲区内容
  free(buf); // 释放缓冲区
}

  void
str_print_dec(FILE * const out, const void * const data, const u32 len) // 将data区域的内容以十进制形式 (每个字节3位数字) 打印到文件out
{
  const u8 * const ptr = data; // 数据指针
  const u32 strsz = len * 4; // 每个字节输出4个字符 (空格+三位十进制)，计算总长度
  u8 * const buf = malloc(strsz); // 分配临时缓冲区
  if (!buf) return; // 分配失败则返回
  for (u32 i = 0; i < len; i++) {
    const u8 v = ptr[i]; // 当前字节值 (0-255)
    buf[i*4] = ' '; // 分隔符
    const u8 v1 = v / 100u; // 百位
    const u8 v23 = v % 100u; // 低两位 (0-99)
    buf[i*4+1] = (u8)'0' + v1; // 百位字符
    buf[i*4+2] = (u8)'0' + (v23 / 10u); // 十位字符
    buf[i*4+3] = (u8)'0' + (v23 % 10u); // 个位字符
  }
  fwrite(buf, strsz, 1, out); // 一次性写入
  free(buf); // 释放缓冲区
}

// 返回一个以NULL结尾的字符串标记列表。
// 使用后只需释放返回的指针 (char **)。
// 内部会将原始字符串复制一份，并在其上进行strtok_r操作，
// 然后将整个结果 (指针数组和标记化的字符串数据) 重新分配到一块连续内存中。
  char **
strtoks(const char * const str, const char * const delim) // 将字符串str按delim分割成多个标记
{
  if (str == NULL) // 输入字符串为空
    return NULL;
  size_t nptr_alloc = 32; // 初始分配的指针数组大小
  char ** tokens = malloc(sizeof(tokens[0]) * nptr_alloc); // 分配指针数组
  if (tokens == NULL)
    return NULL;
  const size_t bufsize = strlen(str) + 1; // 计算原始字符串的缓冲区大小 (包括'\0')
  char * const buf = malloc(bufsize); // 分配临时缓冲区用于strtok_r (因为它会修改原串)
  if (buf == NULL)
    goto fail_buf; // 分配失败，跳转到清理标签

  memcpy(buf, str, bufsize); // 复制原始字符串到临时缓冲区
  char * saveptr = NULL; // strtok_r的上下文指针
  char * tok = strtok_r(buf, delim, &saveptr); // 获取第一个标记
  size_t ntoks = 0; // 标记计数
  while (tok) { // 循环获取所有标记
    if (ntoks >= nptr_alloc) { // 如果指针数组空间不足
      nptr_alloc += 32; // 增加分配大小
      char ** const r = realloc(tokens, sizeof(tokens[0]) * nptr_alloc); // 重新分配指针数组
      if (r == NULL) // 重分配失败
        goto fail_realloc;
      tokens = r; // 更新指针数组指针
    }
    tokens[ntoks] = tok; // 存储标记指针 (此时指向buf中的位置)
    ntoks++; // 标记计数加1
    tok = strtok_r(NULL, delim, &saveptr); // 获取下一个标记
  }
  tokens[ntoks] = NULL; // 在指针数组末尾添加NULL作为结束标记
  const size_t nptr = ntoks + 1; // 指针数组中实际元素个数 (包括末尾NULL)
  // 计算最终连续内存块的大小 = 指针数组大小 + 标记化字符串数据大小
  const size_t rsize = (sizeof(tokens[0]) * nptr) + bufsize;
  // 重新分配tokens，使其足够大以容纳指针数组和字符串数据
  char ** const r = realloc(tokens, rsize);
  if (r == NULL)
    goto fail_realloc; // 重新分配失败

  tokens = r; // 更新tokens指针
  // 计算字符串数据在最终内存块中的目标地址 (紧跟在指针数组之后)
  char * const dest = (char *)(&(tokens[nptr]));
  memcpy(dest, buf, bufsize); // 将buf中的标记化字符串数据复制到目标位置
  // 修正tokens数组中的指针，使其指向新的连续内存块中的字符串数据
  for (u64 i = 0; i < ntoks; i++)
    tokens[i] += (dest - buf); // 偏移量 = dest起始地址 - buf起始地址

  free(buf); // 释放临时的buf
  return tokens; // 返回指向连续内存块的指针数组

fail_realloc: // realloc失败处理
  free(buf); // 释放buf
fail_buf: // buf分配失败处理
  free(tokens); // 释放tokens
  return NULL;
}

  u32
strtoks_count(const char * const * const toks) // 计算strtoks返回的标记数量 (不包括末尾NULL)
{
  if (!toks) // 如果输入为空
    return 0;
  u32 n = 0;
  while (toks[n++]); // 循环直到toks[n-1]为NULL, 此时n是标记数+1
  // 例如: {"a", "b", NULL}
  // n=0: toks[0] != NULL, n=1
  // n=1: toks[1] != NULL, n=2
  // n=2: toks[2] == NULL, loop ends, n=3
  // 应该返回2. 当前代码返回n (即3).
  return n; // 注意：此函数返回的计数比实际标记数多1 (如果toks非空且至少有一个NULL结束符)
            // 如果toks是空数组(只有NULL), n会是1.
            // 如果toks是 {{NULL}}, n会是1.
            // 正确的计数应该是 n ? (n-1) : 0;
}
// }}} string

// qsbr {{{ // Quiescent-State-Based Reclamation (基于静止状态的回收) RCU实现
#define QSBR_STATES_NR ((23)) // 每个分片(shard)的容量；有效值如 3*8-1=23; 5*8-1=39; 7*8-1=55
                              // 这个值需要小于64 (因为用u64位图)，并且通常选择为素数或避免某些模式
#define QSBR_SHARD_BITS  ((5)) // 分片数量的位数 (2^5 = 32个分片)
#define QSBR_SHARD_NR    (((1u) << QSBR_SHARD_BITS)) // 分片总数
#define QSBR_SHARD_MASK  ((QSBR_SHARD_NR - 1)) // 用于计算分片索引的掩码

struct qsbr_ref_real { // QSBR引用计数/状态的实际结构 (用户通过qsbr_ref访问)
#ifdef QSBR_DEBUG // 如果启用了QSBR调试
  pthread_t ptid; // 线程ID (8字节)
  u32 status; // 状态 (4字节)
  u32 nbt; // 回溯帧数 (4字节)
#define QSBR_DEBUG_BTNR ((14)) // 存储的回溯帧数量
  void * backtrace[QSBR_DEBUG_BTNR]; // 回溯信息
#endif
  volatile au64 qstate; // 静止状态值 (用户更新此值，原子访问)
  // 以下为内部使用
  struct qsbr_ref_real * volatile * pptr; // 指向自身在shard的ptrs数组中的位置的指针 (用于快速移除或停放)
  struct qsbr_ref_real * park; // 指向qsbr->target的指针 (用于park/resume优化时，pptr指向此)
};

// 确保用户可见的qsbr_ref类型与内部qsbr_ref_real大小一致
static_assert(sizeof(struct qsbr_ref) == sizeof(struct qsbr_ref_real), "sizeof qsbr_ref");

// Quiescent-State-Based Reclamation RCU 主结构
struct qsbr {
  struct qsbr_ref_real target; // 一个特殊的目标qsbr_ref，用于表示已注销或停放的引用
  u64 padding0[5]; // 填充到下一个缓存行 (target占16字节，加5*8=40字节，总共56，还差8字节到64)
                   // 如果QSBR_DEBUG未定义，target是8字节，加40字节是48，差16字节。
                   // 实际大小取决于QSBR_DEBUG。目标可能是让shards数组缓存行对齐。
  struct qshard { // QSBR分片结构
    au64 bitmap; // 位图，标记ptrs数组中哪些槽位被占用 (最低QSBR_STATES_NR位)
                 // 最高位 (第63位) 可能用作分片锁或其他标记
    struct qsbr_ref_real * volatile ptrs[QSBR_STATES_NR]; // 指向已注册qsbr_ref的指针数组
  } shards[QSBR_SHARD_NR]; // 分片数组
};

  struct qsbr *
qsbr_create(void) // 创建并初始化一个QSBR实例
{
  struct qsbr * const q = yalloc(sizeof(*q)); // 分配缓存行对齐的内存
  if (q) // 分配成功
    memset(q, 0, sizeof(*q)); // 清零整个结构
  return q;
}

  static inline struct qshard *
qsbr_shard(struct qsbr * const q, void * const ptr) // 根据指针ptr的哈希值选择一个分片
{
  // 使用ptr的CRC32C哈希值低位作为分片索引
  const u32 sid = crc32c_u64(0, (u64)ptr) & QSBR_SHARD_MASK;
  debug_assert(sid < QSBR_SHARD_NR); // 确保索引在有效范围内
  return &(q->shards[sid]); // 返回对应分片的指针
}

  static inline void
qsbr_write_qstate(struct qsbr_ref_real * const ref, const u64 v) // 原子地写入静止状态值
{
  // 使用松散内存序，因为RCU的静止状态更新通常不需要强顺序保证，
  // 等待操作(qsbr_wait)会使用更强的内存屏障。
  atomic_store_explicit(&ref->qstate, v, MO_RELAXED);
}

  bool
qsbr_register(struct qsbr * const q, struct qsbr_ref * const qref) // 注册一个qsbr_ref到QSBR系统中
{
  struct qsbr_ref_real * const ref = (typeof(ref))qref; // 转换为内部类型
  struct qshard * const shard = qsbr_shard(q, ref); // 选择一个分片
  qsbr_write_qstate(ref, 0); // 初始化静止状态为0 (通常表示活跃)

  do { // 循环尝试在分片中找到一个空槽并注册
    u64 bits = atomic_load_explicit(&shard->bitmap, MO_CONSUME); // 读取当前分片的位图
    // __builtin_ctzl(~bits) 找到位图中第一个为0的位 (即空槽) 的索引
    const u32 pos = (u32)__builtin_ctzl(~bits);
    if (unlikely(pos >= QSBR_STATES_NR)) // 如果没有空槽 (pos超出范围)
      return false; // 注册失败 (分片已满)

    const u64 bits1 = bits | (1lu << pos); // 准备新的位图值 (将pos位设为1)
    // 尝试用CAS更新位图，如果成功，则表示已抢占到该槽位
    // MO_ACQUIRE: 确保成功注册后，后续对此引用的操作能看到其初始化状态
    if (atomic_compare_exchange_weak_explicit(&shard->bitmap, &bits, bits1, MO_ACQUIRE, MO_RELAXED)) {
      shard->ptrs[pos] = ref; // 将引用指针存入槽位

      // 设置引用内部的pptr和park指针
      ref->pptr = &(shard->ptrs[pos]); // pptr指向自身在ptrs数组中的位置
      ref->park = &q->target; // park指向qsbr的target引用 (用于停放)
#ifdef QSBR_DEBUG // 调试信息
      ref->ptid = (u64)pthread_self(); // 记录线程ID
      ref->tid = 0; // 线程ID (似乎未使用或未完全实现)
      ref->status = 1; // 状态：已注册
      ref->nbt = backtrace(ref->backtrace, QSBR_DEBUG_BTNR); // 记录注册时的回溯
#endif
      return true; // 注册成功
    }
    // CAS失败则循环重试
  } while (true);
}

  void
qsbr_unregister(struct qsbr * const q, struct qsbr_ref * const qref) // 从QSBR系统中注销一个qsbr_ref
{
  struct qsbr_ref_real * const ref = (typeof(ref))qref; // 转换为内部类型
  struct qshard * const shard = qsbr_shard(q, ref); // 获取分片
  // 通过pptr直接计算出引用在ptrs数组中的索引
  const u32 pos = (u32)(ref->pptr - shard->ptrs);
  debug_assert(pos < QSBR_STATES_NR); // 确保索引有效
  debug_assert(shard->bitmap & (1lu << pos)); // 确保该槽位在位图中确实被标记为占用

  shard->ptrs[pos] = &q->target; // 将槽位指针指向target (标记为空闲或无效，或用于停放的引用)
  // 原子地清除位图中对应槽位的标记 (将pos位设为0)
  // MO_RELEASE: 确保注销操作对qsbr_wait可见
  (void)atomic_fetch_and_explicit(&shard->bitmap, ~(1lu << pos), MO_RELEASE);
#ifdef QSBR_DEBUG
  ref->tid = 0; // 线程ID (似乎未使用)
  ref->ptid = 0; // 清除记录的线程ID
  ref->status = 0xffff; // 状态：已注销
  ref->nbt = 0; // 清除回溯信息
#endif
  ref->pptr = NULL; // 清除pptr
  // 等待qsbr_wait离开，如果它正在处理这个分片
  // (通过检查分片位图的第63位，该位在qsbr_wait中被用作临时锁)
  while (atomic_load_explicit(&shard->bitmap, MO_CONSUME) >> 63) // 如果第63位是1 (锁被持有)
    cpu_pause(); // 自旋等待
}

  inline void
qsbr_update(struct qsbr_ref * const qref, const u64 v) // 更新一个已注册qsbr_ref的静止状态值
{
  struct qsbr_ref_real * const ref = (typeof(ref))qref; // 转换为内部类型
  debug_assert((*ref->pptr) == ref); // 必须是未停放的引用 (pptr指向自身)
  // RCU更新不需要强顺序保证 (release/acquire)
  qsbr_write_qstate(ref, v); // 原子写入新状态值
}

  inline void
qsbr_park(struct qsbr_ref * const qref) // 停放一个qsbr_ref (暂时使其不参与静止状态检查)
{
  cpu_cfence(); // 编译器屏障，防止重排，确保之前的状态更新对其他线程可见
  struct qsbr_ref_real * const ref = (typeof(ref))qref; // 转换为内部类型
  // 将分片中指向此引用的指针改为指向qsbr->target
  // 这样qsbr_wait会检查q->target的状态，而不是此引用的状态
  *ref->pptr = ref->park; // ref->park 指向 q->target
#ifdef QSBR_DEBUG
  ref->status = 0xfff; // 状态：已停放
#endif
}

  inline void
qsbr_resume(struct qsbr_ref * const qref) // 恢复一个已停放的qsbr_ref
{
  struct qsbr_ref_real * const ref = (typeof(ref))qref; // 转换为内部类型
  // 将分片中指向此引用的指针恢复为指向自身
  *ref->pptr = ref;
#ifdef QSBR_DEBUG
  ref->status = 0xf; // 状态：已恢复 (或任意非停放状态)
#endif
  cpu_cfence(); // 编译器屏障，确保此更改对后续的qsbr_wait可见
}

// 等待者需要外部同步 (通常只有一个线程调用qsbr_wait)
  void
qsbr_wait(struct qsbr * const q, const u64 target) // 等待所有已注册的活跃线程都达到或超过target静止状态
{
  cpu_cfence(); // 编译器屏障，确保之前的内存操作完成
  qsbr_write_qstate(&q->target, target); // 设置target引用的状态为目标值 (用于停放的引用)
  u64 cbits = 0; // 检查位图；每个位对应一个分片，为1表示该分片需要检查
  u64 bms[QSBR_SHARD_NR]; // 存储所有分片位图的快照
  // 非安全地获取所有活跃用户的快照
  for (u32 i = 0; i < QSBR_SHARD_NR; i++) {
    // 读取分片的位图 (只关心低QSBR_STATES_NR位，不包括可能的锁定位)
    bms[i] = atomic_load_explicit(&q->shards[i].bitmap, MO_CONSUME) & ((1lu << QSBR_STATES_NR) -1);
    if (bms[i]) // 如果分片中有活跃引用
      cbits |= (1lu << i); // 在cbits中标记此分片需要检查
  }

  while (cbits) { // 只要还有分片需要检查
    // 遍历所有需要检查的分片 (由cbits标记)
    for (u64 ctmp = cbits; ctmp; ctmp &= (ctmp - 1)) { // ctmp &= (ctmp-1) 清除最低的置位比特
      const u32 i = (u32)__builtin_ctzl(ctmp); // 获取需要检查的分片索引i
      struct qshard * const shard = &(q->shards[i]); // 获取分片指针
      // 尝试获取分片锁 (通过原子或操作设置位图的第63位)
      // MO_ACQUIRE确保获取锁后能看到其他线程对分片的修改
      const u64 bits1_before_lock = atomic_fetch_or_explicit(&(shard->bitmap), 1lu << 63, MO_ACQUIRE);
      // 遍历快照bms[i]中记录的该分片的活跃引用
      for (u64 bits_in_snapshot = bms[i]; bits_in_snapshot; bits_in_snapshot &= (bits_in_snapshot - 1)) {
        const u64 current_bit = bits_in_snapshot & -bits_in_snapshot; // 提取快照中当前检查的引用对应的位
        const u32 ref_pos = __builtin_ctzl(current_bit); // 获取引用在ptrs数组中的位置
        // 检查该引用是否已注销 (bits1_before_lock中对应位为0)
        // 或者其静止状态是否已达到目标值
        // shard->ptrs[ref_pos] 是当前检查的引用指针
        // atomic_load_explicit读取其qstate
        if (((bits1_before_lock & current_bit) == 0) || // 如果在获取分片锁时，该位已经是0 (表示已注销)
            (atomic_load_explicit(&(shard->ptrs[ref_pos]->qstate), MO_CONSUME) == target)) {
          bms[i] &= ~current_bit; // 从快照中移除此引用 (表示已满足条件)
        }
      }
      // 释放分片锁 (原子与操作清除位图的第63位)
      // MO_RELEASE确保释放锁后，其他线程能看到此分片状态的更新
      (void)atomic_fetch_and_explicit(&(shard->bitmap), ~(1lu << 63), MO_RELEASE);
      if (bms[i] == 0) // 如果此分片的所有引用都已满足条件
        cbits &= ~(1lu << i); // 从cbits中移除此分片
    }
#if defined(CORR) // 如果在协程环境
    corr_yield(); // 让出执行权，避免忙等
#else
    // 在非协程环境下，这里是忙等待循环，可以考虑短暂休眠或使用其他同步原语
#endif
  }
  debug_assert(cbits == 0); // 此时所有分片都应检查完毕
  cpu_cfence(); // 编译器屏障，确保后续操作在所有线程静止后进行
}

  void
qsbr_destroy(struct qsbr * const q) // 销毁QSBR实例
{
  if (q)
    free(q); // 释放QSBR结构内存
}
#undef QSBR_STATES_NR // 取消宏定义 QSBR_STATES_NR
#undef QSBR_BITMAP_NR // 取消宏定义 QSBR_BITMAP_NR (注意：之前定义的是 QSBR_SHARD_BITS 和 QSBR_SHARD_NR)
// }}} qsbr

// vim:fdm=marker // vim折叠标记
