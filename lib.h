/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#pragma once // 防止头文件被多次包含

// includes {{{ // 包含的头文件区域开始
// C headers // C 标准库头文件
#include <errno.h>      // 错误码定义
#include <inttypes.h>   // 整型类型格式转换
#include <math.h>       // 数学函数
#include <stdbool.h>    //布尔类型
#include <stddef.h>     // 标准定义 (如 size_t, NULL)
#include <stdio.h>      // 标准输入输出
#include <stdlib.h>     // 标准库函数 (如 malloc, free)
#include <string.h>     // 字符串操作
#include <assert.h>     // 断言

// POSIX headers // POSIX 标准头文件
#include <fcntl.h>      // 文件控制 (如 open, fcntl)
#include <pthread.h>    // POSIX 线程
#include <unistd.h>     // POSIX 标准符号常量和类型 (如 read, write, close)

// Linux headers // Linux 特定头文件
#include <sys/mman.h>   // 内存映射 (如 mmap, munmap)
#include <sys/resource.h> // 资源使用 (如 getrusage)
#include <sys/stat.h>   // 文件状态 (如 stat, fstat)
#include <sys/types.h>  // 基本系统数据类型

// SIMD // SIMD (单指令多数据) 相关头文件
#if defined(__x86_64__) // 如果是 x86_64 架构
#include <x86intrin.h>  // x86 内联函数
#elif defined(__aarch64__) // 如果是 aarch64 架构
#include <arm_acle.h>   // ARM C 语言扩展
#include <arm_neon.h>   // ARM NEON 指令集
#endif
// }}} includes // 包含的头文件区域结束

#ifdef __cplusplus // 如果是 C++ 环境
extern "C" { // 使用 C 语言链接方式
#endif

// types {{{ // 类型定义区域开始
typedef char            s8;   // 8位有符号整数
typedef short           s16;  // 16位有符号整数
typedef int             s32;  // 32位有符号整数
typedef long            s64;  // 64位有符号整数
typedef __int128_t      s128; // 128位有符号整数
static_assert(sizeof(s8) == 1, "sizeof(s8)");
static_assert(sizeof(s16) == 2, "sizeof(s16)");
static_assert(sizeof(s32) == 4, "sizeof(s32)");
static_assert(sizeof(s64) == 8, "sizeof(s64)");
static_assert(sizeof(s128) == 16, "sizeof(s128)");

typedef unsigned char   u8;   // 8位无符号整数
typedef unsigned short  u16;  // 16位无符号整数
typedef unsigned int    u32;  // 32位无符号整数
typedef unsigned long   u64;  // 64位无符号整数
typedef __uint128_t     u128; // 128位无符号整数
static_assert(sizeof(u8) == 1, "sizeof(u8)");
static_assert(sizeof(u16) == 2, "sizeof(u16)");
static_assert(sizeof(u32) == 4, "sizeof(u32)");
static_assert(sizeof(u64) == 8, "sizeof(u64)");
static_assert(sizeof(u128) == 16, "sizeof(u128)");

#if defined(__x86_64__) // 如果是 x86_64 架构
typedef __m128i m128;   // 128位 SIMD 数据类型
#if defined(__AVX2__)   // 如果支持 AVX2 指令集
typedef __m256i m256;   // 256位 SIMD 数据类型
#endif // __AVX2__
#if defined(__AVX512F__) // 如果支持 AVX512F 指令集
typedef __m512i m512;   // 512位 SIMD 数据类型
#endif // __AVX512F__
#elif defined(__aarch64__) // 如果是 aarch64 架构
typedef uint8x16_t m128; // 128位 NEON 数据类型 (16个8位无符号整数)
#else
#error Need x86_64 or AArch64. // 需要 x86_64 或 AArch64 架构
#endif
// }}} types // 类型定义区域结束

// defs {{{ // 宏定义区域开始
#define likely(____x____)   __builtin_expect(____x____, 1) // 标记表达式 ____x____ 很可能为真 (用于编译器优化)
#define unlikely(____x____) __builtin_expect(____x____, 0) // 标记表达式 ____x____ 很可能为假 (用于编译器优化)

// ansi colors // ANSI 终端颜色代码
// 3X:fg; 4X:bg; 9X:light fg; 10X:light bg;
// X can be one of the following colors:
// 0:black;   1:red;     2:green;  3:yellow;
// 4:blue;    5:magenta; 6:cyan;   7:white;
#define TERMCLR(____code____) "\x1b[" #____code____ "m" // 定义终端颜色宏
// }}} defs // 宏定义区域结束

// const {{{ // 常量定义区域开始
#define PGSZ ((4096lu)) // 定义页大小为 4096 字节
// }}} const // 常量定义区域结束

// math {{{ // 数学相关函数区域开始
  // 64位整数哈希函数
  extern u64
mhash64(const u64 v);

  // 32位整数哈希函数
  extern u32
mhash32(const u32 v);

  // 计算两个64位无符号整数的最大公约数
  extern u64
gcd64(u64 a, u64 b);
// }}} math // 数学相关函数区域结束

// random {{{ // 随机数相关函数区域开始
  // 生成一个64位无符号随机数
  extern u64
random_u64(void);

  // 设置64位无符号随机数生成器的种子
  extern void
srandom_u64(const u64 seed);

  // 生成一个 [0.0, 1.0) 范围内的双精度浮点随机数
  extern double
random_double(void);
// }}} random // 随机数相关函数区域结束

// timing {{{ // 时间相关函数区域开始
  // 获取当前时间的纳秒级时间戳
  extern u64
time_nsec(void);

  // 获取当前时间的秒级时间戳 (双精度浮点数)
  extern double
time_sec(void);

  // 计算与上一个纳秒级时间戳的时间差
  extern u64
time_diff_nsec(const u64 last);

  // 计算与上一个秒级时间戳的时间差
  extern double
time_diff_sec(const double last);

  // 将当前时间格式化为字符串 (YYYY-MM-DD-HH-MM-SS)
  extern void
time_stamp(char * str, const size_t size);

  // 将当前时间格式化为字符串 (YYYYMMDD-HHMMSS)
  extern void
time_stamp2(char * str, const size_t size);
// }}} timing // 时间相关函数区域结束

// cpucache {{{ // CPU 缓存相关函数区域开始
  // CPU暂停指令 (用于自旋等待)
  extern void
cpu_pause(void);

  // CPU内存屏障 (写屏障，确保所有之前的写操作完成)
  extern void
cpu_mfence(void);

  // CPU内存屏障 (通用屏障，确保所有之前的读写操作完成)
  extern void
cpu_cfence(void);

  // CPU预取指令 (预取到L1缓存，用于读)
  extern void
cpu_prefetch0(const void * const ptr);

  // CPU预取指令 (预取到L2缓存，用于读)
  extern void
cpu_prefetch1(const void * const ptr);

  // CPU预取指令 (预取到L3缓存，用于读)
  extern void
cpu_prefetch2(const void * const ptr);

  // CPU预取指令 (预取到非临时缓存，用于读)
  extern void
cpu_prefetch3(const void * const ptr);

  // CPU预取指令 (预取数据并标记为可写)
  extern void
cpu_prefetchw(const void * const ptr);
// }}} cpucache // CPU 缓存相关函数区域结束

// crc32c {{{ // CRC32C 校验和计算函数区域开始
  // 计算单个8位无符号整数的 CRC32C 增量
  extern u32
crc32c_u8(const u32 crc, const u8 v);

  // 计算单个16位无符号整数的 CRC32C 增量
  extern u32
crc32c_u16(const u32 crc, const u16 v);

  // 计算单个32位无符号整数的 CRC32C 增量
  extern u32
crc32c_u32(const u32 crc, const u32 v);

  // 计算单个64位无符号整数的 CRC32C 增量
  extern u32
crc32c_u64(const u32 crc, const u64 v);

// 1 <= nr <= 3 // 适用于1到3字节的增量计算
  extern u32
crc32c_inc_123(const u8 * buf, u32 nr, u32 crc);

// nr % 4 == 0 // 适用于字节数为4的倍数的增量计算
  extern u32
crc32c_inc_x4(const u8 * buf, u32 nr, u32 crc);

  // 通用 CRC32C 增量计算函数
  extern u32
crc32c_inc(const u8 * buf, u32 nr, u32 crc);
// }}} crc32c // CRC32C 校验和计算函数区域结束

// debug {{{ // 调试相关函数区域开始
  // 触发调试断点
  extern void
debug_break(void);

  // 打印当前函数调用栈
  extern void
debug_backtrace(void);

  // 监视一个 u64 变量的值 (当收到 SIGUSR1 信号时打印)
  extern void
watch_u64_usr1(u64 * const ptr);

#ifndef NDEBUG // 如果未定义 NDEBUG (即调试模式)
  // 断言条件 v 为真，否则终止程序
  extern void
debug_assert(const bool v);
#else // 如果定义了 NDEBUG (即发布模式)
#define debug_assert(expr) ((void)0) // 断言宏为空操作
#endif

__attribute__((noreturn)) // 标记函数不会返回
  // 打印错误信息并终止程序
  extern void
debug_die(void);

__attribute__((noreturn)) // 标记函数不会返回
  // 打印 perror() 信息并终止程序
  extern void
debug_die_perror(void);

  // 将进程的内存映射信息转储到指定文件流
  extern void
debug_dump_maps(FILE * const out);

  // 切换性能分析工具 (如 perf) 的开关状态
  extern bool
debug_perf_switch(void);
// }}} debug // 调试相关函数区域结束

// mm {{{ // 内存管理相关函数区域开始
#ifdef ALLOCFAIL // 如果定义了 ALLOCFAIL (用于测试内存分配失败场景)
  // 模拟内存分配失败
  extern bool
alloc_fail(void);
#endif

  // 按指定对齐方式分配内存
  extern void *
xalloc(const size_t align, const size_t size);

  // 分配内存 (通常是对 malloc 的封装，可能带有错误检查)
  extern void *
yalloc(const size_t size);

  // 分配二维数组内存 (数组元素本身也是指针)
  extern void **
malloc_2d(const size_t nr, const size_t size);

  // 分配并清零二维数组内存
  extern void **
calloc_2d(const size_t nr, const size_t size);

  // 解除内存页映射
  extern void
pages_unmap(void * const ptr, const size_t size);

  // 锁定内存页 (防止被交换到磁盘)
  extern void
pages_lock(void * const ptr, const size_t size);

/* hugepages */ // 大页内存相关
// force posix allocators: -DVALGRIND_MEMCHECK // 强制使用 POSIX 分配器 (用于 Valgrind 内存检查)
  // 分配指定数量的 4KB 内存页
  extern void *
pages_alloc_4kb(const size_t nr_4kb);

  // 分配指定数量的 2MB 大页内存
  extern void *
pages_alloc_2mb(const size_t nr_2mb);

  // 分配指定数量的 1GB 大页内存
  extern void *
pages_alloc_1gb(const size_t nr_1gb);

  // 尝试以最佳方式分配指定大小的内存 (优先使用大页)
  // size_out: 返回实际分配的内存大小
  extern void *
pages_alloc_best(const size_t size, const bool try_1gb, u64 * const size_out);
// }}} mm // 内存管理相关函数区域结束

// process/thread {{{ // 进程/线程相关函数区域开始
  // 获取线程名称
  extern void
thread_get_name(const pthread_t pt, char * const name, const size_t len);

  // 设置线程名称
  extern void
thread_set_name(const pthread_t pt, const char * const name);

  // 获取进程的常驻内存集大小 (RSS)
  extern long
process_get_rss(void);

  // 获取进程可用的 CPU核心数
  extern u32
process_affinity_count(void);

  // 获取进程的 CPU 亲和性列表
  extern u32
process_getaffinity_list(const u32 max, u32 * const cores);

  // 设置线程的 CPU 亲和性列表
  extern void
thread_setaffinity_list(const u32 nr, const u32 * const list);

  // 将当前线程绑定到指定的 CPU 核心
  extern void
thread_pin(const u32 cpu);

  // 获取进程的 CPU 使用时间 (微秒)
  extern u64
process_cpu_time_usec(void);

// if args == true, argx is void ** // 如果 args 为 true, argx 是 void ** 类型
// if args == false, argx is void *  // 如果 args 为 false, argx 是 void * 类型
  // 创建指定数量的线程，执行同一函数，并等待所有线程完成
  extern u64
thread_fork_join(u32 nr, void *(*func) (void *), const bool args, void * const argx);

  // 在指定的 CPU 核心上创建线程
  extern int
thread_create_at(const u32 cpu, pthread_t * const thread, void *(*start_routine) (void *), void * const arg);
// }}} process/thread // 进程/线程相关函数区域结束

// locking {{{ // 锁机制相关区域开始
typedef union { // 自旋锁定义
  u32 opaque; // 不透明数据，用于存储锁状态
} spinlock;

  // 初始化自旋锁
  extern void
spinlock_init(spinlock * const lock);

  // 获取自旋锁 (阻塞等待)
  extern void
spinlock_lock(spinlock * const lock);

  // 尝试获取自旋锁 (非阻塞)
  extern bool
spinlock_trylock(spinlock * const lock);

  // 释放自旋锁
  extern void
spinlock_unlock(spinlock * const lock);

typedef union { // 读写锁定义
  u32 opaque; // 不透明数据
} rwlock;

  // 初始化读写锁
  extern void
rwlock_init(rwlock * const lock);

  // 尝试获取读锁 (非阻塞)
  extern bool
rwlock_trylock_read(rwlock * const lock);

// low-priority reader-lock; use with trylock_write_hp // 低优先级读锁；与高优先级写锁配合使用
  extern bool
rwlock_trylock_read_lp(rwlock * const lock);

  // 尝试获取指定次数的读锁 (非阻塞)
  extern bool
rwlock_trylock_read_nr(rwlock * const lock, u16 nr);

  // 获取读锁 (阻塞等待)
  extern void
rwlock_lock_read(rwlock * const lock);

  // 释放读锁
  extern void
rwlock_unlock_read(rwlock * const lock);

  // 尝试获取写锁 (非阻塞)
  extern bool
rwlock_trylock_write(rwlock * const lock);

  // 尝试获取指定次数的写锁 (非阻塞)
  extern bool
rwlock_trylock_write_nr(rwlock * const lock, u16 nr);

  // 获取写锁 (阻塞等待)
  extern void
rwlock_lock_write(rwlock * const lock);

// writer has higher priority; new readers are blocked // 写者具有更高优先级；新的读者将被阻塞
  extern bool
rwlock_trylock_write_hp(rwlock * const lock);

  // 尝试获取指定次数的高优先级写锁 (非阻塞)
  extern bool
rwlock_trylock_write_hp_nr(rwlock * const lock, u16 nr);

  // 获取高优先级写锁 (阻塞等待)
  extern void
rwlock_lock_write_hp(rwlock * const lock);

  // 释放写锁
  extern void
rwlock_unlock_write(rwlock * const lock);

  // 将写锁转换为读锁 (原子操作)
  extern void
rwlock_write_to_read(rwlock * const lock);

typedef union { // 互斥锁定义
  u64 opqaue[8]; // 不透明数据 (大小为64字节，可能为了缓存行对齐)
} mutex;

  // 初始化互斥锁
  extern void
mutex_init(mutex * const lock);

  // 获取互斥锁 (阻塞等待)
  extern void
mutex_lock(mutex * const lock);

  // 尝试获取互斥锁 (非阻塞)
  extern bool
mutex_trylock(mutex * const lock);

  // 释放互斥锁
  extern void
mutex_unlock(mutex * const lock);

  // 销毁互斥锁
  extern void
mutex_deinit(mutex * const lock);
// }}} locking // 锁机制相关区域结束

// coroutine {{{ // 协程相关函数区域开始
// 协程栈切换 (底层汇编实现)
// saversp: 保存当前栈指针的地址
// newrsp: 新的栈指针
// retval: 传递给新协程的返回值
extern u64 co_switch_stack(u64 * const saversp, const u64 newrsp, const u64 retval);

struct co; // 协程结构体前向声明

  // 创建一个协程
  // stacksize: 协程栈大小
  // func: 协程执行的函数
  // priv: 传递给协程函数的私有数据
  // host: 指向宿主协程栈指针的指针 (用于返回)
  extern struct co *
co_create(const u64 stacksize, void * func, void * priv, u64 * const host);

  // 重用一个已存在的协程对象
  extern void
co_reuse(struct co * const co, void * func, void * priv, u64 * const host);

  // 创建并立即切换到一个新的协程 (类似 fork)
  extern struct co *
co_fork(void * func, void * priv);

  // 获取当前协程的私有数据
  extern void *
co_priv(void);

  // 进入 (切换到) 指定的协程
  extern u64
co_enter(struct co * const to, const u64 retval);

  // 切换到指定的协程
  extern u64
co_switch_to(struct co * const to, const u64 retval);

  // 从当前协程返回到宿主协程
  extern u64
co_back(const u64 retval);

  // 协程退出
  extern void
co_exit(const u64 retval);

  // 检查协程是否有效
  extern bool
co_valid(struct co * const co);

  // 获取当前正在执行的协程对象
  extern struct co *
co_self(void);

  // 销毁协程对象
  extern void
co_destroy(struct co * const co);

struct corr; // 另一种协程实现 (可能用于循环或特定模式)

  // 创建一个 corr 协程
  extern struct corr *
corr_create(const u64 stacksize, void * func, void * priv, u64 * const host);

  // 创建并链接一个新的 corr 协程到前一个协程
  extern struct corr *
corr_link(const u64 stacksize, void * func, void * priv, struct corr * const prev);

  // 重用一个 corr 协程
  extern void
corr_reuse(struct corr * const co, void * func, void * priv, u64 * const host);

  // 重用并重新链接一个 corr 协程
  extern void
corr_relink(struct corr * const co, void * func, void * priv, struct corr * const prev);

  // 进入一个 corr 协程
  extern void
corr_enter(struct corr * const co);

  // corr 协程让出执行权
  extern void
corr_yield(void);

  // corr 协程退出
  extern void
corr_exit(void);

  // 销毁一个 corr 协程
  extern void
corr_destroy(struct corr * const co);
// }}} coroutine // 协程相关函数区域结束

// bits {{{ // 位操作相关函数区域开始
  // 反转32位无符号整数的比特位
  extern u32
bits_reverse_u32(const u32 v);

  // 反转64位无符号整数的比特位
  extern u64
bits_reverse_u64(const u64 v);

  // 64位无符号整数循环左移
  extern u64
bits_rotl_u64(const u64 v, const u8 n);

  // 64位无符号整数循环右移
  extern u64
bits_rotr_u64(const u64 v, const u8 n);

  // 32位无符号整数循环左移
  extern u32
bits_rotl_u32(const u32 v, const u8 n);

  // 32位无符号整数循环右移
  extern u32
bits_rotr_u32(const u32 v, const u8 n);

  // 向上取整到最近的2的幂次方 (64位)
  extern u64
bits_p2_up_u64(const u64 v);

  // 向上取整到最近的2的幂次方 (32位)
  extern u32
bits_p2_up_u32(const u32 v);

  // 向下取整到最近的2的幂次方 (64位)
  extern u64
bits_p2_down_u64(const u64 v);

  // 向下取整到最近的2的幂次方 (32位)
  extern u32
bits_p2_down_u32(const u32 v);

  // 将 v 向上取整到 2^power 的倍数
  extern u64
bits_round_up(const u64 v, const u8 power);

  // 将 v 向上取整到 a 的倍数
  extern u64
bits_round_up_a(const u64 v, const u64 a);

  // 将 v 向下取整到 2^power 的倍数
  extern u64
bits_round_down(const u64 v, const u8 power);

  // 将 v 向下取整到 a 的倍数
  extern u64
bits_round_down_a(const u64 v, const u64 a);
// }}} bits // 位操作相关函数区域结束

// simd {{{ // SIMD 相关函数区域开始
  // 从 m128 类型的 SIMD 向量中提取8位元素的位掩码
  extern u32
m128_movemask_u8(const m128 v);

//  extern u32
//m128_movemask_u16(const m128 v); // 提取16位元素的位掩码 (注释掉了)
//
//  extern u32
//m128_movemask_u32(const m128 v); // 提取32位元素的位掩码 (注释掉了)
// }}} simd // SIMD 相关函数区域结束

// vi128 {{{ // 可变长度整数编码 (VarInt128) 相关函数区域开始
  // 估算编码一个 u32 整数所需的字节数
  extern u32
vi128_estimate_u32(const u32 v);

  // 将 u32 整数编码到目标缓冲区
  extern u8 *
vi128_encode_u32(u8 * dst, u32 v);

  // 从源缓冲区解码一个 u32 整数
  extern const u8 *
vi128_decode_u32(const u8 * src, u32 * const out);

  // 估算编码一个 u64 整数所需的字节数
  extern u32
vi128_estimate_u64(const u64 v);

  // 将 u64 整数编码到目标缓冲区
  extern u8 *
vi128_encode_u64(u8 * dst, u64 v);

  // 从源缓冲区解码一个 u64 整数
  extern const u8 *
vi128_decode_u64(const u8 * src, u64 * const out);
// }}} vi128 // 可变长度整数编码相关函数区域结束

// misc {{{ // 其他杂项函数区域开始
// TODO: only works on little endian? // TODO: 只在小端字节序下工作？
struct entry13 { // what a beautiful name // 一个有趣的结构体名称
  union {
    u16 e1; // 16位字段
    struct { // easy for debugging // 便于调试的结构
      u64 e1_64:16; // e1 的64位版本 (占16位)
      u64 e3:48;    // 48位字段
    };
    u64 v64;  // 整个结构体作为64位整数访问
    void * ptr; // 整个结构体作为指针访问
  };
};

static_assert(sizeof(struct entry13) == 8, "sizeof(entry13) != 8"); // 确保结构体大小为8字节

// directly access read .e1 and .e3 // 可以直接读取 .e1 和 .e3
// directly write .e1 // 可以直接写入 .e1
// use entry13_update() to update the entire entry // 使用 entry13_update() 更新整个条目

  // 创建一个 entry13 结构体实例
  extern struct entry13
entry13(const u16 e1, const u64 e3);

  // 更新 entry13 结构体中的 e3 字段
  extern void
entry13_update_e3(struct entry13 * const e, const u64 e3);

  // 将 u64 整数转换为 void 指针
  extern void *
u64_to_ptr(const u64 v);

  // 将 void 指针转换为 u64 整数
  extern u64
ptr_to_u64(const void * const ptr);

  // 获取 malloc 分配的内存块的可用大小
  extern size_t
m_usable_size(void * const ptr);

  // 获取文件描述符对应的文件大小
  extern size_t
fdsize(const int fd);

  // 计算两个内存区域的最长公共前缀长度
  extern u32
memlcp(const u8 * const p1, const u8 * const p2, const u32 max);

__attribute__ ((format (printf, 2, 3))) // 标记函数类似 printf，进行格式字符串检查
  // 带时间戳和线程ID的日志打印函数
  extern void
logger_printf(const int fd, const char * const fmt, ...);
// }}} misc // 其他杂项函数区域结束

// bitmap {{{ // 位图操作相关函数区域开始
struct bitmap; // 位图结构体前向声明

  // 创建一个指定大小的位图
  extern struct bitmap *
bitmap_create(const u64 nbits);

  // 初始化一个已分配内存的位图
  extern void
bitmap_init(struct bitmap * const bm, const u64 nbits);

  // 测试位图中指定索引的位是否为1
  extern bool
bitmap_test(const struct bitmap * const bm, const u64 idx);

  // 测试位图中所有位是否都为1
  extern bool
bitmap_test_all1(struct bitmap * const bm);

  // 测试位图中所有位是否都为0
  extern bool
bitmap_test_all0(struct bitmap * const bm);

  // 将位图中指定索引的位设置为1
  extern void
bitmap_set1(struct bitmap * const bm, const u64 idx);

  // 将位图中指定索引的位设置为0
  extern void
bitmap_set0(struct bitmap * const bm, const u64 idx);

  // 安全地将位图中指定索引的位设置为1 (64位原子操作)
  extern void
bitmap_set1_safe64(struct bitmap * const bm, const u64 idx);

  // 安全地将位图中指定索引的位设置为0 (64位原子操作)
  extern void
bitmap_set0_safe64(struct bitmap * const bm, const u64 idx);

  // 计算位图中为1的位的数量
  extern u64
bitmap_count(struct bitmap * const bm);

  // 查找位图中第一个为1的位的索引
  extern u64
bitmap_first(struct bitmap * const bm);

  // 将位图中所有位设置为1
  extern void
bitmap_set_all1(struct bitmap * const bm);

  // 将位图中所有位设置为0
  extern void
bitmap_set_all0(struct bitmap * const bm);
// }}} bitmap // 位图操作相关函数区域结束

// slab {{{ // Slab 分配器相关函数区域开始
struct slab; // Slab 分配器结构体前向声明

  // 创建一个 Slab 分配器
  // obj_size: 每个对象的大小
  // blk_size: 每个内存块的大小
  extern struct slab *
slab_create(const u64 obj_size, const u64 blk_size);

  // 非安全地预留指定数量的对象空间 (不进行加锁)
  extern bool
slab_reserve_unsafe(struct slab * const slab, const u64 nr);

  // 非安全地从 Slab 分配一个对象 (不进行加锁)
  extern void *
slab_alloc_unsafe(struct slab * const slab);

  // 安全地从 Slab 分配一个对象 (进行加锁)
  extern void *
slab_alloc_safe(struct slab * const slab);

  // 非安全地释放一个对象到 Slab (不进行加锁)
  extern void
slab_free_unsafe(struct slab * const slab, void * const ptr);

  // 安全地释放一个对象到 Slab (进行加锁)
  extern void
slab_free_safe(struct slab * const slab, void * const ptr);

  // 释放 Slab 中的所有对象
  extern void
slab_free_all(struct slab * const slab);

  // 获取 Slab 中已分配对象的数量
  extern u64
slab_get_nalloc(struct slab * const slab);

  // 销毁 Slab 分配器
  extern void
slab_destroy(struct slab * const slab);
// }}}  slab // Slab 分配器相关函数区域结束

// qsort {{{ // 快速排序和相关函数区域开始
  // u16 类型比较函数 (用于 qsort)
  extern int
compare_u16(const void * const p1, const void * const p2);

  // 对 u16 数组进行快速排序
  extern void
qsort_u16(u16 * const array, const size_t nr);

  // 在已排序的 u16 数组中二分查找指定值
  extern u16 *
bsearch_u16(const u16 v, const u16 * const array, const size_t nr);

  // 打乱 u16 数组的顺序
  extern void
shuffle_u16(u16 * const array, const u64 nr);

  // u32 类型比较函数
  extern int
compare_u32(const void * const p1, const void * const p2);

  // 对 u32 数组进行快速排序
  extern void
qsort_u32(u32 * const array, const size_t nr);

  // 在已排序的 u32 数组中二分查找指定值
  extern u32 *
bsearch_u32(const u32 v, const u32 * const array, const size_t nr);

  // 打乱 u32 数组的顺序
  extern void
shuffle_u32(u32 * const array, const u64 nr);

  // u64 类型比较函数
  extern int
compare_u64(const void * const p1, const void * const p2);

  // 对 u64 数组进行快速排序
  extern void
qsort_u64(u64 * const array, const size_t nr);

  // 在已排序的 u64 数组中二分查找指定值
  extern u64 *
bsearch_u64(const u64 v, const u64 * const array, const size_t nr);

  // 打乱 u64 数组的顺序
  extern void
shuffle_u64(u64 * const array, const u64 nr);

  // double 类型比较函数
  extern int
compare_double(const void * const p1, const void * const p2);

  // 对 double 数组进行快速排序
  extern void
qsort_double(double * const array, const size_t nr);

  // 对 u64 数组进行采样并输出到文件 (用于分析数据分布)
  extern void
qsort_u64_sample(const u64 * const array0, const u64 nr, const u64 res, FILE * const out);

  // 对 double 数组进行采样并输出到文件
  extern void
qsort_double_sample(const double * const array0, const u64 nr, const u64 res, FILE * const out);
// }}} qsort // 快速排序和相关函数区域结束

// xlog {{{ // 简单日志追加与读取工具区域开始
struct xlog; // xlog 结构体前向声明

  // 创建一个 xlog 实例
  // nr_init: 初始记录数量
  // unit_size: 每条记录的大小
  extern struct xlog *
xlog_create(const u64 nr_init, const u64 unit_size);

  // 向 xlog 追加一条记录
  extern void
xlog_append(struct xlog * const xlog, const void * const rec);

  // 向 xlog 追加一条记录 (循环写入，当空间满时覆盖旧记录)
  extern void
xlog_append_cycle(struct xlog * const xlog, const void * const rec);

  // 重置 xlog (清空所有记录)
  extern void
xlog_reset(struct xlog * const xlog);

  // 从 xlog 读取最多 nr_max 条记录到缓冲区
  extern u64
xlog_read(struct xlog * const xlog, void * const buf, const u64 nr_max);

  // 将 xlog 中的所有记录转储到指定文件流
  extern void
xlog_dump(struct xlog * const xlog, FILE * const out);

  // 销毁 xlog 实例
  extern void
xlog_destroy(struct xlog * const xlog);

struct xlog_iter; // xlog 迭代器结构体前向声明

  // 创建一个 xlog 迭代器
  extern struct xlog_iter *
xlog_iter_create(const struct xlog * const xlog);

  // 获取迭代器的下一条记录
  extern bool
xlog_iter_next(struct xlog_iter * const iter, void * const out);
// free iter after use // 使用后需要释放迭代器
// }}} xlog // 简单日志追加与读取工具区域结束

// string {{{ // 字符串操作相关函数区域开始
// XXX strdec_ and strhex_ functions does not append the trailing '\0' to the output string // XXX strdec_ 和 strhex_ 函数不会在输出字符串末尾追加 '\0'
// size of out should be >= 10 // out 的大小应至少为 10
  // 将 u32 整数转换为十进制字符串
  extern void
strdec_32(void * const out, const u32 v);

// size of out should be >= 20 // out 的大小应至少为 20
  // 将 u64 整数转换为十进制字符串
  extern void
strdec_64(void * const out, const u64 v);

// size of out should be >= 8 // out 的大小应至少为 8
  // 将 u32 整数转换为十六进制字符串
  extern void
strhex_32(void * const out, const u32 v);

// size of out should be >= 16 // out 的大小应至少为 16
  // 将 u64 整数转换为十六进制字符串
  extern void
strhex_64(void * const out, const u64 v);

  // 将字符串转换为 u64 整数
  extern u64
a2u64(const void * const str);

  // 将字符串转换为 u32 整数
  extern u32
a2u32(const void * const str);

  // 将字符串转换为 s64 整数
  extern s64
a2s64(const void * const str);

  // 将字符串转换为 s32 整数
  extern s32
a2s32(const void * const str);

  // 以十六进制格式打印内存数据到文件流
  extern void
str_print_hex(FILE * const out, const void * const data, const u32 len);

  // 以十进制格式打印内存数据到文件流 (通常用于字节数组)
  extern void
str_print_dec(FILE * const out, const void * const data, const u32 len);

// user should free returned ptr (and nothing else) after use // 用户使用后应释放返回的指针 (只需释放最外层指针)
  // 将字符串按指定分隔符分割成多个子字符串
  extern char **
strtoks(const char * const str, const char * const delim);

  // 计算 strtoks 返回的子字符串数量
  extern u32
strtoks_count(const char * const * const toks);
// }}} string // 字符串操作相关函数区域结束

// qsbr {{{ // QSBR (Quiescent State-Based Reclamation) 内存回收机制区域开始
// QSBR vs EBR (Quiescent-State vs Epoch Based Reclaimation)
// QSBR: readers just use qsbr_update -> qsbr_update -> ... repeatedly
// EBR: readers use qsbr_update -> qsbr_park -> qsbr_resume -> qsbr_update -> ...
// The advantage of EBR is qsbr_park can happen much earlier than the next qsbr_update
// The disadvantage is the extra cost, a pair of park/resume is used in every iteration
// QSBR: 读者只需重复调用 qsbr_update -> qsbr_update -> ...
// EBR: 读者使用 qsbr_update -> qsbr_park -> qsbr_resume -> qsbr_update -> ...
// EBR 的优点是 qsbr_park 可以在下一次 qsbr_update 之前更早地发生
// EBR 的缺点是额外的开销，每次迭代都使用一对 park/resume
struct qsbr; // QSBR 主结构体前向声明
struct qsbr_ref { // QSBR 线程引用结构体
#ifdef QSBR_DEBUG // 如果定义了 QSBR_DEBUG
  u64 debug[16]; // 调试信息
#endif
  u64 opaque[3]; // 不透明数据，用于存储线程状态
};

  // 创建一个 QSBR 实例
  extern struct qsbr *
qsbr_create(void);

// every READER accessing the shared data must first register itself with the qsbr // 每个访问共享数据的读者线程必须首先向 QSBR 注册
  extern bool
qsbr_register(struct qsbr * const q, struct qsbr_ref * const qref);

  // 从 QSBR 注销一个读者线程
  extern void
qsbr_unregister(struct qsbr * const q, struct qsbr_ref * const qref);

// For READER: mark the beginning of critical section; like rcu_read_lock() // 对于读者：标记临界区的开始；类似 rcu_read_lock()
  // 读者线程更新其静止状态版本号
  extern void
qsbr_update(struct qsbr_ref * const qref, const u64 v);

// temporarily stop access the shared data to avoid blocking writers // 暂时停止访问共享数据以避免阻塞写者
// READER can use qsbr_park (like rcu_read_unlock()) in conjunction with qsbr_update // 读者可以使用 qsbr_park (类似 rcu_read_unlock()) 配合 qsbr_update
// qsbr_park is roughly equivalent to qsbr_unregister, but faster // qsbr_park 大致等同于 qsbr_unregister，但更快
  // 读者线程暂停对共享数据的访问
  extern void
qsbr_park(struct qsbr_ref * const qref);

// undo the effect of qsbr_park; must use it between qsbr_park and qsbr_update // 撤销 qsbr_park 的效果；必须在 qsbr_park 和 qsbr_update 之间使用
// qsbr_resume is roughly equivalent to qsbr_register, but faster // qsbr_resume 大致等同于 qsbr_register，但更快
  // 读者线程恢复对共享数据的访问
  extern void
qsbr_resume(struct qsbr_ref * const qref);

// WRITER: wait until all the readers have announced v=target with qsbr_update // 对于写者：等待所有读者都通过 qsbr_update 声明其版本号达到 target
  extern void
qsbr_wait(struct qsbr * const q, const u64 target);

  // 销毁 QSBR 实例
  extern void
qsbr_destroy(struct qsbr * const q);
// }}} qsbr // QSBR 内存回收机制区域结束

#ifdef __cplusplus // 如果是 C++ 环境
} // extern "C" 结束
#endif
// vim:fdm=marker // vim 折叠标记