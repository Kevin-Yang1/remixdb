/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

/*
 * WormHole - 高性能并发有序键值映射数据结构
 *
 * 文件功能说明:
 * =============
 * 本文件定义了 WormHole 数据结构的完整 API 接口。WormHole 是一个高性能的并发
 * 有序键值映射实现，专门为高并发场景设计，具有以下特点：
 *
 * 核心特性:
 * ---------
 * • 并发安全: 支持多线程并发读写操作
 * • 有序存储: 键值对按键的字典序自动排序
 * • 高性能: 针对现代多核 CPU 优化的无锁/低锁设计
 * • 内存高效: 紧凑的内存布局和缓存友好的数据结构
 * • 范围查询: 支持高效的范围查询和迭代器
 *
 * API 分类:
 * ---------
 * 1. 基础操作: 创建、销毁、引用管理
 * 2. 数据操作: 插入、查询、删除、合并
 * 3. 迭代器: 范围扫描、游标操作
 * 4. 安全模式: 自动状态刷新的线程安全 API
 * 5. 非安全模式: 高性能但需要手动管理状态的 API
 *
 * 使用场景:
 * ---------
 * • LSM 树的内存表 (MemTable) 实现
 * • 高并发的索引结构
 * • 需要有序访问的缓存系统
 * • 实时数据分析和排序
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 前向声明
struct wormhole;     // WormHole 主数据结构
struct wormref;      // WormHole 引用句柄 (用于线程安全访问)

// wormhole {{{ // WormHole 主要 API 开始

// === 创建和销毁 === //

// 创建一个 WormHole 实例 (支持安全和非安全操作)
// 参数: mm - 内存管理器接口
// 返回: WormHole 实例指针，失败时返回 NULL
  extern struct wormhole *
wormhole_create(const struct kvmap_mm * const mm);

// 创建一个 WormHole 实例 (仅支持非安全操作，性能更高)
// 参数: mm - 内存管理器接口
// 返回: WormHole 实例指针，失败时返回 NULL
// 注意: 此实例只能与 whunsafe_* 系列函数配合使用
  extern struct wormhole *
whunsafe_create(const struct kvmap_mm * const mm);

// === 基本数据操作 === //

// 根据键查询值
// 参数: ref - WormHole 引用, key - 查询的键, out - 输出缓冲区
// 返回: 找到的键值对指针，未找到时返回 NULL
  extern struct kv *
wormhole_get(struct wormref * const ref, const struct kref * const key, struct kv * const out);

// 检查键是否存在 (不返回值，性能更高)
// 参数: ref - WormHole 引用, key - 查询的键
// 返回: 存在时返回 true，不存在时返回 false
  extern bool
wormhole_probe(struct wormref * const ref, const struct kref * const key);

// 插入或更新键值对
// 参数: ref - WormHole 引用, kv - 要插入的键值对
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_put(struct wormref * const ref, struct kv * const kv);

// 合并操作 (使用用户定义的合并函数)
// 参数: ref - WormHole 引用, kref - 键引用, uf - 合并函数, priv - 用户数据
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_merge(struct wormref * const ref, const struct kref * const kref,
    kv_merge_func uf, void * const priv);

// 就地处理 (读取模式) - 对现有键值对执行只读操作
// 参数: ref - WormHole 引用, key - 键, uf - 处理函数, priv - 用户数据
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_inpr(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv);

// 就地处理 (写入模式) - 对现有键值对执行修改操作
// 参数: ref - WormHole 引用, key - 键, uf - 处理函数, priv - 用户数据
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_inpw(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv);

// === 删除操作 === //

// 删除指定键的键值对
// 参数: ref - WormHole 引用, key - 要删除的键
// 返回: 成功删除时返回 true，键不存在时返回 false
  extern bool
wormhole_del(struct wormref * const ref, const struct kref * const key);

// 范围删除 - 删除指定范围内的所有键值对
// 参数: ref - WormHole 引用, start - 范围起始键, end - 范围结束键
// 返回: 删除的键值对数量
  extern u64
wormhole_delr(struct wormref * const ref, const struct kref * const start,
    const struct kref * const end);

// === 迭代器操作 === //

// 创建迭代器
// 参数: ref - WormHole 引用
// 返回: 迭代器指针，失败时返回 NULL
  extern struct wormhole_iter *
wormhole_iter_create(struct wormref * const ref);

// 将迭代器定位到指定键或其后继位置
// 参数: iter - 迭代器, key - 目标键
  extern void
wormhole_iter_seek(struct wormhole_iter * const iter, const struct kref * const key);

// 检查迭代器是否指向有效位置
// 参数: iter - 迭代器
// 返回: 有效时返回 true，无效时返回 false
  extern bool
wormhole_iter_valid(struct wormhole_iter * const iter);

// 查看当前位置的键值对 (不移动迭代器)
// 参数: iter - 迭代器, out - 输出缓冲区
// 返回: 当前键值对指针，无效位置时返回 NULL
  extern struct kv *
wormhole_iter_peek(struct wormhole_iter * const iter, struct kv * const out);

// 获取当前位置的键引用
// 参数: iter - 迭代器, kref - 输出的键引用
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_iter_kref(struct wormhole_iter * const iter, struct kref * const kref);

// 获取当前位置的键值引用
// 参数: iter - 迭代器, kvref - 输出的键值引用
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_iter_kvref(struct wormhole_iter * const iter, struct kvref * const kvref);

// 跳过一个元素
// 参数: iter - 迭代器
  extern void
wormhole_iter_skip1(struct wormhole_iter * const iter);

// 跳过指定数量的元素
// 参数: iter - 迭代器, nr - 要跳过的元素数量
  extern void
wormhole_iter_skip(struct wormhole_iter * const iter, const u32 nr);

// 移动到下一个位置并返回键值对
// 参数: iter - 迭代器, out - 输出缓冲区
// 返回: 下一个键值对指针，到达末尾时返回 NULL
  extern struct kv *
wormhole_iter_next(struct wormhole_iter * const iter, struct kv * const out);

// 对当前位置执行就地处理
// 参数: iter - 迭代器, uf - 处理函数, priv - 用户数据
// 返回: 成功时返回 true，失败时返回 false
  extern bool
wormhole_iter_inp(struct wormhole_iter * const iter, kv_inp_func uf, void * const priv);

// 暂停迭代器 (释放资源但保持位置)
// 参数: iter - 迭代器
  extern void
wormhole_iter_park(struct wormhole_iter * const iter);

// 销毁迭代器
// 参数: iter - 迭代器
  extern void
wormhole_iter_destroy(struct wormhole_iter * const iter);

// === 引用管理 === //

// 获取 WormHole 的引用句柄 (用于线程安全访问)
// 参数: map - WormHole 实例
// 返回: 引用句柄指针
  extern struct wormref *
wormhole_ref(struct wormhole * const map);

// 释放引用句柄并返回原始 WormHole 指针
// 参数: ref - 引用句柄
// 返回: 原始 WormHole 指针
  extern struct wormhole *
wormhole_unref(struct wormref * const ref);

// 暂停引用 (释放资源)
// 参数: ref - 引用句柄
  extern void
wormhole_park(struct wormref * const ref);

// 恢复引用 (重新获取资源)
// 参数: ref - 引用句柄
  extern void
wormhole_resume(struct wormref * const ref);

// 刷新查询状态 (在高并发环境中确保数据一致性)
// 参数: ref - 引用句柄
  extern void
wormhole_refresh_qstate(struct wormref * const ref);

// === 维护操作 === //

// 使用多线程进行清理和垃圾回收
// 参数: map - WormHole 实例, nr_threads - 线程数量
  extern void
wormhole_clean_th(struct wormhole * const map, const u32 nr_threads);

// 执行清理和垃圾回收 (单线程)
// 参数: map - WormHole 实例
  extern void
wormhole_clean(struct wormhole * const map);

// 销毁 WormHole 实例
// 参数: map - WormHole 实例
  extern void
wormhole_destroy(struct wormhole * const map);

// === 安全 API (自动状态刷新的线程安全版本) === //

// 安全版本的查询操作 (自动处理状态刷新)
// 参数和返回值与对应的普通版本相同
  extern struct kv *
whsafe_get(struct wormref * const ref, const struct kref * const key, struct kv * const out);

  extern bool
whsafe_probe(struct wormref * const ref, const struct kref * const key);

  extern bool
whsafe_put(struct wormref * const ref, struct kv * const kv);

  extern bool
whsafe_merge(struct wormref * const ref, const struct kref * const kref,
    kv_merge_func uf, void * const priv);

  extern bool
whsafe_inpr(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv);

  extern bool
whsafe_inpw(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv);

  extern bool
whsafe_del(struct wormref * const ref, const struct kref * const key);

  extern u64
whsafe_delr(struct wormref * const ref, const struct kref * const start,
    const struct kref * const end);

// 安全版本的迭代器操作
// 注意: 使用 wormhole_iter_create 创建迭代器
  extern void
whsafe_iter_seek(struct wormhole_iter * const iter, const struct kref * const key);

  extern struct kv *
whsafe_iter_peek(struct wormhole_iter * const iter, struct kv * const out);

// 以下函数与普通版本相同:
// - wormhole_iter_valid
// - wormhole_iter_peek
// - wormhole_iter_kref
// - wormhole_iter_kvref
// - wormhole_iter_skip1
// - wormhole_iter_skip
// - wormhole_iter_next
// - wormhole_iter_inp

  extern void
whsafe_iter_park(struct wormhole_iter * const iter);

  extern void
whsafe_iter_destroy(struct wormhole_iter * const iter);

  extern struct wormref *
whsafe_ref(struct wormhole * const map);

// 使用 wormhole_unref 释放引用

// === 非安全 API (高性能但需要手动管理状态) === //

// 非安全版本的操作 (直接操作 WormHole 实例，无引用管理)
// 注意: 这些函数性能更高，但不是线程安全的，适用于单线程或已同步的环境
  extern struct kv *
whunsafe_get(struct wormhole * const map, const struct kref * const key, struct kv * const out);

  extern bool
whunsafe_probe(struct wormhole * const map, const struct kref * const key);

  extern bool
whunsafe_put(struct wormhole * const map, struct kv * const kv);

  extern bool
whunsafe_merge(struct wormhole * const map, const struct kref * const kref,
    kv_merge_func uf, void * const priv);

  extern bool
whunsafe_inp(struct wormhole * const map, const struct kref * const key,
    kv_inp_func uf, void * const priv);

  extern bool
whunsafe_del(struct wormhole * const map, const struct kref * const key);

  extern u64
whunsafe_delr(struct wormhole * const map, const struct kref * const start,
    const struct kref * const end);

// 非安全版本的迭代器操作
  extern struct wormhole_iter *
whunsafe_iter_create(struct wormhole * const map);

  extern void
whunsafe_iter_seek(struct wormhole_iter * const iter, const struct kref * const key);

// 以下函数与普通版本相同:
// - wormhole_iter_valid
// - wormhole_iter_peek
// - wormhole_iter_kref

  extern void
whunsafe_iter_skip1(struct wormhole_iter * const iter);

  extern void
whunsafe_iter_skip(struct wormhole_iter * const iter, const u32 nr);

  extern struct kv *
whunsafe_iter_next(struct wormhole_iter * const iter, struct kv * const out);

// 使用 wormhole_iter_inp 进行就地处理

  extern void
whunsafe_iter_destroy(struct wormhole_iter * const iter);

// === 调试和工具函数 === //

// 将 WormHole 的内部状态打印到文件
// 参数: map - WormHole 实例, out - 输出文件
  extern void
wormhole_fprint(struct wormhole * const map, FILE * const out);

// === kvmap API 适配器 === //

// 标准 kvmap 接口的 WormHole 实现
extern const struct kvmap_api kvmap_api_wormhole;   // 普通版本
extern const struct kvmap_api kvmap_api_whsafe;     // 安全版本
extern const struct kvmap_api kvmap_api_whunsafe;   // 非安全版本

// }}} wormhole // WormHole API 结束

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
