/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#pragma once // 防止头文件重复包含

#include "lib.h" // 基础库
#if defined(LIBURING) // 如果定义了LIBURING
#include <liburing.h> // 包含liburing头文件
#endif // LIBURING

#ifdef __cplusplus // 如果是C++编译器
extern "C" { // 使用C语言的方式进行链接
#endif

// wring {{{
struct wring; // 写环形缓冲区

// iosz: 固定的写入大小; 必须是PGSZ的倍数
  extern struct wring *
wring_create(const int fd, const u32 iosz, const u32 depth); // 创建写环形缓冲区

  extern void
wring_update_fd(struct wring * const wring, const int fd); // 更新文件描述符

  extern void
wring_destroy(struct wring * const wring); // 销毁写环形缓冲区

  extern void *
wring_acquire(struct wring * const wring); // 获取一个可用的缓冲区

// 写入部分缓冲区
  extern void
wring_write_partial(struct wring * const wring, const off_t off,
    void * const buf, const size_t buf_off, const u32 size);

  extern void
wring_write(struct wring * const wring, const off_t off, void * const buf); // 写入缓冲区

// 刷新队列并等待完成
  extern void
wring_flush(struct wring * const wring);

// 发送一个fsync请求，但不等待其完成
  extern void
wring_fsync(struct wring * const wring);
// }}} wring

// coq {{{

struct coq; // 协程队列
typedef bool (*cowq_func) (void * priv); // 协程工作队列函数指针

  extern struct coq *
coq_create(void); // 创建协程队列

  extern void
coq_destroy(struct coq * const coq); // 销毁协程队列

// 在Linux上优先使用io_uring；回退到POSIX AIO
  extern struct coq *
coq_create_auto(const u32 depth); // 自动创建协程队列

  extern void
coq_destroy_auto(struct coq * const coq); // 自动销毁协程队列

  extern u32
corq_enqueue(struct coq * const q, struct co * const co); // 将协程加入队列

  extern u32
cowq_enqueue(struct coq * const q, cowq_func exec, void * const priv); // 将工作函数加入队列

  extern void
cowq_remove(struct coq * const q, const u32 i); // 从队列中移除工作函数

  extern void
coq_yield(struct coq * const q); // 协程主动让出

  extern void
coq_idle(struct coq * const q); // 空闲时调用

  extern void
coq_run(struct coq * const q); // 运行协程

  extern void
coq_install(struct coq * const q); // 安装协程队列

  extern void
coq_uninstall(void); // 卸载协程队列

  extern struct coq *
coq_current(void); // 获取当前的协程队列

  extern ssize_t
coq_pread_aio(struct coq * const q, const int fd, void * const buf, const size_t count, const off_t offset); // 异步读

  extern ssize_t
coq_pwrite_aio(struct coq * const q, const int fd, const void * const buf, const size_t count, const off_t offset); // 异步写

#if defined(LIBURING)
// io_uring-specific
  extern struct io_uring *
coq_uring_create(const u32 depth); // 创建io_uring

// 在pread_uring和pwrite_uring中使用ring==NULL
  extern struct coq *
coq_uring_create_pair(const u32 depth); // 创建io_uring对

  extern void
coq_uring_destroy(struct io_uring * const ring); // 销毁io_uring

  extern void
coq_uring_destroy_pair(struct coq * const coq); // 销毁io_uring对

  extern ssize_t
coq_pread_uring(struct coq * const q, struct io_uring * const ring,
    const int fd, void * const buf, const size_t count, const off_t offset); // 使用io_uring进行异步读

  extern ssize_t
coq_pwrite_uring(struct coq * const q, struct io_uring * const ring,
    const int fd, const void * const buf, const size_t count, const off_t offset); // 使用io_uring进行异步写
#endif // LIBURING
// }}} coq

// rcache {{{
  extern struct rcache *
rcache_create(const u64 size_mb, const u32 fd_bits); // 创建读缓存

  extern void
rcache_destroy(struct rcache * const c); // 销毁读缓存

  extern void
rcache_close_lazy(struct rcache * const c, const int fd); // 延迟关闭

  extern u64
rcache_close_flush(struct rcache * const c); // 刷新关闭

  extern void
rcache_close(struct rcache * const c, const int fd); // 关闭

  extern void *
rcache_acquire(struct rcache * const c, const int fd, const u32 pageid); // 获取页面

  extern void
rcache_retain(struct rcache * const c, const void * const buf); // 保留页面

  extern void
rcache_release(struct rcache * const c, const void * const buf); // 释放页面

  extern void
rcache_thread_stat_reset(void); // 重置线程统计信息

  extern u64
rcache_thread_stat_reads(void); // 获取线程读统计信息
// }}} rcache

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
