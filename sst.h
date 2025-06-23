/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#pragma once

#include "blkio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SST_VLEN_TS ((0x10000u)) // 墓碑标记 (tomb stone)
#define SST_VLEN_MASK ((0xffffu)) // 实际值长度掩码 real vlen == vlen & 0xffff

// kv {{{
  /**
   * @brief 估算 kv 结构序列化后的大小
   */
  extern size_t
sst_kv_vi128_estimate(const struct kv * const kv);

  /**
   * @brief 将 kv 结构序列化到 ptr 指向的内存
   */
  extern u8 *
sst_kv_vi128_encode(u8 * ptr, const struct kv * const kv);

  /**
   * @brief 计算 kv 结构在内存中的总大小
   */
  extern size_t
sst_kv_size(const struct kv * const kv);

  /**
   * @brief 将 kvref 复制到一个新的 kv 结构中
   */
  extern struct kv *
sst_kvref_dup2_kv(struct kvref * const kvref, struct kv * const out);
// }}} kv

// mm {{{

  /**
   * @brief 带墓碑标记的内存映射输入转换函数
   */
  extern struct kv *
kvmap_mm_in_ts(struct kv * const kv, void * const priv);

  /**
   * @brief 带墓碑标记的内存映射输出转换函数
   */
  extern struct kv *
kvmap_mm_out_ts(struct kv * const kv, struct kv * const out);

// 带墓碑标记的内存映射 API
extern const struct kvmap_mm kvmap_mm_ts;
// }}} mm

// sst {{{
// 单个排序字符串表 (Sorted String Table)
struct sst;

  /**
   * @brief 打开一个 SST 文件
   * @param dirname 目录名
   * @param seq 序列号
   * @param way 路数
   */
  extern struct sst *
sst_open(const char * const dirname, const u64 seq, const u32 way);

  /**
   * @brief 获取 SST 的元数据
   */
  extern const struct sst_meta *
sst_meta(struct sst * const sst);

  /**
   * @brief 为 SST 设置读缓存
   */
  extern void
sst_rcache(struct sst * const sst, struct rcache * const rc);

  /**
   * @brief 从 SST 中获取一个键值对
   */
  extern struct kv *
sst_get(struct sst * const map, const struct kref * const key, struct kv * const out);

  /**
   * @brief 检查一个键是否存在于 SST 中
   */
  extern bool
sst_probe(struct sst* const map, const struct kref * const key);

  /**
   * @brief 获取 SST 中的第一个键
   */
  extern struct kv *
sst_first_key(struct sst * const map, struct kv * const out);

  /**
   * @brief 获取 SST 中的最后一个键
   */
  extern struct kv *
sst_last_key(struct sst * const map, struct kv * const out);

  /**
   * @brief 销毁（关闭）一个 SST
   */
  extern void
sst_destroy(struct sst * const map);

  /**
   * @brief 将 SST 的内容转储到文件
   */
  extern void
sst_dump(struct sst * const sst, const char * const fn);

  /**
   * @brief 将 SST 的信息打印到文件流
   */
  extern void
sst_fprint(struct sst * const map, FILE * const out);

// SST 迭代器
struct sst_iter;

  /**
   * @brief 创建一个 SST 迭代器
   */
  extern struct sst_iter *
sst_iter_create(struct sst * const sst);

  /**
   * @brief 检查当前迭代器指向的是否是墓碑
   */
  extern bool
sst_iter_ts(struct sst_iter * const iter);

  /**
   * @brief 将迭代器定位到指定的键
   */
  extern void
sst_iter_seek(struct sst_iter * const iter, const struct kref * const key);

  /**
   * @brief 将迭代器定位到开头 (NULL)
   */
  extern void
sst_iter_seek_null(struct sst_iter * const iter);

  /**
   * @brief 检查迭代器是否有效
   */
  extern bool
sst_iter_valid(struct sst_iter * const iter);

  /**
   * @brief 查看当前迭代器指向的键值对，但不移动迭代器
   */
  extern struct kv *
sst_iter_peek(struct sst_iter * const iter, struct kv * const out);

  /**
   * @brief 获取当前迭代器指向的键引用
   */
  extern bool
sst_iter_kref(struct sst_iter * const iter, struct kref * const kref);

  /**
   * @brief 获取当前迭代器指向的键值对引用
   */
  extern bool
sst_iter_kvref(struct sst_iter * const iter, struct kvref * const kvref);

  /**
   * @brief 保留迭代器状态，防止被修改
   */
  extern u64
sst_iter_retain(struct sst_iter * const iter);

  /**
   * @brief 释放由 sst_iter_retain 保留的状态
   */
  extern void
sst_iter_release(struct sst_iter * const iter, const u64 opaque);

  /**
   * @brief 迭代器向前移动一个位置
   */
  extern void
sst_iter_skip1(struct sst_iter * const iter);

  /**
   * @brief 迭代器向前移动 nr 个位置
   */
  extern void
sst_iter_skip(struct sst_iter * const iter, const u32 nr);

  /**
   * @brief 获取下一个键值对并移动迭代器
   */
  extern struct kv *
sst_iter_next(struct sst_iter * const iter, struct kv * const out);

  /**
   * @brief 暂停迭代器，释放部分资源
   */
  extern void
sst_iter_park(struct sst_iter * const iter);

  u64
sst_iter_retain(struct sst_iter * const iter);

  void
sst_iter_release(struct sst_iter * const iter, const u64 opaque);

  /**
   * @brief 销毁 SST 迭代器
   */
  extern void
sst_iter_destroy(struct sst_iter * const iter);
// }}} sst

// build-sst {{{
// api 包含有序的键，并支持 iter_next()。
// map_api 中的所有键都将被添加到 sstable 中。
  /**
   * @brief 从一个内存迭代器构建 SST
   * @param dirname 目录名
   * @param miter 内存迭代器
   * @param seq 序列号
   * @param way 路数
   * @param maxblkid0 块 ID 上限
   * @param del 构建后是否删除源
   * @param ckeys 是否压缩键
   * @param k0 最小键
   * @param kz 最大键
   */
  extern u64
sst_build(const char * const dirname, struct miter * const miter,
    const u64 seq, const u32 way, const u32 maxblkid0, const bool del, const bool ckeys,
    const struct kv * const k0, const struct kv * const kz);
// }}} build-sst

// msstx {{{
// msst (multi-sst) 多个 SST 的集合
struct msst;
struct msstx_iter;

// msstx: msst 的简单实现
  /**
   * @brief 打开一个 msstx 实例（一组 SST）
   */
  extern struct msst *
msstx_open(const char * const dirname, const u64 seq, const u32 nway);

  /**
   * @brief 为 msst 设置读缓存
   */
  extern void
msst_rcache(struct msst * const msst, struct rcache * const rc);

  /**
   * @brief 销毁 msstx 实例
   */
  extern void
msstx_destroy(struct msst * const msst);

  /**
   * @brief 创建 msstx 迭代器
   */
  extern struct msstx_iter *
msstx_iter_create(struct msst * const msst);

  /**
   * @brief 从 msstx 中获取一个键值对
   */
  extern struct kv *
msstx_get(struct msst * const msst, const struct kref * const key, struct kv * const out);

  /**
   * @brief 检查键是否存在于 msstx 中
   */
  extern bool
msstx_probe(struct msst * const msst, const struct kref * const key);

  /**
   * @brief 检查 msstx 迭代器是否有效
   */
  extern bool
msstx_iter_valid(struct msstx_iter * const iter);

  /**
   * @brief 定位 msstx 迭代器
   */
  extern void
msstx_iter_seek(struct msstx_iter * const iter, const struct kref * const key);

  /**
   * @brief 将 msstx 迭代器定位到开头
   */
  extern void
msstx_iter_seek_null(struct msstx_iter * const iter);

  /**
   * @brief 查看 msstx 迭代器当前项
   */
  extern struct kv *
msstx_iter_peek(struct msstx_iter * const iter, struct kv * const out);

  /**
   * @brief 获取 msstx 迭代器当前键引用
   */
  extern bool
msstx_iter_kref(struct msstx_iter * const iter, struct kref * const kref);

  /**
   * @brief 获取 msstx 迭代器当前键值对引用
   */
  extern bool
msstx_iter_kvref(struct msstx_iter * const iter, struct kvref * const kvref);

  /**
   * @brief 保留 msstx 迭代器状态
   */
  extern u64
msstx_iter_retain(struct msstx_iter * const iter);

  /**
   * @brief 释放 msstx 迭代器保留的状态
   */
  extern void
msstx_iter_release(struct msstx_iter * const iter, const u64 opaque);

  /**
   * @brief msstx 迭代器前进一格
   */
  extern void
msstx_iter_skip1(struct msstx_iter * const iter);

  /**
   * @brief msstx 迭代器前进 n 格
   */
  extern void
msstx_iter_skip(struct msstx_iter * const iter, const u32 nr);

  /**
   * @brief 获取 msstx 迭代器下一项
   */
  extern struct kv *
msstx_iter_next(struct msstx_iter * const iter, struct kv * const out);

  /**
   * @brief 暂停 msstx 迭代器
   */
  extern void
msstx_iter_park(struct msstx_iter * const iter);

  /**
   * @brief 销毁 msstx 迭代器
   */
  extern void
msstx_iter_destroy(struct msstx_iter * const iter);
// }}} msstx

// ssty {{{
// ssty: SST 的扩展元数据
struct ssty;

  /**
   * @brief 打开 ssty
   */
  extern struct ssty *
ssty_open(const char * const dirname, const u64 seq, const u32 nway);

  /**
   * @brief 销毁 ssty
   */
  extern void
ssty_destroy(struct ssty * const ssty);

  /**
   * @brief 打印 ssty 信息
   */
  extern void
ssty_fprint(struct ssty * const ssty, FILE * const fout);
// }}} ssty

// mssty {{{
// mssty: 带有扩展元数据的多SST视图
struct mssty_ref;
struct mssty_iter;

  /**
   * @brief 为一个已有的 msst 打开 y-元数据
   */
  extern bool
mssty_open_y(const char * const dirname, struct msst * const msst);

  /**
   * @brief 打开一个 mssty 实例
   */
  extern struct msst *
mssty_open(const char * const dirname, const u64 seq, const u32 nway);

  /**
   * @brief 销毁 mssty 实例
   */
  extern void
mssty_destroy(struct msst * const msst);

  /**
   * @brief 打印 mssty 信息
   */
  extern void
mssty_fprint(struct msst * const msst, FILE * const fout);

  /**
   * @brief 获取 mssty 的引用
   */
  extern struct mssty_ref *
mssty_ref(struct msst * const msst);

  /**
   * @brief 释放 mssty 的引用
   */
  extern struct msst *
mssty_unref(struct mssty_ref * const ref);

  /**
   * @brief 创建 mssty 迭代器
   */
  extern struct mssty_iter *
mssty_iter_create(struct mssty_ref * const ref);

  /**
   * @brief 检查 mssty 迭代器是否有效
   */
  extern bool
mssty_iter_valid(struct mssty_iter * const iter);

  /**
   * @brief 定位 mssty 迭代器
   */
  extern void
mssty_iter_seek(struct mssty_iter * const iter, const struct kref * const key);

  /**
   * @brief 将 mssty 迭代器定位到开头
   */
  extern void
mssty_iter_seek_null(struct mssty_iter * const iter);

  /**
   * @brief 将迭代器定位到 key 附近
   */
  extern void
mssty_iter_seek_near(struct mssty_iter * const iter, const struct kref * const key, const bool bsearch_keys);

  /**
   * @brief 查看 mssty 迭代器当前项
   */
  extern struct kv *
mssty_iter_peek(struct mssty_iter * const iter, struct kv * const out);

  /**
   * @brief 获取 mssty 迭代器当前键引用
   */
  extern bool
mssty_iter_kref(struct mssty_iter * const iter, struct kref * const kref);

  /**
   * @brief 获取 mssty 迭代器当前键值对引用
   */
  extern bool
mssty_iter_kvref(struct mssty_iter * const iter, struct kvref * const kvref);

  /**
   * @brief 保留 mssty 迭代器状态
   */
  extern u64
mssty_iter_retain(struct mssty_iter * const iter);

  /**
   * @brief 释放 mssty 迭代器保留的状态
   */
  extern void
mssty_iter_release(struct mssty_iter * const iter, const u64 opaque);

  /**
   * @brief mssty 迭代器前进一格
   */
  extern void
mssty_iter_skip1(struct mssty_iter * const iter);

  /**
   * @brief mssty 迭代器前进 n 格
   */
  extern void
mssty_iter_skip(struct mssty_iter * const iter, const u32 nr);

  /**
   * @brief 获取 mssty 迭代器下一项
   */
  extern struct kv *
mssty_iter_next(struct mssty_iter * const iter, struct kv * const out);

  /**
   * @brief 暂停 mssty 迭代器
   */
  extern void
mssty_iter_park(struct mssty_iter * const iter);

  /**
   * @brief 销毁 mssty 迭代器
   */
  extern void
mssty_iter_destroy(struct mssty_iter * const iter);

// ts iter: 如果一个键的最新版本是墓碑，则忽略它
  /**
   * @brief 检查当前迭代器（ts模式）是否指向墓碑
   */
  extern bool
mssty_iter_ts(struct mssty_iter * const iter);

  /**
   * @brief 定位迭代器（ts模式），跳过墓碑
   */
  extern void
mssty_iter_seek_ts(struct mssty_iter * const iter, const struct kref * const key);

  /**
   * @brief 迭代器前进一格（ts模式），跳过墓碑
   */
  extern void
mssty_iter_skip1_ts(struct mssty_iter * const iter);

  /**
   * @brief 迭代器前进 n 格（ts模式），跳过墓碑
   */
  extern void
mssty_iter_skip_ts(struct mssty_iter * const iter, const u32 nr);

  /**
   * @brief 获取下一项（ts模式），跳过墓碑
   */
  extern struct kv *
mssty_iter_next_ts(struct mssty_iter * const iter, struct kv * const out);

// dup iter: 返回所有版本，包括旧键和墓碑
  /**
   * @brief 查看当前项（dup模式），包括所有版本
   */
  extern struct kv *
mssty_iter_peek_dup(struct mssty_iter * const iter, struct kv * const out);

  /**
   * @brief 迭代器前进一格（dup模式）
   */
  extern void
mssty_iter_skip1_dup(struct mssty_iter * const iter);

  /**
   * @brief 迭代器前进 n 格（dup模式）
   */
  extern void
mssty_iter_skip_dup(struct mssty_iter * const iter, const u32 nr);

  /**
   * @brief 获取下一项（dup模式）
   */
  extern struct kv *
mssty_iter_next_dup(struct mssty_iter * const iter, struct kv * const out);

  /**
   * @brief 获取当前键引用（dup模式）
   */
  extern bool
mssty_iter_kref_dup(struct mssty_iter * const iter, struct kref * const kref);

  /**
   * @brief 获取当前键值对引用（dup模式）
   */
  extern bool
mssty_iter_kvref_dup(struct mssty_iter * const iter, struct kvref * const kvref);

// mssty_get 可能返回墓碑
  extern struct kv *
mssty_get(struct mssty_ref * const ref, const struct kref * const key, struct kv * const out);

// mssty_probe 可能返回墓碑
  extern bool
mssty_probe(struct mssty_ref * const ref, const struct kref * const key);

// 对墓碑返回 NULL
  extern struct kv *
mssty_get_ts(struct mssty_ref * const ref, const struct kref * const key, struct kv * const out);

// 对墓碑返回 false
  extern bool
mssty_probe_ts(struct mssty_ref * const ref, const struct kref * const key);

  /**
   * @brief 获取值（ts模式），跳过墓碑
   */
  extern bool
mssty_get_value_ts(struct mssty_ref * const ref, const struct kref * const key,
    void * const vbuf_out, u32 * const vlen_out);

  /**
   * @brief 获取 msst 中的第一个键值对
   */
  extern struct kv *
mssty_first(struct msst * const msst, struct kv * const out);

  /**
   * @brief 获取 msst 中的最后一个键值对
   */
  extern struct kv *
mssty_last(struct msst * const msst, struct kv * const out);

  /**
   * @brief 转储 msst 内容到文件
   */
  extern void
mssty_dump(struct msst * const msst, const char * const fn);
// }}} mssty

// build-ssty {{{
// 基于一组 sstable 构建扩展元数据。
// y0 和 way0 是可选的，用于加速排序
  extern u32
ssty_build(const char * const dirname, struct msst * const msst,
    const u64 seq, const u32 way, struct msst * const y0, const u32 way0, const bool tags);
// }}} build-ssty

// msstv {{{
// msstv: 多SST的版本化视图
struct msstv;
struct msstv_iter;
struct msstv_ref;

  /**
   * @brief 创建一个 msstv 实例
   */
  extern struct msstv *
msstv_create(const u64 nslots, const u64 version);

  /**
   * @brief 向 msstv 中追加一个 msst
   */
  extern void
msstv_append(struct msstv * const v, struct msst * const msst, const struct kv * const anchor);

  /**
   * @brief 为 msstv 设置读缓存
   */
  extern void
msstv_rcache(struct msstv * const v, struct rcache * const rc);

  /**
   * @brief 销毁 msstv 实例
   */
  extern void
msstv_destroy(struct msstv * const v);

  /**
   * @brief 从文件打开 msstv
   */
  extern struct msstv *
msstv_open(const char * const dirname, const char * const filename);

  /**
   * @brief 根据版本号打开 msstv
   */
  extern struct msstv *
msstv_open_version(const char * const dirname, const u64 version);

  /**
   * @brief 获取 msstv 的引用
   */
  extern struct msstv_ref *
msstv_ref(struct msstv * const v);

  /**
   * @brief 释放 msstv 的引用
   */
  extern struct msstv *
msstv_unref(struct msstv_ref * const ref);

  /**
   * @brief 从 msstv 中获取键值对
   */
  extern struct kv *
msstv_get(struct msstv_ref * const ref, const struct kref * const key, struct kv * const out);

  /**
   * @brief 检查键是否存在于 msstv 中
   */
  extern bool
msstv_probe(struct msstv_ref * const ref, const struct kref * const key);

// 对墓碑返回 NULL
  extern struct kv *
msstv_get_ts(struct msstv_ref * const ref, const struct kref * const key, struct kv * const out);

// 对墓碑返回 false
  extern bool
msstv_probe_ts(struct msstv_ref * const ref, const struct kref * const key);

  /**
   * @brief 获取值（ts模式），跳过墓碑
   */
  extern bool
msstv_get_value_ts(struct msstv_ref * const ref, const struct kref * const key,
    void * const vbuf_out, u32 * const vlen_out);

  /**
   * @brief 创建 msstv 迭代器
   */
  extern struct msstv_iter *
msstv_iter_create(struct msstv_ref * const ref);

  /**
   * @brief 检查 msstv 迭代器是否有效
   */
  extern bool
msstv_iter_valid(struct msstv_iter * const vi);

  /**
   * @brief 定位 msstv 迭代器
   */
  extern void
msstv_iter_seek(struct msstv_iter * const vi, const struct kref * const key);

  /**
   * @brief 查看 msstv 迭代器当前项
   */
  extern struct kv *
msstv_iter_peek(struct msstv_iter * const vi, struct kv * const out);

  /**
   * @brief 获取 msstv 迭代器当前键引用
   */
  extern bool
msstv_iter_kref(struct msstv_iter * const vi, struct kref * const kref);

  /**
   * @brief 获取 msstv 迭代器当前键值对引用
   */
  extern bool
msstv_iter_kvref(struct msstv_iter * const vi, struct kvref * const kvref);

  /**
   * @brief 保留 msstv 迭代器状态
   */
  extern u64
msstv_iter_retain(struct msstv_iter * const vi);

  /**
   * @brief 释放 msstv 迭代器保留的状态
   */
  extern void
msstv_iter_release(struct msstv_iter * const vi, const u64 opaque);

  /**
   * @brief msstv 迭代器前进一格
   */
  extern void
msstv_iter_skip1(struct msstv_iter * const vi);

  /**
   * @brief msstv 迭代器前进 n 格
   */
  extern void
msstv_iter_skip(struct msstv_iter * const vi, const u32 nr);

  /**
   * @brief 获取 msstv 迭代器下一项
   */
  extern struct kv *
msstv_iter_next(struct msstv_iter * const vi, struct kv * const out);

  /**
   * @brief 暂停 msstv 迭代器
   */
  extern void
msstv_iter_park(struct msstv_iter * const vi);

  /**
   * @brief 检查 msstv 迭代器（ts模式）是否指向墓碑
   */
  extern bool
msstv_iter_ts(struct msstv_iter * const vi);

  /**
   * @brief 定位 msstv 迭代器（ts模式）
   */
  extern void
msstv_iter_seek_ts(struct msstv_iter * const vi, const struct kref * const key);

  /**
   * @brief msstv 迭代器前进一格（ts模式）
   */
  extern void
msstv_iter_skip1_ts(struct msstv_iter * const vi);

  /**
   * @brief msstv 迭代器前进 n 格（ts模式）
   */
  extern void
msstv_iter_skip_ts(struct msstv_iter * const vi, const u32 nr);

  /**
   * @brief 获取 msstv 迭代器下一项（ts模式）
   */
  extern struct kv *
msstv_iter_next_ts(struct msstv_iter * const vi, struct kv * const out);

  /**
   * @brief 打印 msstv 信息
   */
  extern void
msstv_fprint(struct msstv * const v, FILE * const out);

  /**
   * @brief 销毁 msstv 迭代器
   */
  extern void
msstv_iter_destroy(struct msstv_iter * const vi);

// 不安全!
// 返回以 NULL 结尾的 msstv 的锚点
// 返回的指针使用后应被释放
// 必须在持有 msstv 时使用
// anchor->vlen: 0: 接受; 1: 拒绝
  extern struct kv **
msstv_anchors(struct msstv * const v);
// }}} msstv

// msstz {{{
// msstz: 顶层管理器
struct msstz;

  /**
   * @brief 打开一个 msstz 数据库实例
   */
  extern struct msstz *
msstz_open(const char * const dirname, const u64 cache_size_mb, const bool ckeys, const bool tags);

  /**
   * @brief 销毁 msstz 实例
   */
  extern void
msstz_destroy(struct msstz * const z);

  /**
   * @brief 获取日志文件描述符
   */
  extern int
msstz_logfd(struct msstz * const z);

// 返回自打开以来的写入字节数
  extern u64
msstz_stat_writes(struct msstz * const z);

// 返回自打开以来的读取字节数
  extern u64
msstz_stat_reads(struct msstz * const z);

// 默认为 0
  extern void
msstz_set_minsz(struct msstz * const z, const u64 minsz);

  /**
   * @brief 获取当前版本号
   */
  extern u64
msstz_version(struct msstz * const z);

  /**
   * @brief 获取当前的 msstv 视图
   */
  extern struct msstv *
msstz_getv(struct msstz * const z);

  /**
   * @brief 释放 msstv 视图
   */
  extern void
msstz_putv(struct msstz * const z, struct msstv * const v);

// 范围查询回调函数
typedef void (*msstz_range_cb)(void * priv, const bool accepted, const struct kv * k0, const struct kv * kz);

  /**
   * @brief 执行合并（compaction）
   */
  extern void
msstz_comp(struct msstz * const z, const struct kvmap_api * const api1, void * const map1,
    const u32 nr_workers, const u32 co_per_worker, const u64 max_reject);
// }}} msstz

// api {{{
// 导出的各种 kvmap API 实现
extern const struct kvmap_api kvmap_api_sst;
extern const struct kvmap_api kvmap_api_msstx;
extern const struct kvmap_api kvmap_api_mssty;
extern const struct kvmap_api kvmap_api_mssty_ts;
extern const struct kvmap_api kvmap_api_msstv;
extern const struct kvmap_api kvmap_api_msstv_ts;
// }}} api

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
