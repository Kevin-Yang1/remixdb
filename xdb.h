/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#pragma once // 防止头文件被多次包含

#include "lib.h" // 包含基础库
#include "kv.h"  // 包含键值对 (KV) 定义

#ifdef __cplusplus // 如果是 C++ 环境
extern "C" { // 使用 C 语言链接方式
#endif

// 前向声明 XDB 相关结构体
struct xdb;
struct xdb_ref;
struct xdb_iter;

// xdb {{{ // XDB API 定义区域开始
  // 打开一个 XDB 数据库实例
  // 参数:
  //   dir: 数据库目录路径
  //   cache_size_mb: SSTable 缓存大小 (MB)
  //   mt_size_mb: 内存表大小 (MB)
  //   wal_size_mb: WAL 文件大小 (MB)
  //   ckeys: 是否为 SSTable 生成压缩键 (compact keys)
  //   tags: 是否使用哈希标签 (用于加速点查)
  //   nr_workers: 压缩工作线程数
  //   co_per_worker: 每个压缩工作线程的协程数
  //   worker_cores: 压缩工作线程绑定的 CPU 核心配置字符串
  extern struct xdb *
xdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const size_t wal_size_mb,
    const bool ckeys, const bool tags, const u32 nr_workers, const u32 co_per_worker, const char * const worker_cores);

  // 关闭一个 XDB 数据库实例
  extern void
xdb_close(struct xdb * const xdb);

// kvmap_api // kvmap API 相关函数
  // 获取一个 XDB 数据库的引用 (通常每个线程持有一个)
  extern struct xdb_ref *
xdb_ref(struct xdb * const xdb);

  // 释放一个 XDB 数据库的引用
  extern struct xdb*
xdb_unref(struct xdb_ref * const ref);

  // 从数据库中获取指定键的值
  // 参数:
  //   ref: XDB 数据库引用
  //   kref: 键引用
  //   out: (可选) 用于存储结果的 KV 对象缓冲区；如果为 NULL，则函数会分配新的内存
  // 返回: 指向 KV 对象的指针 (如果找到且非删除标记)，否则返回 NULL。调用者需要负责释放返回的 KV 对象 (如果 out 为 NULL)。
  extern struct kv *
xdb_get(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const out);

  // 探测数据库中是否存在指定的键 (不返回值内容)
  extern bool
xdb_probe(struct xdb_ref * const ref, const struct kref * const kref);

  // 向数据库中插入或更新一个键值对
  extern bool
xdb_put(struct xdb_ref * const ref, const struct kv * const kv);

  // 从数据库中删除一个键 (通过写入删除标记实现)
  extern bool
xdb_del(struct xdb_ref * const ref, const struct kref * const kref);

  // 将 WAL (预写日志) 缓冲区的数据同步到磁盘
  extern void
xdb_sync(struct xdb_ref * const ref);

// AKA Atomic Read-Modify-Write (原子读-改-写操作)
// 由于分配失败，合并操作可能会在未执行任何操作的情况下失败。
// 由于中止和重试，uf() 可能会被多次调用 (这些不是错误)。
// 如果最后一次调用成功，则最后一次调用将产生实际效果。
// 返回的 kvs 将被忽略，除了最后一个 (由最后一次调用 uf 返回的那个)。
// uf 分配的内存必须在 xdb_merge 返回后由调用者释放。
// 如果 kv0 不为 NULL，uf 可以执行原地更新 (只需从 uf() 返回 kv0)。
// 如果 kv0 不是来自内存表 (从分区加载的)，原地更新仍可能导致内存表插入。
  extern bool
xdb_merge(struct xdb_ref * const ref, const struct kref * const kref, kv_merge_func uf, void * const priv);

// iter // 迭代器相关函数
  // 创建一个新的 XDB 迭代器
  extern struct xdb_iter *
xdb_iter_create(struct xdb_ref * const ref);

  // 停放迭代器 (释放其可能持有的资源，如锁，允许其他操作进行)
  extern void
xdb_iter_park(struct xdb_iter * const iter);

  // 将迭代器定位到指定的键 (或大于等于该键的第一个键)
  extern void
xdb_iter_seek(struct xdb_iter * const iter, const struct kref * const key);

  // 检查迭代器当前是否指向一个有效的 KV 对
  extern bool
xdb_iter_valid(struct xdb_iter * const iter);

  // 获取迭代器当前指向的 KV 对 (不移动迭代器)
  // 参数:
  //   out: (可选) 用于存储结果的 KV 对象缓冲区；如果为 NULL，则函数会分配新的内存
  // 返回: 指向 KV 对象的指针，如果迭代器无效则返回 NULL。调用者需要负责释放返回的 KV 对象 (如果 out 为 NULL)。
  extern struct kv *
xdb_iter_peek(struct xdb_iter * const iter, struct kv * const out);

  // 获取迭代器当前指向的键引用
  extern bool
xdb_iter_kref(struct xdb_iter * const iter, struct kref * const kref);

  // 获取迭代器当前指向的键值引用
  extern bool
xdb_iter_kvref(struct xdb_iter * const iter, struct kvref * const kvref);

  // 将迭代器向前移动一个唯一的键
  extern void
xdb_iter_skip1(struct xdb_iter * const iter);

  // 将迭代器向前移动 n 个唯一的键
  extern void
xdb_iter_skip(struct xdb_iter * const iter, u32 n);

  // 获取迭代器当前指向的 KV 对，并将迭代器向前移动一个唯一的键
  // 参数:
  //   out: (可选) 用于存储结果的 KV 对象缓冲区；如果为 NULL，则函数会分配新的内存
  // 返回: 指向 KV 对象的指针，如果迭代器无效则返回 NULL。调用者需要负责释放返回的 KV 对象 (如果 out 为 NULL)。
  extern struct kv*
xdb_iter_next(struct xdb_iter * const iter, struct kv * const out);

  // 销毁 XDB 迭代器并释放相关资源
  extern void
xdb_iter_destroy(struct xdb_iter * const iter);

// 指向 XDB 的 kvmap_api 实现的全局变量
extern const struct kvmap_api kvmap_api_xdb;
// }}} xdb // XDB API 定义区域结束

// remixdb {{{ // RemixDB API 定义区域开始 (对 XDB 的简化封装)
  // 打开一个 RemixDB 数据库实例 (使用推荐的默认配置)
  // 参数:
  //   dir: 数据库目录路径
  //   cache_size_mb: SSTable 缓存大小 (MB)
  //   mt_size_mb: 内存表大小 (MB)
  //   tags: 是否使用哈希标签
  extern struct xdb *
remixdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const bool tags);

  // 打开一个 RemixDB 数据库实例 (使用紧凑模式配置，优化磁盘空间)
  // 参数:
  //   dir: 数据库目录路径
  //   cache_size_mb: SSTable 缓存大小 (MB)
  //   mt_size_mb: 内存表大小 (MB)
  extern struct xdb *
remixdb_open_compact(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb);

  // 获取一个 RemixDB 数据库的引用 (内部调用 xdb_ref)
  extern struct xdb_ref *
remixdb_ref(struct xdb * const xdb);

  // 释放一个 RemixDB 数据库的引用 (内部调用 xdb_unref)
  extern void
remixdb_unref(struct xdb_ref * const ref);

  // 关闭一个 RemixDB 数据库实例 (内部调用 xdb_close)
  extern void
remixdb_close(struct xdb * const xdb);

  // 向数据库中插入或更新一个键值对
  // 参数:
  //   ref: 数据库引用
  //   kbuf: 键数据缓冲区
  //   klen: 键长度
  //   vbuf: 值数据缓冲区
  //   vlen: 值长度
  extern bool
remixdb_put(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
    const void * const vbuf, const u32 vlen);

  // 从数据库中删除一个键
  extern bool
remixdb_del(struct xdb_ref * const ref, const void * const kbuf, const u32 klen);

  // 探测数据库中是否存在指定的键
  extern bool
remixdb_probe(struct xdb_ref * const ref, const void * const kbuf, const u32 klen);

  // 从数据库中获取指定键的值
  // 参数:
  //   vbuf_out: 用于存储值的输出缓冲区
  //   vlen_out: 指向用于存储值长度的变量的指针
  // 返回: 如果找到键且非删除标记，则返回 true，并将值复制到 vbuf_out，长度写入 *vlen_out；否则返回 false。
  extern bool
remixdb_get(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
    void * const vbuf_out, u32 * const vlen_out);

  // 将 WAL 数据同步到磁盘
  extern void
remixdb_sync(struct xdb_ref * const ref);

  // 创建一个新的 RemixDB 迭代器
  extern struct xdb_iter *
remixdb_iter_create(struct xdb_ref * const ref);

  // 将迭代器定位到指定的键
  extern void
remixdb_iter_seek(struct xdb_iter * const iter, const void * const kbuf, const u32 klen);

  // 检查迭代器是否有效
  extern bool
remixdb_iter_valid(struct xdb_iter * const iter);

  // 获取迭代器当前指向的键值对 (不移动迭代器)
  // 参数:
  //   kbuf_out: (可选) 用于存储键的输出缓冲区
  //   klen_out: (可选) 指向用于存储键长度的变量的指针
  //   vbuf_out: (可选) 用于存储值的输出缓冲区
  //   vlen_out: (可选) 指向用于存储值长度的变量的指针
  // 返回: 如果迭代器有效，则返回 true，并填充提供的缓冲区；否则返回 false。
  extern bool
remixdb_iter_peek(struct xdb_iter * const iter,
    void * const kbuf_out, u32 * const klen_out,
    void * const vbuf_out, u32 * const vlen_out);

  // 将迭代器向前移动一个唯一的键
  extern void
remixdb_iter_skip1(struct xdb_iter * const iter);

  // 将迭代器向前移动 nr 个唯一的键
  extern void
remixdb_iter_skip(struct xdb_iter * const iter, const u32 nr);

  // 停放迭代器
  extern void
remixdb_iter_park(struct xdb_iter * const iter);

  // 销毁 RemixDB 迭代器
  extern void
remixdb_iter_destroy(struct xdb_iter * const iter);
// }}} remixdb // RemixDB API 定义区域结束

#ifdef __cplusplus // 如果是 C++ 环境
} // extern "C" 结束
#endif
// vim:fdm=marker // vim 折叠标记