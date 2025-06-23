/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE // 定义_GNU_SOURCE以启用GNU扩展

// headers {{{
#include <assert.h> // 包含断言宏
#include "lib.h"    // 包含库函数
#include "ctypes.h" // 包含自定义类型
#include "kv.h"     // 包含键值对相关定义
#include "wh.h"     // 包含Wormhole自身头文件
// }}} headers

// def {{{
#define WH_HMAPINIT_SIZE ((1u << 12)) // 哈希表初始大小。10: 16KB/64KB  12: 64KB/256KB  14: 256KB/1MB
#define WH_SLABMETA_SIZE ((1lu << 21)) // 元数据Slab分配器大小 (2MB)

#ifndef HEAPCHECKING // 如果没有定义HEAPCHECKING
#define WH_SLABLEAF_SIZE ((1lu << 21)) // 叶子节点Slab分配器大小 (2MB)
#else // 如果定义了HEAPCHECKING (用于内存调试)
#define WH_SLABLEAF_SIZE ((1lu << 21)) // 用于valgrind调试的叶子节点Slab大小 (2MB)
#endif

#define WH_KPN ((128u)) // 每个叶子节点中的键数量；必须是2的幂
#define WH_HDIV (((1u << 16)) / WH_KPN) // 哈希分区因子，用于快速定位hs数组中的大致位置
#define WH_MID ((WH_KPN >> 1)) // 节点分裂的理想切割点，越接近越好
#define WH_BKT_NR ((8)) // Cuckoo哈希表中每个桶的槽位数
#define WH_KPN2 ((WH_KPN + WH_KPN)) // 两倍的节点键容量

#define WH_KPN_MRG (((WH_KPN + WH_MID) >> 1 )) // 节点合并阈值 (3/4 * KPN)

// FO 固定为 256. 不要修改.
#define WH_FO  ((256u)) // 索引扇出 (fan-out)，代表一个元数据节点可以有多少个子节点
// 位图中的位数
#define WH_BMNR ((WH_FO >> 6)) // 位图所需的u64数量 (256 / 64 = 4)
// }}} def

// struct {{{
// Wormhole元数据节点，用于构建前缀树（Trie）
struct wormmeta {
  struct entry13 k13; // 存储键引用(kref)和键长(klen)
  struct entry13 l13; // 存储最左叶子(lmost)、最小位(bitmin)和最大位(bitmax)
  struct entry13 r13; // 存储最右叶子(rmost)和32位哈希低位(hash32_lo)
  struct entry13 p13; // 存储左路径叶子(lpath)和32位哈希高位(hash32_hi)
  u64 bitmap[0]; // 位图，当 bitmin != bitmax 时使用，大小为4个u64 (柔性数组成员)
};
static_assert(sizeof(struct wormmeta) == 32, "sizeof(wormmeta) != 32"); // 静态断言确保wormmeta大小为32字节

// 64位键的键值对结构 (whu64)
struct wormkv64 { u64 key; void * ptr; };

// Wormhole叶子节点，类似B+树的叶子
struct wormleaf {
  // 第一个缓存行
  rwlock leaflock;   // 叶子节点的读写锁
  spinlock sortlock; // 用于保护迭代器查找(iter_seek)时的排序操作
  au64 lv;           // 版本号 (不使用第一个u64)
  struct wormleaf * prev; // 指向前一个叶子节点
  struct wormleaf * next; // 指向后一个叶子节点
  struct kv * anchor;     // 分裂时产生的锚点键，代表该叶子节点中的最小键

  u32 nr_sorted; // 已排序的键数量
  u32 nr_keys;   // 总键数量
  u64 reserved[2]; // 预留空间

  struct entry13 hs[WH_KPN]; // 按哈希值排序的条目数组
  u8 ss[WH_KPN];             // 按键值排序的条目索引 (指向hs数组)
};

// Cuckoo哈希表的槽位，存储部分键(pkey)
struct wormslot { u16 t[WH_BKT_NR]; };
static_assert(sizeof(struct wormslot) == 16, "sizeof(wormslot) != 16"); // 静态断言确保wormslot大小为16字节

// Cuckoo哈希表的元数据桶，存储指向wormmeta的指针
struct wormmbkt { struct wormmeta * e[WH_BKT_NR]; };
static_assert(sizeof(struct wormmbkt) == 64, "sizeof(wormmbkt) != 64"); // 静态断言确保wormmbkt大小为64字节

// Wormhole哈希表 (MetaTrieHT)
struct wormhmap {
  au64 hv; // 哈希表版本
  struct wormslot * wmap; // 指向槽位图 (wmap)，存储pkey
  struct wormmbkt * pmap; // 指向元数据指针图 (pmap)，存储元数据指针
  u32 mask;     // 哈希掩码
  u32 maxplen;  // 索引中存储的最大前缀长度
  u64 msize;    // pmap和wmap的总大小

  struct slab * slab1; // 用于分配wormmeta (32字节)
  struct slab * slab2; // 用于分配带位图的wormmeta (32+32字节)
  struct kv * pbuf;    // 用于元数据合并的临时缓冲区
};
static_assert(sizeof(struct wormhmap) == 64, "sizeof(wormhmap) != 64"); // 静态断言确保wormhmap大小为64字节

// Wormhole 键值存储的顶层结构
struct wormhole {
  // 1 line  64字节
  union {
    volatile au64 hmap_ptr; // 安全的哈希表指针 (原子访问)
    struct wormhmap * hmap; // 不安全的哈希表指针 (非原子访问)
  };
  u64 padding0[6]; // 内存对齐填充
  struct wormleaf * leaf0; // 初始叶子节点，通常不用
  // 1 line
  struct kvmap_mm mm; // 内存管理回调函数
  struct qsbr * qsbr; // QSBR内存回收机制
  struct slab * slab_leaf; // 叶子节点的Slab分配器
  struct kv * pbuf;        // 元数据合并的临时缓冲区
  u32 leaftype; // 叶子类型 (未使用)
  u32 padding1; // 内存对齐填充
  // 2 lines
  struct wormhmap hmap2[2]; // 两个哈希表，用于无锁扩容
  // fifth line
  rwlock metalock; // 元数据操作的锁
  u32 padding2[15]; // 内存对齐填充
};

// Wormhole 迭代器
struct wormhole_iter {
  struct wormref * ref; // 仅安全迭代器使用，持有线程引用
  struct wormhole * map; // 指向Wormhole实例
  struct wormleaf * leaf; // 当前指向的叶子节点
  u32 is; // 在ss数组中的索引
};

// Wormhole 线程引用，用于安全并发访问
struct wormref {
  struct wormhole * map; // 指向Wormhole实例
  struct qsbr_ref qref; // QSBR线程引用
};
// }}} struct

// helpers {{{

// meta {{{
// wormmeta 结构体的访问和操作函数

  // 加载元数据中的键引用
  static inline struct kv *
wormmeta_keyref_load(const struct wormmeta * const meta)
{
  return u64_to_ptr(meta->k13.e3); // 从entry13中提取指针
}

  // 加载元数据中的键长
  static inline u16
wormmeta_klen_load(const struct wormmeta * const meta)
{
  return meta->k13.e1; // 从entry13中提取键长
}

  // 加载元数据中的最左叶子节点
  static inline struct wormleaf *
wormmeta_lmost_load(const struct wormmeta * const meta)
{
  return u64_to_ptr(meta->l13.e3 & (~0x3flu)); // 提取指针，并清除低位的标志位
}

  // 加载元数据中的最小位索引
  static inline u32
wormmeta_bitmin_load(const struct wormmeta * const meta)
{
  return (u32)(meta->l13.v64 & 0x1fflu); // 从l13的64位值中提取bitmin
}

  // 加载元数据中的最大位索引
  static inline u32
wormmeta_bitmax_load(const struct wormmeta * const meta)
{
  return (u32)((meta->l13.v64 >> 9) & 0x1fflu); // 从l13的64位值中提取bitmax
}

  // 加载元数据中的32位哈希值
  static inline u32
wormmeta_hash32_load(const struct wormmeta * const meta)
{
  return ((u32)meta->r13.e1) | (((u32)meta->p13.e1) << 16); // 组合两个16位部分成32位哈希
}

  // 加载元数据中的最右叶子节点
  static inline struct wormleaf *
wormmeta_rmost_load(const struct wormmeta * const meta)
{
  return u64_to_ptr(meta->r13.e3); // 从r13中提取指针
}

  // 加载元数据中的左路径叶子节点
  static inline struct wormleaf *
wormmeta_lpath_load(const struct wormmeta * const meta)
{
  return u64_to_ptr(meta->p13.e3); // 从p13中提取指针
}

// internal
  // 存储左路径叶子节点
  static inline void
wormmeta_lpath_store(struct wormmeta * const meta, struct wormleaf * const leaf)
{
  entry13_update_e3(&meta->p13, ptr_to_u64(leaf)); // 更新p13中的指针部分
}

// also updates leaf_klen_eq and
  // 存储最左叶子节点，并根据键长是否相等更新左路径
  static inline void
wormmeta_lmost_store(struct wormmeta * const meta, struct wormleaf * const leaf)
{
  const u64 minmax = meta->l13.v64 & 0x3fffflu; // 保留bitmin和bitmax部分
  meta->l13.v64 = (((u64)leaf) << 16) | minmax; // 更新指针部分

  const bool leaf_klen_eq = leaf->anchor->klen == wormmeta_klen_load(meta); // 检查叶子锚点键长是否与元数据键长相等
  wormmeta_lpath_store(meta, leaf_klen_eq ? leaf : leaf->prev); // 如果相等，左路径是当前叶子，否则是前一个叶子
}

  // 存储最小位索引
  static inline void
wormmeta_bitmin_store(struct wormmeta * const meta, const u32 bitmin)
{
  meta->l13.v64 = (meta->l13.v64 & (~0x1fflu)) | bitmin; // 更新l13中的bitmin部分
}

  // 存储最大位索引
  static inline void
wormmeta_bitmax_store(struct wormmeta * const meta, const u32 bitmax)
{
  meta->l13.v64 = (meta->l13.v64 & (~0x3fe00lu)) | (bitmax << 9); // 更新l13中的bitmax部分
}

  // 存储最右叶子节点
  static inline void
wormmeta_rmost_store(struct wormmeta * const meta, struct wormleaf * const leaf)
{
  entry13_update_e3(&meta->r13, ptr_to_u64(leaf)); // 更新r13中的指针部分
}

// for wormmeta_alloc
  // 初始化wormmeta节点
  static void
wormmeta_init(struct wormmeta * const meta, struct wormleaf * const lrmost,
    struct kv * const keyref, const u32 alen, const u32 bit)
{
  keyref->refcnt++; // 共享键引用，增加引用计数

  const u32 plen = keyref->klen; // 获取键长
  debug_assert(plen <= UINT16_MAX); // 确保键长不超过16位
  meta->k13 = entry13((u16)plen, ptr_to_u64(keyref)); // 设置k13
  meta->l13.v64 = (ptr_to_u64(lrmost) << 16) | (bit << 9) | bit; // 设置lmost, bitmin, bitmax

  const u32 hash32 = keyref->hashlo; // 获取32位哈希
  meta->r13 = entry13((u16)hash32, ptr_to_u64(lrmost)); // 设置r13

  const bool leaf_klen_eq = alen == plen; // 比较锚点键长和元数据键长
  meta->p13 = entry13((u16)(hash32 >> 16), ptr_to_u64(leaf_klen_eq ? lrmost : lrmost->prev)); // 设置p13
}
// }}} meta

// meta-bitmap {{{
// wormmeta 位图操作函数

  // 测试位图中指定id位是否被设置
  static inline bool
wormmeta_bm_test(const struct wormmeta * const meta, const u32 id)
{
  debug_assert(id < WH_FO); // 确保id在合法范围内
  const u32 bitmin = wormmeta_bitmin_load(meta); // 加载最小位
  const u32 bitmax = wormmeta_bitmax_load(meta); // 加载最大位
  if (bitmin == bitmax) { // 半节点 (只有一个分支)
    return bitmin == id; // 直接比较
  } else { // 全节点 (有多个分支)
    return (bool)((meta->bitmap[id >> 6u] >> (id & 0x3fu)) & 1lu); // 在位图中测试位
  }
}

// meta must be a full node
  // 在位图中设置指定id位 (meta必须是全节点)
  static void
wormmeta_bm_set(struct wormmeta * const meta, const u32 id)
{
  // 需要替换 meta
  u64 * const ptr = &(meta->bitmap[id >> 6u]); // 定位到对应的u64
  const u64 bit = 1lu << (id & 0x3fu); // 计算位的掩码
  if ((*ptr) & bit) // 如果位已经被设置
    return; // 直接返回

  (*ptr) |= bit; // 设置位

  // 更新最小位
  if (id < wormmeta_bitmin_load(meta))
    wormmeta_bitmin_store(meta, id);

  // 更新最大位
  const u32 oldmax = wormmeta_bitmax_load(meta);
  if (oldmax == WH_FO || id > oldmax)
    wormmeta_bitmax_store(meta, id);
}

// find the lowest bit > id0
// return WH_FO if not found
  // 查找大于id0的最低位
  static inline u32
wormmeta_bm_gt(const struct wormmeta * const meta, const u32 id0)
{
  u32 ix = id0 >> 6; // 计算u64数组的索引
  u64 bits = meta->bitmap[ix] & ~((1lu << (id0 & 0x3fu)) - 1lu); // 清除id0及以下的位
  if (bits) // 如果当前u64中有满足条件的位
    return (ix << 6) + (u32)__builtin_ctzl(bits); // 返回最低位的索引

  while (++ix < WH_BMNR) { // 遍历后续的u64
    bits = meta->bitmap[ix];
    if (bits) // 如果找到
      return (ix << 6) + (u32)__builtin_ctzl(bits); // 返回最低位的索引
  }

  return WH_FO; // 未找到
}

// find the highest bit that is lower than the id0
// return WH_FO if not found
  // 查找小于id0的最高位
  static inline u32
wormmeta_bm_lt(const struct wormmeta * const meta, const u32 id0)
{
  u32 ix = id0 >> 6; // 计算u64数组的索引
  u64 bits = meta->bitmap[ix] & ((1lu << (id0 & 0x3fu)) - 1lu); // 清除id0及以上的位
  if (bits) // 如果当前u64中有满足条件的位
    return (ix << 6) + 63u - (u32)__builtin_clzl(bits); // 返回最高位的索引

  while (ix--) { // 遍历之前的u64
    bits = meta->bitmap[ix];
    if (bits) // 如果找到
      return (ix << 6) + 63u - (u32)__builtin_clzl(bits); // 返回最高位的索引
  }

  return WH_FO; // 未找到
}

// meta must be a full node
  // 清除位图中指定id位 (meta必须是全节点)
  static inline void
wormmeta_bm_clear(struct wormmeta * const meta, const u32 id)
{
  debug_assert(wormmeta_bitmin_load(meta) < wormmeta_bitmax_load(meta)); // 确保是全节点
  meta->bitmap[id >> 6u] &= (~(1lu << (id & 0x3fu))); // 清除位

  // 更新最小位
  if (id == wormmeta_bitmin_load(meta))
    wormmeta_bitmin_store(meta, wormmeta_bm_gt(meta, id));

  // 更新最大位
  if (id == wormmeta_bitmax_load(meta))
    wormmeta_bitmax_store(meta, wormmeta_bm_lt(meta, id));
}
// }}} meta-bitmap

// key/prefix {{{
// 键/前缀相关辅助函数

  // 根据32位哈希值生成部分键(pkey)，用于Cuckoo哈希
  static inline u16
wormhole_pkey(const u32 hash32)
{
  const u16 pkey0 = ((u16)hash32) ^ ((u16)(hash32 >> 16)); // 高16位和低16位异或
  return pkey0 ? pkey0 : 1; // pkey不能为0，0表示空槽位
}

  // 字节序翻转，用于Cuckoo哈希的第二个哈希位置
  static inline u32
wormhole_bswap(const u32 hashlo)
{
  return __builtin_bswap32(hashlo); // 使用内建函数进行字节序翻转
}

  // 检查给定的键是否与元数据中的键匹配
  static inline bool
wormhole_key_meta_match(const struct kv * const key, const struct wormmeta * const meta)
{
  return (key->klen == wormmeta_klen_load(meta)) // 比较键长
    && (!memcmp(key->kv, wormmeta_keyref_load(meta)->kv, key->klen)); // 比较键内容
}

// called by get_kref_slot
  // 检查给定的键引用(kref)是否与元数据中的键匹配
  static inline bool
wormhole_kref_meta_match(const struct kref * const kref,
    const struct wormmeta * const meta)
{
  return (kref->len == wormmeta_klen_load(meta)) // 比较键长
    && (!memcmp(kref->ptr, wormmeta_keyref_load(meta)->kv, kref->len)); // 比较键内容
}

// called from meta_down ... get_kref1_slot
// will access rmost, prefetching is effective here
  // 检查键引用(kref)加上一个字符(cid)是否与元数据中的键匹配
  static inline bool
wormhole_kref1_meta_match(const struct kref * const kref,
    const struct wormmeta * const meta, const u8 cid)
{
  const u8 * const keybuf = wormmeta_keyref_load(meta)->kv; // 获取元数据中的键
  const u32 plen = kref->len; // 获取前缀长度
  return ((plen + 1) == wormmeta_klen_load(meta)) // 比较总长度
    && (!memcmp(kref->ptr, keybuf, plen)) // 比较前缀部分
    && (keybuf[plen] == cid); // 比较最后一个字符
}

// warning: be careful with buffer overflow
  // 设置前缀键的长度并更新哈希
  static inline void
wormhole_prefix(struct kv * const pfx, const u32 klen)
{
  pfx->klen = klen; // 设置键长
  kv_update_hash(pfx); // 更新哈希值
}

// for split
  // 前缀键长度加1，并增量更新哈希
  static inline void
wormhole_prefix_inc1(struct kv * const pfx)
{
  pfx->hashlo = crc32c_u8(pfx->hashlo, pfx->kv[pfx->klen]); // 增量计算CRC32C
  pfx->klen++; // 键长加1
}

// meta_lcp only
  // 增量更新键引用的哈希和长度
  static inline void
wormhole_kref_inc(struct kref * const kref, const u32 len0,
    const u32 crc, const u32 inc)
{
  kref->hash32 = crc32c_inc(kref->ptr + len0, inc, crc); // 增量计算哈希
  kref->len = len0 + inc; // 更新长度
}

// meta_lcp only
  // 增量更新键引用的哈希和长度 (1,2,3字节优化)
  static inline void
wormhole_kref_inc_123(struct kref * const kref, const u32 len0,
    const u32 crc, const u32 inc)
{
  kref->hash32 = crc32c_inc_123(kref->ptr + len0, inc, crc); // 使用1/2/3字节优化的增量哈希
  kref->len = len0 + inc; // 更新长度
}
// }}} key/prefix

// alloc {{{
// 内存分配相关函数

  // 分配一个锚点键(anchor key)
  static inline struct kv *
wormhole_alloc_akey(const size_t klen)
{
#ifdef ALLOCFAIL // 用于测试的分配失败宏
  if (alloc_fail())
    return NULL;
#endif
  return malloc(sizeof(struct kv) + klen); // 分配内存
}

  // 释放一个锚点键
  static inline void
wormhole_free_akey(struct kv * const akey)
{
  free(akey); // 释放内存
}

  // 分配一个元数据键(meta key)
  static inline struct kv *
wormhole_alloc_mkey(const size_t klen)
{
#ifdef ALLOCFAIL
  if (alloc_fail())
    return NULL;
#endif
  return malloc(sizeof(struct kv) + klen); // 分配内存
}

  // 释放一个元数据键
  static inline void
wormhole_free_mkey(struct kv * const mkey)
{
  free(mkey); // 释放内存
}

  // 分配一个叶子节点
  static struct wormleaf *
wormleaf_alloc(struct wormhole * const map, struct wormleaf * const prev,
    struct wormleaf * const next, struct kv * const anchor)
{
  struct wormleaf * const leaf = slab_alloc_safe(map->slab_leaf); // 从Slab分配器安全地分配
  if (leaf == NULL) // 分配失败
    return NULL;

  rwlock_init(&(leaf->leaflock)); // 初始化读写锁
  spinlock_init(&(leaf->sortlock)); // 初始化自旋锁

  // keep the old version; new version will be assigned by split functions
  //leaf->lv = 0; // 保留旧版本号，新版本号将在分裂函数中分配

  leaf->prev = prev; // 设置前驱指针
  leaf->next = next; // 设置后继指针
  leaf->anchor = anchor; // 设置锚点键

  leaf->nr_keys = 0; // 初始化键数量
  leaf->nr_sorted = 0; // 初始化已排序键数量

  // hs requires zero init.
  memset(leaf->hs, 0, sizeof(leaf->hs[0]) * WH_KPN); // hs数组需要零初始化
  return leaf;
}

  // 释放一个叶子节点
  static void
wormleaf_free(struct slab * const slab, struct wormleaf * const leaf)
{
  debug_assert(leaf->leaflock.opaque == 0); // 确保锁未被持有
  wormhole_free_akey(leaf->anchor); // 释放锚点键
  slab_free_safe(slab, leaf); // 安全地释放回Slab分配器
}

  // 分配一个元数据节点
  static struct wormmeta *
wormmeta_alloc(struct wormhmap * const hmap, struct wormleaf * const lrmost,
    struct kv * const keyref, const u32 alen, const u32 bit)
{
  debug_assert(alen <= UINT16_MAX); // 断言锚点键长
  debug_assert(lrmost && keyref); // 断言指针非空

  struct wormmeta * const meta = slab_alloc_unsafe(hmap->slab1); // 从Slab不安全地分配
  if (meta == NULL) // 分配失败
    return NULL;

  wormmeta_init(meta, lrmost, keyref, alen, bit); // 初始化元数据节点
  return meta;
}

  // 为Slab分配器预留空间
  static inline bool
wormhole_slab_reserve(struct wormhole * const map, const u32 nr)
{
#ifdef ALLOCFAIL
  if (alloc_fail())
    return false;
#endif
  for (u32 i = 0; i < 2; i++) { // 遍历两个哈希表
    if (!(map->hmap2[i].slab1 && map->hmap2[i].slab2)) // 如果Slab不存在则跳过
      continue;
    if (!slab_reserve_unsafe(map->hmap2[i].slab1, nr)) // 预留空间
      return false;
    if (!slab_reserve_unsafe(map->hmap2[i].slab2, nr)) // 预留空间
      return false;
  }
  return true;
}

  // 释放元数据中的键引用 (减少引用计数)
  static void
wormmeta_keyref_release(struct wormmeta * const meta)
{
  struct kv * const keyref = wormmeta_keyref_load(meta); // 加载键引用
  debug_assert(keyref->refcnt); // 确保引用计数大于0
  keyref->refcnt--; // 减少引用计数
  if (keyref->refcnt == 0) // 如果引用计数为0
    wormhole_free_mkey(keyref); // 释放元数据键
}

  // 释放一个元数据节点
  static void
wormmeta_free(struct wormhmap * const hmap, struct wormmeta * const meta)
{
  wormmeta_keyref_release(meta); // 释放键引用
  slab_free_unsafe(hmap->slab1, meta); // 释放回Slab
}
// }}} alloc

// lock {{{
// 锁相关函数

  // 获取叶子节点写锁 (带park/resume)
  static void
wormleaf_lock_write(struct wormleaf * const leaf, struct wormref * const ref)
{
  if (!rwlock_trylock_write(&(leaf->leaflock))) { // 尝试获取写锁
    wormhole_park(ref); // 如果失败，暂停当前线程的QSBR
    rwlock_lock_write(&(leaf->leaflock)); // 阻塞式获取写锁
    wormhole_resume(ref); // 恢复QSBR
  }
}

  // 获取叶子节点读锁 (带park/resume)
  static void
wormleaf_lock_read(struct wormleaf * const leaf, struct wormref * const ref)
{
  if (!rwlock_trylock_read(&(leaf->leaflock))) { // 尝试获取读锁
    wormhole_park(ref); // 如果失败，暂停当前线程的QSBR
    rwlock_lock_read(&(leaf->leaflock)); // 阻塞式获取读锁
    wormhole_resume(ref); // 恢复QSBR
  }
}

  // 释放叶子节点写锁
  static void
wormleaf_unlock_write(struct wormleaf * const leaf)
{
  rwlock_unlock_write(&(leaf->leaflock));
}

  // 释放叶子节点读锁
  static void
wormleaf_unlock_read(struct wormleaf * const leaf)
{
  rwlock_unlock_read(&(leaf->leaflock));
}

  // 获取元数据写锁 (带park/resume)
  static void
wormhmap_lock(struct wormhole * const map, struct wormref * const ref)
{
  if (!rwlock_trylock_write(&(map->metalock))) { // 尝试获取写锁
    wormhole_park(ref); // 如果失败，暂停QSBR
    rwlock_lock_write(&(map->metalock)); // 阻塞式获取写锁
    wormhole_resume(ref); // 恢复QSBR
  }
}

  // 释放元数据写锁
  static inline void
wormhmap_unlock(struct wormhole * const map)
{
  rwlock_unlock_write(&(map->metalock));
}
// }}} lock

// hmap-version {{{
// 哈希表版本管理

  // 切换到另一个哈希表
  static inline struct wormhmap *
wormhmap_switch(struct wormhole * const map, struct wormhmap * const hmap)
{
  return (hmap == map->hmap2) ? (hmap + 1) : (hmap - 1); // 返回hmap2数组中的另一个
}

  // 原子加载当前活动的哈希表
  static inline struct wormhmap *
wormhmap_load(struct wormhole * const map)
{
  return (struct wormhmap *)atomic_load_explicit(&(map->hmap_ptr), MO_ACQUIRE); // 使用ACQUIRE内存序原子加载
}

  // 原子存储当前活动的哈希表
  static inline void
wormhmap_store(struct wormhole * const map, struct wormhmap * const hmap)
{
  atomic_store_explicit(&(map->hmap_ptr), (u64)hmap, MO_RELEASE); // 使用RELEASE内存序原子存储
}

  // 加载哈希表版本号
  static inline u64
wormhmap_version_load(const struct wormhmap * const hmap)
{
  // no concurrent access
  return atomic_load_explicit(&(hmap->hv), MO_ACQUIRE); // 使用ACQUIRE内存序原子加载
}

  // 存储哈希表版本号
  static inline void
wormhmap_version_store(struct wormhmap * const hmap, const u64 v)
{
  atomic_store_explicit(&(hmap->hv), v, MO_RELEASE); // 使用RELEASE内存序原子存储
}

  // 加载叶子节点版本号
  static inline u64
wormleaf_version_load(struct wormleaf * const leaf)
{
  return atomic_load_explicit(&(leaf->lv), MO_CONSUME); // 使用CONSUME内存序原子加载
}

  // 存储叶子节点版本号
  static inline void
wormleaf_version_store(struct wormleaf * const leaf, const u64 v)
{
  atomic_store_explicit(&(leaf->lv), v, MO_RELEASE); // 使用RELEASE内存序原子存储
}
// }}} hmap-version

// co {{{
// 协程(Coroutine)相关预取和让出操作

  // 预取哈希表的pmap桶
  static inline void
wormhmap_prefetch_pmap(const struct wormhmap * const hmap, const u32 idx)
{
#if defined(CORR) // 如果定义了协程
  (void)hmap;
  (void)idx;
#else // 否则
  cpu_prefetch0(&(hmap->pmap[idx])); // 预取数据到缓存
#endif
}

  // 获取元数据指针并预取/让出
  static inline struct wormmeta *
wormhmap_get_meta(const struct wormhmap * const hmap, const u32 mid, const u32 i)
{
  struct wormmeta * const meta = hmap->pmap[mid].e[i]; // 获取元数据指针
#if defined(CORR)
  cpu_prefetch0(meta); // 预取元数据
  corr_yield(); // 协程让出
#endif
  return meta;
}

  // 预取叶子节点
  static inline void
wormleaf_prefetch(struct wormleaf * const leaf, const u32 hashlo)
{
  const u32 i = wormhole_pkey(hashlo) / WH_HDIV; // 计算大致索引
#if defined(CORR)
  cpu_prefetch0(leaf); // 预取叶子节点本身
  cpu_prefetch0(&(leaf->hs[i-4])); // 预取哈希数组
  cpu_prefetch0(&(leaf->hs[i+4])); // 预取哈希数组
  corr_yield(); // 协程让出
#else
  cpu_prefetch0(&(leaf->hs[i])); // 预取哈希数组
#endif
}

  // 键引用和kv的匹配 (带预取/让出)
  static inline bool
wormhole_kref_kv_match(const struct kref * const key, const struct kv * const curr)
{
#if defined(CORR)
  const u8 * const ptr = (typeof(ptr))curr;
  cpu_prefetch0(ptr); // 预取kv数据
  cpu_prefetch0(ptr + 64);
  if (key->len > 56) {
    cpu_prefetch0(ptr + 128);
    cpu_prefetch0(ptr + 192);
  }
  corr_yield(); // 协程让出
#endif
  return kref_kv_match(key, curr); // 实际比较
}

  // 更新QSBR状态并暂停/让出
  static inline void
wormhole_qsbr_update_pause(struct wormref * const ref, const u64 v)
{
  qsbr_update(&ref->qref, v); // 更新QSBR状态
#if defined(CORR)
  corr_yield(); // 协程让出
#endif
}
// }}} co

// }}} helpers

// hmap {{{
// hmap 是 Wormhole 的 MetaTrieHT (元数据前缀树哈希表)
  // 初始化哈希表
  static bool
wormhmap_init(struct wormhmap * const hmap, struct kv * const pbuf)
{
  const u64 wsize = sizeof(hmap->wmap[0]) * WH_HMAPINIT_SIZE; // 计算wmap大小
  const u64 psize = sizeof(hmap->pmap[0]) * WH_HMAPINIT_SIZE; // 计算pmap大小
  u64 msize = wsize + psize; // 总大小
  u8 * const mem = pages_alloc_best(msize, true, &msize); // 分配大页内存
  if (mem == NULL) // 分配失败
    return false;

  hmap->pmap = (typeof(hmap->pmap))mem; // 设置pmap指针
  hmap->wmap = (typeof(hmap->wmap))(mem + psize); // 设置wmap指针
  hmap->msize = msize; // 记录总大小
  hmap->mask = WH_HMAPINIT_SIZE - 1; // 设置哈希掩码
  wormhmap_version_store(hmap, 0); // 初始化版本号
  hmap->maxplen = 0; // 初始化最大前缀长度
  hmap->pbuf = pbuf; // 设置临时缓冲区
  return true;
}

  // 销毁哈希表
  static inline void
wormhmap_deinit(struct wormhmap * const hmap)
{
  if (hmap->pmap) { // 如果已分配
    pages_unmap(hmap->pmap, hmap->msize); // 解除内存映射
    hmap->pmap = NULL;
    hmap->wmap = NULL;
  }
}

  // 返回一个全零的128位向量
  static inline m128
wormhmap_zero(void)
{
#if defined(__x86_64__)
  return _mm_setzero_si128(); // x86 SIMD指令
#elif defined(__aarch64__)
  return vdupq_n_u8(0); // ARM NEON指令
#endif
}

  // 将16位的pkey广播到一个128位向量
  static inline m128
wormhmap_m128_pkey(const u16 pkey)
{
#if defined(__x86_64__)
  return _mm_set1_epi16((short)pkey); // x86 SIMD指令
#elif defined(__aarch64__)
  return vreinterpretq_u8_u16(vdupq_n_u16(pkey)); // ARM NEON指令
#endif
}

  // 使用SIMD指令匹配pkey，返回一个掩码
  static inline u32
wormhmap_match_mask(const struct wormslot * const s, const m128 skey)
{
#if defined(__x86_64__)
  const m128 sv = _mm_load_si128((const void *)s); // 加载槽位数据
  return (u32)_mm_movemask_epi8(_mm_cmpeq_epi16(skey, sv)); // 比较并生成掩码
#elif defined(__aarch64__)
  const uint16x8_t sv = vld1q_u16((const u16 *)s); // 加载16字节数据
  const uint16x8_t cmp = vceqq_u16(vreinterpretq_u16_u8(skey), sv); // 比较，结果为0xffff或0x0000
  static const uint16x8_t mbits = {0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00, 0x3000, 0xc000}; // 用于提取掩码的位
  return (u32)vaddvq_u16(vandq_u16(cmp, mbits)); // 与、求和得到掩码
#endif
}

  // 使用SIMD指令检查是否有任何匹配的pkey
  static inline bool
wormhmap_match_any(const struct wormslot * const s, const m128 skey)
{
#if defined(__x86_64__)
  return wormhmap_match_mask(s, skey) != 0; // 检查掩码是否非零
#elif defined(__aarch64__)
  const uint16x8_t sv = vld1q_u16((const u16 *)s); // 加载数据
  const uint16x8_t cmp = vceqq_u16(vreinterpretq_u16_u8(skey), sv); // 比较
  return vaddvq_u32(vreinterpretq_u32_u16(cmp)) != 0; // 求和检查是否有匹配
#endif
}

// meta_lcp only
  // 快速窥探哈希表中是否存在某个哈希值，用于LCP搜索优化
  static inline bool
wormhmap_peek(const struct wormhmap * const hmap, const u32 hash32)
{
  const m128 sk = wormhmap_m128_pkey(wormhole_pkey(hash32)); // 生成pkey向量
  const u32 midx = hash32 & hmap->mask; // 计算主哈希位置
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 计算备用哈希位置
  return wormhmap_match_any(&(hmap->wmap[midx]), sk) // 检查主位置
    || wormhmap_match_any(&(hmap->wmap[midy]), sk); // 检查备用位置
}

  // 在哈希表的指定桶(mid)中查找匹配的元数据
  static inline struct wormmeta *
wormhmap_get_slot(const struct wormhmap * const hmap, const u32 mid,
    const m128 skey, const struct kv * const key)
{
  u32 mask = wormhmap_match_mask(&(hmap->wmap[mid]), skey); // 获取匹配的pkey掩码
  while (mask) { // 遍历所有匹配的槽位
    const u32 i2 = (u32)__builtin_ctz(mask); // 找到最低的匹配位
    struct wormmeta * const meta = wormhmap_get_meta(hmap, mid, i2>>1); // 获取元数据指针
    if (likely(wormhole_key_meta_match(key, meta))) // 完整比较键
      return meta; // 找到，返回
    mask ^= (3u << i2); // 清除已检查的位 (一个pkey占2字节，所以掩码是2位)
  }
  return NULL; // 未找到
}

  // 在哈希表中查找元数据
  static struct wormmeta *
wormhmap_get(const struct wormhmap * const hmap, const struct kv * const key)
{
  const u32 hash32 = key->hashlo; // 获取哈希
  const u32 midx = hash32 & hmap->mask; // 计算主位置
  wormhmap_prefetch_pmap(hmap, midx); // 预取主位置的pmap
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 计算备用位置
  wormhmap_prefetch_pmap(hmap, midy); // 预取备用位置的pmap
  const m128 skey = wormhmap_m128_pkey(wormhole_pkey(hash32)); // 生成pkey向量

  struct wormmeta * const r = wormhmap_get_slot(hmap, midx, skey, key); // 查找主位置
  if (r) // 找到
    return r;
  return wormhmap_get_slot(hmap, midy, skey, key); // 查找备用位置
}

// for meta_lcp only
  // 在哈希表的指定桶(mid)中通过键引用(kref)查找匹配的元数据
  static inline struct wormmeta *
wormhmap_get_kref_slot(const struct wormhmap * const hmap, const u32 mid,
    const m128 skey, const struct kref * const kref)
{
  u32 mask = wormhmap_match_mask(&(hmap->wmap[mid]), skey); // 获取匹配掩码
  while (mask) { // 遍历匹配
    const u32 i2 = (u32)__builtin_ctz(mask); // 找到最低位
    struct wormmeta * const meta = wormhmap_get_meta(hmap, mid, i2>>1); // 获取元数据
    if (likely(wormhole_kref_meta_match(kref, meta))) // 完整比较键引用
      return meta; // 找到

    mask ^= (3u << i2); // 清除已检查位
  }
  return NULL; // 未找到
}

// for meta_lcp only
  // 在哈希表中通过键引用(kref)查找元数据
  static inline struct wormmeta *
wormhmap_get_kref(const struct wormhmap * const hmap, const struct kref * const kref)
{
  const u32 hash32 = kref->hash32; // 获取哈希
  const u32 midx = hash32 & hmap->mask; // 主位置
  wormhmap_prefetch_pmap(hmap, midx); // 预取
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
  wormhmap_prefetch_pmap(hmap, midy); // 预取
  const m128 skey = wormhmap_m128_pkey(wormhole_pkey(hash32)); // pkey向量

  struct wormmeta * const r = wormhmap_get_kref_slot(hmap, midx, skey, kref); // 查找主位置
  if (r) // 找到
    return r;
  return wormhmap_get_kref_slot(hmap, midy, skey, kref); // 查找备用位置
}

// for meta_down only
  // 在哈希表的指定桶(mid)中通过键引用(kref)和下一个字符(cid)查找匹配的元数据
  static inline struct wormmeta *
wormhmap_get_kref1_slot(const struct wormhmap * const hmap, const u32 mid,
    const m128 skey, const struct kref * const kref, const u8 cid)
{
  u32 mask = wormhmap_match_mask(&(hmap->wmap[mid]), skey); // 获取匹配掩码
  while (mask) { // 遍历
    const u32 i2 = (u32)__builtin_ctz(mask); // 最低位
    struct wormmeta * const meta = wormhmap_get_meta(hmap, mid, i2>>1); // 获取元数据
    //cpu_prefetch0(wormmeta_rmost_load(meta)); // will access
    if (likely(wormhole_kref1_meta_match(kref, meta, cid))) // 完整比较 (kref + cid)
      return meta; // 找到

    mask ^= (3u << i2); // 清除
  }
  return NULL; // 未找到
}

// for meta_down only
  // 在哈希表中通过键引用(kref)和下一个字符(cid)查找元数据
  static inline struct wormmeta *
wormhmap_get_kref1(const struct wormhmap * const hmap,
    const struct kref * const kref, const u8 cid)
{
  const u32 hash32 = crc32c_u8(kref->hash32, cid); // 增量计算新哈希
  const u32 midx = hash32 & hmap->mask; // 主位置
  wormhmap_prefetch_pmap(hmap, midx); // 预取
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
  wormhmap_prefetch_pmap(hmap, midy); // 预取
  const m128 skey = wormhmap_m128_pkey(wormhole_pkey(hash32)); // pkey向量

  struct wormmeta * const r = wormhmap_get_kref1_slot(hmap, midx, skey, kref, cid); // 查找主位置
  if (r) // 找到
    return r;
  return wormhmap_get_kref1_slot(hmap, midy, skey, kref, cid); // 查找备用位置
}

  // 计算一个桶中已使用的槽位数
  static inline u32
wormhmap_slot_count(const struct wormslot * const slot)
{
  const u32 mask = wormhmap_match_mask(slot, wormhmap_zero()); // 查找pkey为0的槽位
  return mask ? ((u32)__builtin_ctz(mask) >> 1) : 8; // 返回第一个空槽位的索引，如果没有空槽位则返回8
}

  // 整理哈希表，尝试将不在主位置的条目移回主位置
  static inline void
wormhmap_squeeze(const struct wormhmap * const hmap)
{
  struct wormslot * const wmap = hmap->wmap; // wmap指针
  struct wormmbkt * const pmap = hmap->pmap; // pmap指针
  const u32 mask = hmap->mask; // 哈希掩码
  const u64 nrs64 = ((u64)(hmap->mask)) + 1; // 必须用u64防止溢出
  for (u64 si64 = 0; si64 < nrs64; si64++) { // 遍历所有桶
    const u32 si = (u32)si64; // 当前桶索引
    u32 ci = wormhmap_slot_count(&(wmap[si])); // 当前桶已用槽位数
    for (u32 ei = ci - 1; ei < WH_BKT_NR; ei--) { // 从后往前遍历已用槽位
      struct wormmeta * const meta = pmap[si].e[ei]; // 获取元数据
      const u32 sj = wormmeta_hash32_load(meta) & mask; // 计算主哈希位置
      if (sj == si) // 如果已经在主位置
        continue; // 跳过

      // 尝试移动
      const u32 ej = wormhmap_slot_count(&(wmap[sj])); // 获取主位置桶的已用槽位数
      if (ej < WH_BKT_NR) { // 如果主位置有空间
        wmap[sj].t[ej] = wmap[si].t[ei]; // 移动pkey
        pmap[sj].e[ej] = pmap[si].e[ei]; // 移动元数据指针
        const u32 ni = ci - 1; // 当前桶的新大小
        if (ei < ni) { // 如果被移动的不是最后一个元素
          wmap[si].t[ei] = wmap[si].t[ni]; // 用最后一个元素填补空位
          pmap[si].e[ei] = pmap[si].e[ni];
        }
        wmap[si].t[ni] = 0; // 清空最后一个槽位
        pmap[si].e[ni] = NULL;
        ci--; // 槽位数减一
      }
    }
  }
}

  // 扩展哈希表 (大小加倍)
  static void
wormhmap_expand(struct wormhmap * const hmap)
{
  // 同步扩展
  const u32 mask0 = hmap->mask; // 旧掩码
  if (mask0 == UINT32_MAX) // 检查是否已达最大
    debug_die();
  const u32 nr0 = mask0 + 1; // 旧大小
  const u32 mask1 = mask0 + nr0; // 新掩码
  const u64 nr1 = ((u64)nr0) << 1; // 新大小，必须用u64防止溢出
  const u64 wsize = nr1 * sizeof(hmap->wmap[0]); // 新wmap大小
  const u64 psize = nr1 * sizeof(hmap->pmap[0]); // 新pmap大小
  u64 msize = wsize + psize; // 新总大小
  u8 * mem = pages_alloc_best(msize, true, &msize); // 分配新内存
  if (mem == NULL) { // 分配失败
    // 我们在 wormhole_put() 的一个很深的调用栈中。
    // 优雅地处理失败需要大量改动。
    // 目前我们只是等待可用内存。
    // TODO: 优雅地返回插入失败
    char ts[64];
    time_stamp(ts, 64);
    fprintf(stderr, "%s %s sleep-wait for memory allocation %lukB\n",
        __func__, ts, msize >> 10);
    do {
      sleep(1); // 等待1秒
      mem = pages_alloc_best(msize, true, &msize); // 重试分配
    } while (mem == NULL);
    time_stamp(ts, 64);
    fprintf(stderr, "%s %s memory allocation done\n", __func__, ts);
  }

  struct wormhmap hmap1 = *hmap; // 复制旧hmap信息
  hmap1.pmap = (typeof(hmap1.pmap))mem; // 设置新pmap指针
  hmap1.wmap = (typeof(hmap1.wmap))(mem + psize); // 设置新wmap指针
  hmap1.msize = msize; // 更新大小
  hmap1.mask = mask1; // 更新掩码

  const struct wormslot * const wmap0 = hmap->wmap; // 旧wmap
  const struct wormmbkt * const pmap0 = hmap->pmap; // 旧pmap

  // 将旧表中的所有条目重新哈希到新表中
  for (u32 s = 0; s < nr0; s++) {
    const struct wormmbkt * const bkt = &pmap0[s];
    for (u32 i = 0; (i < WH_BKT_NR) && bkt->e[i]; i++) {
      const struct wormmeta * const meta = bkt->e[i];
      const u32 hash32 = wormmeta_hash32_load(meta);
      const u32 idx0 = hash32 & mask0;
      // 重新计算在新表中的位置，只有主位置会变
      const u32 idx1 = ((idx0 == s) ? hash32 : wormhole_bswap(hash32)) & mask1;

      const u32 n = wormhmap_slot_count(&(hmap1.wmap[idx1])); // 找到新桶的空位
      debug_assert(n < 8);
      hmap1.wmap[idx1].t[n] = wmap0[s].t[i]; // 复制pkey
      hmap1.pmap[idx1].e[n] = bkt->e[i]; // 复制元数据指针
    }
  }
  pages_unmap(hmap->pmap, hmap->msize); // 释放旧内存
  hmap->pmap = hmap1.pmap; // 更新指针
  hmap->wmap = hmap1.wmap;
  hmap->msize = hmap1.msize;
  hmap->mask = hmap1.mask;
  wormhmap_squeeze(hmap); // 整理新表
}

  // Cuckoo哈希插入逻辑
  static bool
wormhmap_cuckoo(struct wormhmap * const hmap, const u32 mid0,
    struct wormmeta * const e0, const u16 s0, const u32 depth)
{
  const u32 ii = wormhmap_slot_count(&(hmap->wmap[mid0])); // 获取当前桶的已用槽位数
  if (ii < WH_BKT_NR) { // 当前桶有空位，直接插入
    hmap->wmap[mid0].t[ii] = s0;
    hmap->pmap[mid0].e[ii] = e0;
    return true;
  } else if (depth == 0) { // 达到最大递归深度，插入失败
    return false;
  }

  // depth > 0, 当前桶已满，需要踢出一个条目
  struct wormmbkt * const bkt = &(hmap->pmap[mid0]);
  u16 * const sv = &(hmap->wmap[mid0].t[0]);
  for (u32 i = 0; i < WH_BKT_NR; i++) { // 遍历桶中所有条目，尝试踢出一个
    const struct wormmeta * const meta = bkt->e[i];
    debug_assert(meta);
    const u32 hash32 = wormmeta_hash32_load(meta);

    const u32 midx = hash32 & hmap->mask; // 主位置
    const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
    const u32 midt = (midx != mid0) ? midx : midy; // 找到它的备用位置
    if (midt != mid0) { // 如果备用位置不是当前桶
      // 如果移动到主位置，不减少递归深度
      const u32 depth1 = (midt == midx) ? depth : (depth - 1);
      if (wormhmap_cuckoo(hmap, midt, bkt->e[i], sv[i], depth1)) { // 递归插入被踢出的条目
        bkt->e[i] = e0; // 成功后，用新条目替换被踢出的条目
        sv[i] = s0;
        return true;
      }
    }
  }
  return false; // 无法踢出任何条目
}

  // 向哈希表设置一个元数据条目
  static void
wormhmap_set(struct wormhmap * const hmap, struct wormmeta * const meta)
{
  const u32 hash32 = wormmeta_hash32_load(meta); // 获取哈希
  const u32 midx = hash32 & hmap->mask; // 主位置
  wormhmap_prefetch_pmap(hmap, midx); // 预取
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
  wormhmap_prefetch_pmap(hmap, midy); // 预取
  const u16 pkey = wormhole_pkey(hash32); // pkey
  // 使用Cuckoo哈希插入
  if (likely(wormhmap_cuckoo(hmap, midx, meta, pkey, 1))) // 尝试插入主位置
    return;
  if (wormhmap_cuckoo(hmap, midy, meta, pkey, 1)) // 尝试插入备用位置
    return;
  if (wormhmap_cuckoo(hmap, midx, meta, pkey, 2)) // 再次尝试踢出
    return;

  // Cuckoo插入失败，扩展哈希表
  wormhmap_expand(hmap);

  wormhmap_set(hmap, meta); // 扩展后重新插入
}

  // 从指定桶中删除一个元数据条目
  static bool
wormhmap_del_slot(struct wormhmap * const hmap, const u32 mid,
    const struct wormmeta * const meta, const m128 skey)
{
  u32 mask = wormhmap_match_mask(&(hmap->wmap[mid]), skey); // 获取匹配掩码
  while (mask) { // 遍历
    const u32 i2 = (u32)__builtin_ctz(mask); // 最低位
    const struct wormmeta * const meta1 = hmap->pmap[mid].e[i2>>1]; // 获取元数据
    if (likely(meta == meta1)) { // 比较指针，确认是同一个条目
      const u32 i = i2 >> 1; // 槽位索引
      const u32 j = wormhmap_slot_count(&(hmap->wmap[mid])) - 1; // 最后一个条目的索引
      // 将最后一个条目移动到当前位置来填补空缺
      hmap->wmap[mid].t[i] = hmap->wmap[mid].t[j];
      hmap->pmap[mid].e[i] = hmap->pmap[mid].e[j];
      hmap->wmap[mid].t[j] = 0; // 清空最后一个槽位
      hmap->pmap[mid].e[j] = NULL;
      return true; // 删除成功
    }
    mask -= (3u << i2); // 清除已检查位
  }
  return false; // 未找到
}

  // 从哈希表中删除一个元数据条目
  static bool
wormhmap_del(struct wormhmap * const hmap, const struct wormmeta * const meta)
{
  const u32 hash32 = wormmeta_hash32_load(meta); // 获取哈希
  const u32 midx = hash32 & hmap->mask; // 主位置
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
  const m128 skey = wormhmap_m128_pkey(wormhole_pkey(hash32)); // pkey向量
  return wormhmap_del_slot(hmap, midx, meta, skey) // 尝试从主位置删除
    || wormhmap_del_slot(hmap, midy, meta, skey); // 尝试从备用位置删除
}

  // 在指定桶中替换一个元数据条目
  static bool
wormhmap_replace_slot(struct wormhmap * const hmap, const u32 mid,
    const struct wormmeta * const old, const m128 skey, struct wormmeta * const new)
{
  u32 mask = wormhmap_match_mask(&(hmap->wmap[mid]), skey); // 获取匹配掩码
  while (mask) { // 遍历
    const u32 i2 = (u32)__builtin_ctz(mask); // 最低位
    struct wormmeta ** const pslot = &hmap->pmap[mid].e[i2>>1]; // 获取元数据指针的指针
    if (likely(old == *pslot)) { // 比较指针
      *pslot = new; // 替换
      return true; // 成功
    }
    mask -= (3u << i2); // 清除
  }
  return false; // 未找到
}

  // 在哈希表中替换一个元数据条目
  static bool
wormhmap_replace(struct wormhmap * const hmap, const struct wormmeta * const old, struct wormmeta * const new)
{
  const u32 hash32 = wormmeta_hash32_load(old); // 获取哈希
  const u32 midx = hash32 & hmap->mask; // 主位置
  const u32 midy = wormhole_bswap(hash32) & hmap->mask; // 备用位置
  const m128 skey = wormhmap_m128_pkey(wormhole_pkey(hash32)); // pkey向量
  return wormhmap_replace_slot(hmap, midx, old, skey, new) // 尝试在主位置替换
    || wormhmap_replace_slot(hmap, midy, old, skey, new); // 尝试在备用位置替换
}
// }}} hmap

// create {{{
// 创建Wormhole实例
// it's unsafe
  // 创建初始的叶子节点 (leaf0)，用于空键
  static bool
wormhole_create_leaf0(struct wormhole * const map)
{
  const bool sr = wormhole_slab_reserve(map, 1); // 预留Slab空间
  if (unlikely(!sr))
    return false;

  // 创建空键的锚点
  struct kv * const anchor = wormhole_alloc_akey(0);
  if (anchor == NULL)
    return false;
  kv_dup2(kv_null(), anchor); // 复制空键

  // 分配初始叶子节点
  struct wormleaf * const leaf0 = wormleaf_alloc(map, NULL, NULL, anchor);
  if (leaf0 == NULL) {
    wormhole_free_akey(anchor);
    return false;
  }

  // 分配空键的元数据键
  struct kv * const mkey = wormhole_alloc_mkey(0);
  if (mkey == NULL) {
    wormleaf_free(map->slab_leaf, leaf0);
    return false;
  }

  wormhole_prefix(mkey, 0); // 设置前缀
  mkey->refcnt = 0; // 初始化引用计数
  // 创建空键的元数据
  for (u32 i = 0; i < 2; i++) { // 为两个哈希表都创建
    if (map->hmap2[i].slab1) {
      struct wormmeta * const m0 = wormmeta_alloc(&map->hmap2[i], leaf0, mkey, 0, WH_FO);
      debug_assert(m0); // 已经预留了足够空间
      wormhmap_set(&(map->hmap2[i]), m0); // 插入到哈希表
    }
  }

  map->leaf0 = leaf0; // 设置初始叶子
  return true;
}

  // 内部创建函数
  static struct wormhole *
wormhole_create_internal(const struct kvmap_mm * const mm, const u32 nh)
{
  struct wormhole * const map = yalloc(sizeof(*map)); // 分配Wormhole结构体
  if (map == NULL)
    return NULL;
  memset(map, 0, sizeof(*map)); // 清零
  // mm
  map->mm = mm ? (*mm) : kvmap_mm_dup; // 设置内存管理回调

  // pbuf for meta-merge
  map->pbuf = yalloc(1lu << 16); // 分配64kB的临时缓冲区
  if (map->pbuf == NULL)
    goto fail;

  // hmap
  for (u32 i = 0; i < nh; i++) { // 初始化nh个哈希表
    struct wormhmap * const hmap = &map->hmap2[i];
    if (!wormhmap_init(hmap, map->pbuf)) // 初始化哈希表
      goto fail;

    hmap->slab1 = slab_create(sizeof(struct wormmeta), WH_SLABMETA_SIZE); // 创建元数据Slab
    if (hmap->slab1 == NULL)
      goto fail;

    hmap->slab2 = slab_create(sizeof(struct wormmeta) + (sizeof(u64) * WH_BMNR), WH_SLABMETA_SIZE); // 创建带位图的元数据Slab
    if (hmap->slab2 == NULL)
      goto fail;
  }

  // leaf slab
  map->slab_leaf = slab_create(sizeof(struct wormleaf), WH_SLABLEAF_SIZE); // 创建叶子节点Slab
  if (map->slab_leaf == NULL)
    goto fail;

  // qsbr
  map->qsbr = qsbr_create(); // 创建QSBR实例
  if (map->qsbr == NULL)
    goto fail;

  // leaf0
  if (!wormhole_create_leaf0(map)) // 创建初始叶子
    goto fail;

  rwlock_init(&(map->metalock)); // 初始化元数据锁
  wormhmap_store(map, &map->hmap2[0]); // 设置当前活动哈希表
  return map; // 成功

fail: // 失败处理，释放已分配资源
  if (map->qsbr)
    qsbr_destroy(map->qsbr);

  if (map->slab_leaf)
    slab_destroy(map->slab_leaf);

  for (u32 i = 0; i < nh; i++) {
    struct wormhmap * const hmap = &map->hmap2[i];
    if (hmap->slab1)
      slab_destroy(hmap->slab1);
    if (hmap->slab2)
      slab_destroy(hmap->slab2);
    wormhmap_deinit(hmap);
  }

  if (map->pbuf)
    free(map->pbuf);

  free(map);
  return NULL;
}

  // 创建一个线程安全的Wormhole实例
  struct wormhole *
wormhole_create(const struct kvmap_mm * const mm)
{
  return wormhole_create_internal(mm, 2); // 创建两个哈希表
}

  // 创建一个非线程安全的Wormhole实例
  struct wormhole *
whunsafe_create(const struct kvmap_mm * const mm)
{
  return wormhole_create_internal(mm, 1); // 创建一个哈希表
}
// }}} create

// jump {{{
// 在索引结构中跳转以定位叶子节点

// lcp {{{
// search in the hash table for the Longest Prefix Match of the search key
// The corresponding wormmeta node is returned and the LPM is recorded in kref
// 在哈希表中搜索给定键的最长前缀匹配(LPM)
// 返回对应的wormmeta节点，并将LPM记录在kref中
  static struct wormmeta *
wormhole_meta_lcp(const struct wormhmap * const hmap, struct kref * const kref, const u32 klen)
{
  // 不变量: lo <= lcp < (lo + gd)
  // 结束条件: gd == 1
  u32 gd = (hmap->maxplen < klen ? hmap->maxplen : klen) + 1u; // 搜索范围
  u32 lo = 0; // 当前匹配长度
  u32 loh = KV_CRC32C_SEED; // 当前哈希

#define META_LCP_GAP_1 ((7u))
  // 第一阶段：大步长搜索 (步长为 gd/8 * 4)，使用peek快速检查
  while (META_LCP_GAP_1 < gd) {
    const u32 inc = gd >> 3 << 2; // x4
    const u32 hash32 = crc32c_inc_x4(kref->ptr + lo, inc, loh); // 增量计算哈希
    if (wormhmap_peek(hmap, hash32)) { // 使用peek快速检查是否存在
      loh = hash32; // 更新哈希
      lo += inc; // 更新长度
      gd -= inc; // 缩小范围
    } else {
      gd = inc; // 缩小范围
    }
  }

  // 第二阶段：小步长二分搜索
  while (1 < gd) {
    const u32 inc = gd >> 1; // 步长减半
    const u32 hash32 = crc32c_inc_123(kref->ptr + lo, inc, loh); // 增量计算哈希
    if (wormhmap_peek(hmap, hash32)) { // peek检查
      loh = hash32;
      lo += inc;
      gd -= inc;
    } else {
      gd = inc;
    }
  }
#undef META_LCP_GAP_1

  kref->hash32 = loh; // 设置最终哈希
  kref->len = lo; // 设置最终长度
  struct wormmeta * ret = wormhmap_get_kref(hmap, kref); // 精确查找
  if (likely(ret != NULL)) // 如果找到
    return ret; // 直接返回

  // 第三阶段：如果之前的peek产生假阳性，则进行更精细的搜索
  gd = lo; // 重置搜索范围
  lo = 0;
  loh = KV_CRC32C_SEED;

#define META_LCP_GAP_2 ((5u))
  while (META_LCP_GAP_2 < gd) {
    const u32 inc = (gd * 3) >> 2; // 步长
    wormhole_kref_inc(kref, lo, loh, inc); // 更新kref
    struct wormmeta * const tmp = wormhmap_get_kref(hmap, kref); // 精确查找
    if (tmp) { // 找到
      loh = kref->hash32;
      lo += inc;
      gd -= inc;
      ret = tmp;
      // 检查下一字节是否在位图中，如果是，则前缀可以更长
      if (wormmeta_bm_test(tmp, kref->ptr[lo])) {
        loh = crc32c_u8(loh, kref->ptr[lo]);
        lo++;
        gd--;
        ret = NULL; // 继续搜索更长的前缀
      } else {
        gd = 1; // 找到最长前缀，结束
        break;
      }
    } else {
      gd = inc; // 未找到，缩小范围
    }
  }

  while (1 < gd) { // 类似上面的循环，但步长不同
    const u32 inc = (gd * 3) >> 2;
    wormhole_kref_inc_123(kref, lo, loh, inc);
    struct wormmeta * const tmp = wormhmap_get_kref(hmap, kref);
    if (tmp) {
      loh = kref->hash32;
      lo += inc;
      gd -= inc;
      ret = tmp;
      if (wormmeta_bm_test(tmp, kref->ptr[lo])) {
        loh = crc32c_u8(loh, kref->ptr[lo]);
        lo++;
        gd--;
        ret = NULL;
      } else {
        break;
      }
    } else {
      gd = inc;
    }
  }
#undef META_LCP_GAP_2

  if (kref->len != lo) { // 更新kref
    kref->hash32 = loh;
    kref->len = lo;
  }
  if (ret == NULL) // 如果最后一次没找到，再找一次
    ret = wormhmap_get_kref(hmap, kref);
  debug_assert(ret); // 必须找到一个，至少空前缀是存在的
  return ret;
}
// }}} lcp

// down {{{
  // 从LPM元数据节点向下走到对应的叶子节点范围
  static struct wormleaf *
wormhole_meta_down(const struct wormhmap * const hmap, const struct kref * const lcp,
    const struct wormmeta * const meta, const u32 klen)
{
  if (likely(lcp->len < klen)) { // 部分匹配
    const u32 id0 = lcp->ptr[lcp->len]; // 获取搜索键在LPM之后的第一个字节
    if (wormmeta_bitmin_load(meta) > id0) { // 如果搜索键小于所有分支
      return wormmeta_lpath_load(meta); // 返回左路径叶子
    } else if (wormmeta_bitmax_load(meta) < id0) { // 如果搜索键大于所有分支
      return wormmeta_rmost_load(meta); // 返回最右叶子
    } else { // 如果搜索键在分支之间 (开销较大)
      // 找到小于id0的最大分支，并获取其最右叶子
      return wormmeta_rmost_load(wormhmap_get_kref1(hmap, lcp, (u8)wormmeta_bm_lt(meta, id0)));
    }
  } else { // 完全匹配 (lcp->len == klen)
    return wormmeta_lpath_load(meta); // 返回左路径叶子
  }
}
// }}} down

// jump-rw {{{
  // 结合LCP和down，从根跳转到目标叶子节点
  static struct wormleaf *
wormhole_jump_leaf(const struct wormhmap * const hmap, const struct kref * const key)
{
  struct kref kref = {.ptr = key->ptr}; // 创建一个可修改的kref副本
  debug_assert(kv_crc32c(key->ptr, key->len) == key->hash32); // 确认哈希正确

  const struct wormmeta * const meta = wormhole_meta_lcp(hmap, &kref, key->len); // 找到最长前缀匹配
  return wormhole_meta_down(hmap, &kref, meta, key->len); // 从LPM节点向下走到叶子
}

  // 为读操作跳转到叶子节点，并获取读锁
  static struct wormleaf *
wormhole_jump_leaf_read(struct wormref * const ref, const struct kref * const key)
{
  struct wormhole * const map = ref->map;
#pragma nounroll // 提示编译器不要展开此循环
  do {
    const struct wormhmap * const hmap = wormhmap_load(map); // 加载当前哈希表
    const u64 v = wormhmap_version_load(hmap); // 加载哈希表版本
    qsbr_update(&ref->qref, v); // 更新QSBR状态
    struct wormleaf * const leaf = wormhole_jump_leaf(hmap, key); // 跳转到叶子
    wormleaf_prefetch(leaf, key->hash32); // 预取叶子数据
#pragma nounroll
    do {
      if (rwlock_trylock_read_nr(&(leaf->leaflock), 64)) { // 尝试获取读锁 (重试64次)
        if (wormleaf_version_load(leaf) <= v) // 检查版本，确保叶子没有在我们跳转后被修改
          return leaf; // 成功，返回叶子
        wormleaf_unlock_read(leaf); // 版本不匹配，解锁并重试
        break;
      }
      // v1在lv之前加载；如果lv <= v，可以更新v1而无需重新跳转
      const u64 v1 = wormhmap_version_load(wormhmap_load(map)); // 加载最新版本
      if (wormleaf_version_load(leaf) > v) // 再次检查叶子版本
        break; // 叶子已更新，需要重新跳转
      wormhole_qsbr_update_pause(ref, v1); // 更新QSBR并暂停
    } while (true);
  } while (true);
}

  // 为写操作跳转到叶子节点，并获取写锁
  static struct wormleaf *
wormhole_jump_leaf_write(struct wormref * const ref, const struct kref * const key)
{
  struct wormhole * const map = ref->map;
#pragma nounroll
  do {
    const struct wormhmap * const hmap = wormhmap_load(map); // 加载当前哈希表
    const u64 v = wormhmap_version_load(hmap); // 加载版本
    qsbr_update(&ref->qref, v); // 更新QSBR
    struct wormleaf * const leaf = wormhole_jump_leaf(hmap, key); // 跳转
    wormleaf_prefetch(leaf, key->hash32); // 预取
#pragma nounroll
    do {
      if (rwlock_trylock_write_nr(&(leaf->leaflock), 64)) { // 尝试获取写锁
        if (wormleaf_version_load(leaf) <= v) // 检查版本
          return leaf; // 成功
        wormleaf_unlock_write(leaf); // 失败，解锁
        break;
      }
      // v1在lv之前加载；如果lv <= v，可以更新v1而无需重新跳转
      const u64 v1 = wormhmap_version_load(wormhmap_load(map)); // 加载最新版本
      if (wormleaf_version_load(leaf) > v) // 再次检查
        break;
      wormhole_qsbr_update_pause(ref, v1); // 更新QSBR并暂停
    } while (true);
  } while (true);
}
// }}} jump-rw

// }}} jump

// leaf-read {{{
// 叶子节点读取操作

  // 根据hs数组的索引获取kv指针
  static inline struct kv *
wormleaf_kv_at_ih(const struct wormleaf * const leaf, const u32 ih)
{
  return u64_to_ptr(leaf->hs[ih].e3); // 从entry13中提取指针
}

  // 根据ss数组的索引获取kv指针
  static inline struct kv *
wormleaf_kv_at_is(const struct wormleaf * const leaf, const u32 is)
{
  return u64_to_ptr(leaf->hs[leaf->ss[is]].e3); // 先从ss获取hs的索引，再获取指针
}

  // 预取ss数组到缓存
  static inline void
wormleaf_prefetch_ss(const struct wormleaf * const leaf)
{
  for (u32 i = 0; i < WH_KPN; i+=64) // 以64字节为步长
    cpu_prefetch0(&leaf->ss[i]); // 预取
}

// leaf must have been sorted
// return the key at [i] as if k1 has been inserted into leaf; i <= leaf->nr_sorted
// 假设k1已经被插入到叶子中，返回索引i处的键；叶子必须已排序
  static const struct kv *
wormleaf_kv_at_is1(const struct wormleaf * const leaf, const u32 i, const u32 is1, const struct kv * const k1)
{
  debug_assert(leaf->nr_keys == leaf->nr_sorted); // 确认已排序
  debug_assert(is1 <= leaf->nr_sorted); // 确认插入位置合法
  if (i < is1) // 如果在插入点之前
    return wormleaf_kv_at_is(leaf, i); // 返回原数组的键
  else if (i > is1) // 如果在插入点之后
    return wormleaf_kv_at_is(leaf, i-1); // 返回原数组中索引-1的键
  else // i == is1
    return k1; // 返回新插入的键
}

// fast point-lookup
// returns WH_KPN if not found
// 快速点查
// 未找到则返回 WH_KPN
  static u32
wormleaf_match_hs(const struct wormleaf * const leaf, const struct kref * const key)
{
  const u16 pkey = wormhole_pkey(key->hash32); // 计算pkey
  const u32 i0 = pkey / WH_HDIV; // 计算hs中的大致位置
  const struct entry13 * const hs = leaf->hs; // hs数组指针

  if (hs[i0].e1 == pkey) { // 检查理想位置
    struct kv * const curr = u64_to_ptr(hs[i0].e3);
    if (likely(wormhole_kref_kv_match(key, curr))) // 完整比较
      return i0; // 命中
  }
  if (hs[i0].e1 == 0) // 如果理想位置为空
    return WH_KPN; // 未找到

  // 线性探测向左搜索
  u32 i = i0 - 1;
  while (i < WH_KPN) {
    if (hs[i].e1 == pkey) {
      struct kv * const curr = u64_to_ptr(hs[i].e3);
      if (likely(wormhole_kref_kv_match(key, curr)))
        return i;
    } else if (hs[i].e1 < pkey) { // 由于hs是按pkey排序的，可以提前终止
      break;
    }
    i--;
  }

  // 线性探测向右搜索
  i = i0 + 1;
  while (i < WH_KPN) {
    if (hs[i].e1 == pkey) {
      struct kv * const curr = u64_to_ptr(hs[i].e3);
      if (likely(wormhole_kref_kv_match(key, curr)))
        return i;
    } else if ((hs[i].e1 > pkey) || (hs[i].e1 == 0)) { // 提前终止
      break;
    }
    i++;
  }

  // not found
  return WH_KPN; // 未找到
}

// search for an existing entry in hs
// 在hs中搜索一个已存在的条目
  static u32
wormleaf_search_ih(const struct wormleaf * const leaf, const struct entry13 e)
{
  const u16 pkey = e.e1; // 获取pkey
  const u32 i0 = pkey / WH_HDIV; // 计算理想位置
  const struct entry13 * const hs = leaf->hs;
  const struct entry13 e0 = hs[i0];

  if (e0.v64 == e.v64) // 检查理想位置
    return i0;

  if (e0.e1 == 0) // 理想位置为空
    return WH_KPN;

  // 线性探测向左
  u32 i = i0 - 1;
  while (i < WH_KPN) {
    const struct entry13 ei = hs[i];
    if (ei.v64 == e.v64) {
      return i;
    } else if (ei.e1 < pkey) {
      break;
    }
    i--;
  }

  // 线性探测向右
  i = i0 + 1;
  while (i < WH_KPN) {
    const struct entry13 ei = hs[i];
    if (ei.v64 == e.v64) {
      return i;
    } else if ((ei.e1 > pkey) || (ei.e1 == 0)) {
      break;
    }
    i++;
  }

  // not found
  return WH_KPN;
}

// search for an existing entry in ss
// 在ss中搜索一个已存在的条目 (通过hs的索引ih)
  static u32
wormleaf_search_is(const struct wormleaf * const leaf, const u8 ih)
{
#if defined(__x86_64__)
  // TODO: avx512
#if defined(__AVX2__)
  const m256 i1 = _mm256_set1_epi8((char)ih); // 将ih广播到256位向量
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m256)) { // 遍历ss数组
    const m256 sv = _mm256_load_si256((m256 *)(leaf->ss+i)); // 加载数据
    const u32 mask = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(sv, i1)); // 比较并生成掩码
    if (mask) // 如果有匹配
      return i + (u32)__builtin_ctz(mask); // 返回索引
  }
#else // SSE4.2
  const m128 i1 = _mm_set1_epi8((char)ih); // 广播到128位向量
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m128)) {
    const m128 sv = _mm_load_si128((m128 *)(leaf->ss+i));
    const u32 mask = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(sv, i1));
    if (mask)
      return i + (u32)__builtin_ctz(mask);
  }
#endif // __AVX2__
#elif defined(__aarch64__)
  static const m128 vtbl = {0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15};
  static const uint16x8_t mbits = {0x0101, 0x0202, 0x0404, 0x0808, 0x1010, 0x2020, 0x4040, 0x8080};
  const m128 i1 = vdupq_n_u8(ih); // 广播
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m128)) {
    const m128 cmp = vceqq_u8(vld1q_u8(leaf->ss+i), i1); // 比较
    const m128 cmp1 = vqtbl1q_u8(cmp, vtbl); // 重排
    const u32 mask = (u32)vaddvq_u16(vandq_u8(vreinterpretq_u16_u8(cmp1), mbits)); // 生成掩码
    if (mask)
      return i + (u32)__builtin_ctz(mask); // 返回索引
  }
#endif // __x86_64__
  debug_die(); // 理论上不应该到这里，因为ih肯定在ss中
}

// assumes there in no duplicated keys
// search the first key that is >= the given key
// return 0 .. nr_sorted
// 假设没有重复键，搜索第一个大于等于给定键的键
// 返回值范围是 0 到 nr_sorted
  static u32
wormleaf_search_ss(const struct wormleaf * const leaf, const struct kref * const key)
{
  u32 lo = 0;
  u32 hi = leaf->nr_sorted;
  // 优化的二分查找，减少缓存不命中
  while ((lo + 2) < hi) {
    const u32 i = (lo + hi) >> 1;
    const struct kv * const curr = wormleaf_kv_at_is(leaf, i);
    cpu_prefetch0(curr); // 预取键值数据
    cpu_prefetch0(leaf->hs + leaf->ss[(lo + i) >> 1]); // 预取hs条目
    cpu_prefetch0(leaf->hs + leaf->ss[(i + 1 + hi) >> 1]);
    const int cmp = kref_kv_compare(key, curr); // 比较
    debug_assert(cmp != 0); // 假设无重复键
    if (cmp < 0)
      hi = i;
    else
      lo = i + 1;
  }

  // 标准二分查找
  while (lo < hi) {
    const u32 i = (lo + hi) >> 1;
    const struct kv * const curr = wormleaf_kv_at_is(leaf, i);
    const int cmp = kref_kv_compare(key, curr);
    debug_assert(cmp != 0);
    if (cmp < 0)
      hi = i;
    else
      lo = i + 1;
  }
  return lo; // 返回下界
}

  // 在叶子节点中查找键
  static u32
wormleaf_seek(const struct wormleaf * const leaf, const struct kref * const key)
{
  debug_assert(leaf->nr_sorted == leaf->nr_keys); // 确保已排序
  wormleaf_prefetch_ss(leaf); // 预取ss数组，对命中和未命中都有好处
  const u32 ih = wormleaf_match_hs(leaf, key); // 首先尝试快速点查
  if (ih < WH_KPN) { // 命中
    return wormleaf_search_is(leaf, (u8)ih); // 在ss中找到对应的索引
  } else { // 未命中，二分查找第一个大于等于的键
    return wormleaf_search_ss(leaf, key);
  }
}

// same to search_sorted but the target is very likely beyond the end
// 与search_sorted类似，但目标很可能在末尾之后
  static u32
wormleaf_seek_end(const struct wormleaf * const leaf, const struct kref * const key)
{
  debug_assert(leaf->nr_keys == leaf->nr_sorted);
  if (leaf->nr_sorted) {
    // 先和最后一个元素比较，做快速路径优化
    const int cmp = kref_kv_compare(key, wormleaf_kv_at_is(leaf, leaf->nr_sorted-1));
    if (cmp > 0) // 大于所有元素
      return leaf->nr_sorted;
    else if (cmp == 0) // 等于最后一个元素
      return leaf->nr_sorted - 1;
    else // 小于最后一个元素，正常查找
      return wormleaf_seek(leaf, key);
  } else {
    return 0; // 叶子为空
  }
}
// }}} leaf-read

// leaf-write {{{
// 叶子节点写入操作

  // 对两个已排序的ss子数组进行归并
  static void
wormleaf_sort_m2(struct wormleaf * const leaf, const u32 n1, const u32 n2)
{
  if (n1 == 0 || n2 == 0)
    return; // 无需排序

  u8 * const ss = leaf->ss;
  u8 et[WH_KPN/2]; // 临时缓冲区，min(n1,n2) < KPN/2
  if (n1 <= n2) { // 归并左边部分
    memcpy(et, &(ss[0]), sizeof(ss[0]) * n1); // 复制左边部分到临时缓冲区
    u8 * eo = ss; // 输出指针
    u8 * e1 = et; // 左边部分指针
    u8 * e2 = &(ss[n1]); // 右边部分指针
    const u8 * const z1 = e1 + n1; // 左边结束
    const u8 * const z2 = e2 + n2; // 右边结束
    while ((e1 < z1) && (e2 < z2)) { // 归并
      const int cmp = kv_compare(wormleaf_kv_at_ih(leaf, *e1), wormleaf_kv_at_ih(leaf, *e2));
      if (cmp < 0)
        *(eo++) = *(e1++);
      else if (cmp > 0)
        *(eo++) = *(e2++);
      else
        debug_die(); // 不应有重复键

      if (eo == e2) // 提前结束优化
        break;
    }
    if (eo < e2) // 复制剩余部分
      memcpy(eo, e1, sizeof(*eo) * (size_t)(e2 - eo));
  } else { // 归并右边部分 (逻辑类似，但方向相反)
    memcpy(et, &(ss[n1]), sizeof(ss[0]) * n2);
    u8 * eo = &(ss[n1 + n2 - 1]); // 从后往前归并
    u8 * e1 = &(ss[n1 - 1]);
    u8 * e2 = &(et[n2 - 1]);
    const u8 * const z1 = e1 - n1;
    const u8 * const z2 = e2 - n2;
    while ((e1 > z1) && (e2 > z2)) {
      const int cmp = kv_compare(wormleaf_kv_at_ih(leaf, *e1), wormleaf_kv_at_ih(leaf, *e2));
      if (cmp < 0)
        *(eo--) = *(e2--);
      else if (cmp > 0)
        *(eo--) = *(e1--);
      else
        debug_die();

      if (eo == e1)
        break;
    }
    if (eo > e1)
      memcpy(e1 + 1, et, sizeof(*eo) * (size_t)(eo - e1));
  }
}

#if defined(__linux__)
  // qsort_r 的比较函数 (Linux版本)
  static int
wormleaf_ss_cmp(const void * const p1, const void * const p2, void * priv)
{
  const struct kv * const k1 = wormleaf_kv_at_ih(priv, *(const u8 *)p1);
  const struct kv * const k2 = wormleaf_kv_at_ih(priv, *(const u8 *)p2);
  return kv_compare(k1, k2);
}
#else // (FreeBSD and APPLE only)
  // qsort_r 的比较函数 (BSD/macOS版本)
  static int
wormleaf_ss_cmp(void * priv, const void * const p1, const void * const p2)
{
  const struct kv * const k1 = wormleaf_kv_at_ih(priv, *(const u8 *)p1);
  const struct kv * const k2 = wormleaf_kv_at_ih(priv, *(const u8 *)p2);
  return kv_compare(k1, k2);
}
#endif // __linux__

  // 对ss数组的指定范围进行排序
  static inline void
wormleaf_sort_range(struct wormleaf * const leaf, const u32 i0, const u32 nr)
{
#if defined(__linux__)
  qsort_r(&(leaf->ss[i0]), nr, sizeof(leaf->ss[0]), wormleaf_ss_cmp, leaf);
#else // (FreeBSD and APPLE only)
  qsort_r(&(leaf->ss[i0]), nr, sizeof(leaf->ss[0]), leaf, wormleaf_ss_cmp);
#endif // __linux__
}

// make sure all keys are sorted in a leaf node
// 确保叶子节点中的所有键都有序
  static void
wormleaf_sync_sorted(struct wormleaf * const leaf)
{
  const u32 s = leaf->nr_sorted; // 已排序数量
  const u32 n = leaf->nr_keys; // 总数量
  if (s == n) // 如果已经全部有序
    return;

  wormleaf_sort_range(leaf, s, n - s); // 对未排序部分进行排序
  // merge-sort inplace
  wormleaf_sort_m2(leaf, s, n - s); // 将已排序和新排序的部分归并
  leaf->nr_sorted = n; // 更新已排序数量
}

// shift a sequence of entries on hs and update the corresponding ss values
// 在hs中移动一个条目序列，并更新ss中对应的索引值
  static void
wormleaf_shift_inc(struct wormleaf * const leaf, const u32 to, const u32 from, const u32 nr)
{
  debug_assert(to == (from+1)); // 确认是向右移动一位
  struct entry13 * const hs = leaf->hs;
  memmove(&(hs[to]), &(hs[from]), sizeof(hs[0]) * nr); // 移动hs中的数据

// 使用SIMD指令批量更新ss数组中的索引
#if defined(__x86_64__)
  // TODO: avx512
#if defined(__AVX2__)
  const m256 ones = _mm256_set1_epi8(1); // 全1向量
  const m256 addx = _mm256_set1_epi8((char)(u8)(INT8_MAX + 1 - from - nr));
  const m256 cmpx = _mm256_set1_epi8((char)(u8)(INT8_MAX - nr));
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m256)) {
    const m256 sv = _mm256_load_si256((m256 *)(leaf->ss+i)); // 加载ss数据
    // 通过比较生成掩码，确定哪些索引需要加1
    const m256 add1 = _mm256_and_si256(_mm256_cmpgt_epi8(_mm256_add_epi8(sv, addx), cmpx), ones);
    _mm256_store_si256((m256 *)(leaf->ss+i), _mm256_add_epi8(sv, add1)); // 存储更新后的值
  }
#else // SSE4.2
  const m128 ones = _mm_set1_epi8(1);
  const m128 addx = _mm_set1_epi8((char)(u8)(INT8_MAX + 1 - from - nr));
  const m128 cmpx = _mm_set1_epi8((char)(u8)(INT8_MAX - nr));
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m128)) {
    const m128 sv = _mm_load_si128((m128 *)(leaf->ss+i));
    const m128 add1 = _mm_and_si128(_mm_cmpgt_epi8(_mm_add_epi8(sv, addx), cmpx), ones);
    _mm_store_si128((m128 *)(leaf->ss+i), _mm_add_epi8(sv, add1));
  }
#endif // __AVX2__
#elif defined(__aarch64__) // __x86_64__
  // aarch64 NEON 实现
  const m128 subx = vdupq_n_u8((u8)from);
  const m128 cmpx = vdupq_n_u8((u8)nr);
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m128)) {
    const m128 sv = vld1q_u8(leaf->ss+i);
    const m128 add1 = vshrq_n_u8(vcltq_u8(vsubq_u8(sv, subx), cmpx), 7);
    vst1q_u8(leaf->ss+i, vaddq_u8(sv, add1));
  }
#endif // __x86_64__
}

  // 向左移动hs中的条目序列并更新ss
  static void
wormleaf_shift_dec(struct wormleaf * const leaf, const u32 to, const u32 from, const u32 nr)
{
  debug_assert(to == (from-1)); // 确认是向左移动一位
  struct entry13 * const hs = leaf->hs;
  memmove(&(hs[to]), &(hs[from]), sizeof(hs[0]) * nr); // 移动hs数据

// SIMD更新ss，逻辑与shift_inc类似，只是最后是减法
#if defined(__x86_64__)
  // TODO: avx512
#if defined(__AVX2__)
  const m256 ones = _mm256_set1_epi8(1);
  const m256 addx = _mm256_set1_epi8((char)(u8)(INT8_MAX + 1 - from - nr));
  const m256 cmpx = _mm256_set1_epi8((char)(u8)(INT8_MAX - nr));
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m256)) {
    const m256 sv = _mm256_load_si256((m256 *)(leaf->ss+i));
    const m256 add1 = _mm256_and_si256(_mm256_cmpgt_epi8(_mm256_add_epi8(sv, addx), cmpx), ones);
    _mm256_store_si256((m256 *)(leaf->ss+i), _mm256_sub_epi8(sv, add1));
  }
#else // SSE4.2
  const m128 ones = _mm_set1_epi8(1);
  const m128 addx = _mm_set1_epi8((char)(u8)(INT8_MAX + 1 - from - nr));
  const m128 cmpx = _mm_set1_epi8((char)(u8)(INT8_MAX - nr));
  for (u32 i = 0; i < leaf->nr_keys; i += 16) {
    const m128 sv = _mm_load_si128((m128 *)(leaf->ss+i));
    const m128 add1 = _mm_and_si128(_mm_cmpgt_epi8(_mm_add_epi8(sv, addx), cmpx), ones);
    _mm_store_si128((m128 *)(leaf->ss+i), _mm_sub_epi8(sv, add1));
  }
#endif // __AVX2__
#elif defined(__aarch64__) // __x86_64__
  // aarch64
  const m128 subx = vdupq_n_u8((u8)from);
  const m128 cmpx = vdupq_n_u8((u8)nr);
  for (u32 i = 0; i < leaf->nr_keys; i += sizeof(m128)) {
    const m128 sv = vld1q_u8(leaf->ss+i);
    const m128 add1 = vshrq_n_u8(vcltq_u8(vsubq_u8(sv, subx), cmpx), 7);
    vst1q_u8(leaf->ss+i, vsubq_u8(sv, add1));
  }
#endif // __x86_64__
}

// insert hs and also shift ss
// 插入条目到hs中，并更新ss
  static u32
wormleaf_insert_hs(struct wormleaf * const leaf, const struct entry13 e)
{
  struct entry13 * const hs = leaf->hs;
  const u16 pkey = e.e1;
  const u32 i0 = pkey / WH_HDIV; // 计算理想位置
  if (hs[i0].e1 == 0) { // 如果理想位置为空，直接插入
    hs[i0] = e;
    return i0;
  }

  // 找到最左边的插入点
  u32 i = i0;
  while (i && hs[i-1].e1 && (hs[i-1].e1 >= pkey))
    i--;
  while ((i < WH_KPN) && hs[i].e1 && (hs[i].e1 < pkey)) // 找到第一个pkey大于等于或为空的位置
    i++;
  const u32 il = --i; // 插入点左边界

  // 找到左边的空槽位
  if (i > (i0 - 1))
    i = i0 - 1;
  while ((i < WH_KPN) && hs[i].e1)
    i--;
  const u32 el = i; // 左边空槽位索引

  // 找到最右边的插入点
  i = il + 1;
  while ((i < WH_KPN) && hs[i].e1 && (hs[i].e1 == pkey))
    i++;
  const u32 ir = i; // 插入点右边界

  // 找到右边的空槽位
  if (i < (i0 + 1))
    i = i0 + 1;
  while ((i < WH_KPN) && hs[i].e1)
    i++;
  const u32 er = i; // 右边空槽位索引

  // el <= il < ir <= er    (如果 < WH_KPN)
  const u32 dl = (el < WH_KPN) ? (il - el) : WH_KPN; // 到左边空位的距离
  const u32 dr = (er < WH_KPN) ? (er - ir) : WH_KPN; // 到右边空位的距离
  if (dl <= dr) { // 如果向左移动更近
    debug_assert(dl < WH_KPN);
    if (dl)
      wormleaf_shift_dec(leaf, el, el+1, dl); // 向左移动腾出空间
    hs[il] = e; // 插入
    return il;
  } else { // 向右移动更近
    debug_assert(dr < WH_KPN);
    if (dr)
      wormleaf_shift_inc(leaf, ir+1, ir, dr); // 向右移动腾出空间
    hs[ir] = e; // 插入
    return ir;
  }
}

  // 插入一个entry13条目到叶子
  static void
wormleaf_insert_e13(struct wormleaf * const leaf, const struct entry13 e)
{
  // 插入到hs并修复所有现有的is
  const u32 ih = wormleaf_insert_hs(leaf, e);
  debug_assert(ih < WH_KPN);
  // 将新的hs索引附加到ss
  leaf->ss[leaf->nr_keys] = (u8)ih;
  // 修复nr
  leaf->nr_keys++;
}

  // 插入一个新的kv到叶子
  static void
wormleaf_insert(struct wormleaf * const leaf, const struct kv * const new)
{
  debug_assert(new->hash == kv_crc32c_extend(kv_crc32c(new->kv, new->klen))); // 确认哈希
  debug_assert(leaf->nr_keys < WH_KPN); // 确认叶子未满

  // 插入
  const struct entry13 e = entry13(wormhole_pkey(new->hashlo), ptr_to_u64(new));
  const u32 nr0 = leaf->nr_keys;
  wormleaf_insert_e13(leaf, e);

  // 顺序插入优化
  if (nr0 == leaf->nr_sorted) { // 如果之前是完全有序的
    if (nr0) {
      const struct kv * const kvn = wormleaf_kv_at_is(leaf, nr0 - 1);
      if (kv_compare(new, kvn) > 0) // 如果新键大于最后一个键
        leaf->nr_sorted = nr0 + 1; // 仍然有序
    } else {
      leaf->nr_sorted = 1; // 第一个元素
    }
  }
}

  // 在hs中删除一个条目后，将周围的条目拉过来填补空位，保持紧凑
  static void
wormleaf_pull_ih(struct wormleaf * const leaf, const u32 ih)
{
  struct entry13 * const hs = leaf->hs;
  // 尝试从左边拉
  u32 i = ih - 1;
  while ((i < WH_KPN) && hs[i].e1 && ((hs[i].e1 / WH_HDIV) > i))
    i--;

  if ((++i) < ih) {
    wormleaf_shift_inc(leaf, i+1, i, ih - i); // 向右移动
    leaf->hs[i].v64 = 0; // 清空
    return;
  }

  // 尝试从右边拉
  i = ih + 1;
  while ((i < WH_KPN) && hs[i].e1 && ((hs[i].e1 / WH_HDIV) < i))
    i++;

  if ((--i) > ih) {
    wormleaf_shift_dec(leaf, ih, ih+1, i - ih); // 向左移动
    hs[i].v64 = 0; // 清空
  }
  // hs[ih] may still be 0
}

// internal only
// 内部函数，移除一个条目
  static struct kv *
wormleaf_remove(struct wormleaf * const leaf, const u32 ih, const u32 is)
{
  // ss
  leaf->ss[is] = leaf->ss[leaf->nr_keys - 1]; // 用最后一个ss条目覆盖被删除的
  if (leaf->nr_sorted > is) // 如果删除点在已排序部分
    leaf->nr_sorted = is; // 将有序边界缩减到删除点

  // ret
  struct kv * const victim = wormleaf_kv_at_ih(leaf, ih); // 获取被删除的kv
  // hs
  leaf->hs[ih].v64 = 0; // 清空hs条目
  leaf->nr_keys--; // 键数量减一
  // use magnet
  wormleaf_pull_ih(leaf, ih); // 整理hs数组
  return victim;
}

// remove key from leaf but do not call free
// 从叶子中移除键，但不释放它
  static struct kv *
wormleaf_remove_ih(struct wormleaf * const leaf, const u32 ih)
{
  // 从ss中移除
  const u32 is = wormleaf_search_is(leaf, (u8)ih); // 找到对应的ss索引
  debug_assert(is < leaf->nr_keys);
  return wormleaf_remove(leaf, ih, is); // 调用内部移除函数
}

  // 通过ss索引移除键
  static struct kv *
wormleaf_remove_is(struct wormleaf * const leaf, const u32 is)
{
  return wormleaf_remove(leaf, leaf->ss[is], is);
}

// for delr (delete-range)
// 范围删除
  static void
wormleaf_delete_range(struct wormhole * const map, struct wormleaf * const leaf,
    const u32 i0, const u32 end)
{
  debug_assert(leaf->nr_keys == leaf->nr_sorted); // 确保已排序
  for (u32 i = end; i > i0; i--) { // 从后往前删除
    const u32 ir = i - 1;
    struct kv * const victim = wormleaf_remove_is(leaf, ir); // 移除
    map->mm.free(victim, map->mm.priv); // 释放
  }
}

// return the old kv; the caller should free the old kv
// 返回旧的kv；调用者应负责释放旧kv
  static struct kv *
wormleaf_update(struct wormleaf * const leaf, const u32 ih, const struct kv * const new)
{
  debug_assert(new->hash == kv_crc32c_extend(kv_crc32c(new->kv, new->klen))); // 确认哈希
  // 在ss中搜索条目 (is)
  struct kv * const old = wormleaf_kv_at_ih(leaf, ih); // 获取旧的kv
  debug_assert(old);

  entry13_update_e3(&leaf->hs[ih], (u64)new); // 更新hs中的指针
  return old; // 返回旧的kv
}
// }}} leaf-write

// leaf-split {{{
// 叶子节点分裂

// It only works correctly in cut_search
// quickly tell if a cut between k1 and k2 can achieve a specific anchor-key length
// 快速判断在k1和k2之间切割是否能得到一个特定长度的锚点键
  static bool
wormhole_split_cut_alen_check(const u32 alen, const struct kv * const k1, const struct kv * const k2)
{
  debug_assert(k2->klen >= alen); // 确认k2长度足够
  return (k1->klen < alen) || (k1->kv[alen - 1] != k2->kv[alen - 1]); // 检查k1长度或第alen-1个字节
}

// return the number of keys that should go to leaf1
// assert(r > 0 && r <= nr_keys)
// (1) r < is1, anchor key is ss[r-1]:ss[r]
// (2) r == is1: anchor key is ss[r-1]:new
// (3) r == is1+1: anchor key is new:ss[r-1] (ss[r-1] is the ss[r] on the logically sorted array)
// (4) r > is1+1: anchor key is ss[r-2]:ss[r-1] (ss[r-2] is the [r-1] on the logically sorted array)
// edge cases:
//   (case 2) is1 == nr_keys: r = nr_keys; ss[r-1]:new
//   (case 3) is1 == 0, r == 1; new:ss[0]
// return 1..WH_KPN
// 搜索最佳分裂点，返回应该留在leaf1中的键的数量
  static u32
wormhole_split_cut_search1(struct wormleaf * const leaf, u32 l, u32 h, const u32 is1, const struct kv * const new)
{
  debug_assert(leaf->nr_keys == leaf->nr_sorted); // 确认已排序
  debug_assert(leaf->nr_keys);
  debug_assert(l < h && h <= leaf->nr_sorted);

  const struct kv * const kl0 = wormleaf_kv_at_is1(leaf, l, is1, new); // 获取逻辑上第一个键
  const struct kv * const kh0 = wormleaf_kv_at_is1(leaf, h, is1, new); // 获取逻辑上最后一个键
  const u32 alen = kv_key_lcp(kl0, kh0) + 1; // 计算锚点键长度
  if (unlikely(alen > UINT16_MAX)) // 检查长度溢出
    return WH_KPN2;

  const u32 target = leaf->next ? WH_MID : WH_KPN_MRG; // 根据是否有后继节点确定分裂目标
  while ((l + 1) < h) { // 二分查找
    const u32 m = (l + h + 1) >> 1;
    if (m <= target) { // 尝试右边
      const struct kv * const k1 = wormleaf_kv_at_is1(leaf, m, is1, new);
      const struct kv * const k2 = wormleaf_kv_at_is1(leaf, h, is1, new);
      if (wormhole_split_cut_alen_check(alen, k1, k2))
        l = m;
      else
        h = m;
    } else { // 尝试左边
      const struct kv * const k1 = wormleaf_kv_at_is1(leaf, l, is1, new);
      const struct kv * const k2 = wormleaf_kv_at_is1(leaf, m, is1, new);
      if (wormhole_split_cut_alen_check(alen, k1, k2))
        h = m;
      else
        l = m;
    }
  }
  return h; // 返回分裂点
}

  // 将leaf1中的部分键移动到leaf2
  static void
wormhole_split_leaf_move1(struct wormleaf * const leaf1, struct wormleaf * const leaf2,
    const u32 cut, const u32 is1, const struct kv * const new)
{
  const u32 nr_keys = leaf1->nr_keys;
  const struct entry13 e1 = entry13(wormhole_pkey(new->hashlo), ptr_to_u64(new));
  struct entry13 es[WH_KPN]; // 临时数组

  if (cut <= is1) { // 如果新键e1在分裂点之后，应进入leaf2
    // leaf2
    for (u32 i = cut; i < is1; i++) // 移动cut到is1之间的键
      wormleaf_insert_e13(leaf2, leaf1->hs[leaf1->ss[i]]);

    wormleaf_insert_e13(leaf2, e1); // 插入新键

    for (u32 i = is1; i < nr_keys; i++) // 移动is1之后的键
      wormleaf_insert_e13(leaf2, leaf1->hs[leaf1->ss[i]]);

    // leaf1
    for (u32 i = 0; i < cut; i++) // 收集要留在leaf1的键
      es[i] = leaf1->hs[leaf1->ss[i]];

  } else { // 如果新键e1在分裂点之前，应进入leaf1
    // leaf2
    for (u32 i = cut - 1; i < nr_keys; i++) // 移动cut-1之后的键到leaf2
      wormleaf_insert_e13(leaf2, leaf1->hs[leaf1->ss[i]]);

    // leaf1
    for (u32 i = 0; i < is1; i++) // 收集is1之前的键
      es[i] = leaf1->hs[leaf1->ss[i]];

    es[is1] = e1; // 插入新键

    for (u32 i = is1 + 1; i < cut; i++) // 收集is1和cut之间的键
      es[i] = leaf1->hs[leaf1->ss[i - 1]];
  }

  leaf2->nr_sorted = leaf2->nr_keys; // leaf2是完全有序的

  memset(leaf1->hs, 0, sizeof(leaf1->hs[0]) * WH_KPN); // 清空leaf1
  leaf1->nr_keys = 0;
  for (u32 i = 0; i < cut; i++) // 重新插入留在leaf1的键
    wormleaf_insert_e13(leaf1, es[i]);
  leaf1->nr_sorted = cut; // leaf1也是完全有序的
  debug_assert((leaf1->nr_sorted + leaf2->nr_sorted) == (nr_keys + 1)); // 确认总数正确
}

// create an anchor for leaf-split
// 为叶子分裂创建一个锚点键
  static struct kv *
wormhole_split_alloc_anchor(const struct kv * const key1, const struct kv * const key2)
{
  const u32 alen = kv_key_lcp(key1, key2) + 1; // 计算LCP+1作为锚点长度
  debug_assert(alen <= key2->klen);

  struct kv * const anchor = wormhole_alloc_akey(alen); // 分配锚点键
  if (anchor)
    kv_refill(anchor, key2->kv, alen, NULL, 0); // 填充锚点键内容
  return anchor;
}

// leaf1 is locked
// split leaf1 into leaf1+leaf2; insert new into leaf1 or leaf2, return leaf2
// leaf1已被写锁定
// 将leaf1分裂成leaf1和leaf2；将new插入到leaf1或leaf2中，返回leaf2
  static struct wormleaf *
wormhole_split_leaf(struct wormhole * const map, struct wormleaf * const leaf1, struct kv * const new)
{
  wormleaf_sync_sorted(leaf1); // 确保leaf1有序
  struct kref kref_new;
  kref_ref_kv(&kref_new, new);
  const u32 is1 = wormleaf_search_ss(leaf1, &kref_new); // 找到new的逻辑插入位置
  const u32 cut = wormhole_split_cut_search1(leaf1, 0, leaf1->nr_keys, is1, new); // 找到最佳分裂点
  if (unlikely(cut == WH_KPN2)) // 如果分裂失败
    return NULL;

  // leaf2的锚点
  debug_assert(cut && (cut <= leaf1->nr_keys));
  const struct kv * const key1 = wormleaf_kv_at_is1(leaf1, cut - 1, is1, new); // 分裂点左边的键
  const struct kv * const key2 = wormleaf_kv_at_is1(leaf1, cut, is1, new); // 分裂点右边的键
  struct kv * const anchor2 = wormhole_split_alloc_anchor(key1, key2); // 创建新叶子的锚点
  if (unlikely(anchor2 == NULL)) // 锚点分配失败
    return NULL;

  // 用anchor2创建leaf2
  struct wormleaf * const leaf2 = wormleaf_alloc(map, leaf1, leaf1->next, anchor2);
  if (unlikely(leaf2 == NULL)) {
    wormhole_free_akey(anchor2);
    return NULL;
  }

  // split_hmap将解锁叶子节点；必须现在移动
  wormhole_split_leaf_move1(leaf1, leaf2, cut, is1, new);
  // 分裂后leaf1和leaf2都应该是有序的
  debug_assert(leaf1->nr_keys == leaf1->nr_sorted);
  debug_assert(leaf2->nr_keys == leaf2->nr_sorted);

  return leaf2;
}
// }}} leaf-split

// leaf-merge {{{
// 叶子节点合并

// MERGE is the only operation that deletes a leaf node (leaf2).
// It ALWAYS merges the right node into the left node even if the left is empty.
// This requires both of their writer locks to be acquired.
// This allows iterators to safely probe the next node (but not backwards).
// In other words, if either the reader or the writer lock of node X has been acquired:
// X->next (the pointer) cannot be changed by any other thread.
// X->next cannot be deleted.
// But the content in X->next can still be changed.
// 合并是唯一删除叶子节点(leaf2)的操作。
// 它总是将右节点合并到左节点，即使左节点是空的。
// 这需要获取两个节点的写锁。
// 这允许迭代器安全地探测下一个节点（但不能向后）。
// 换句话说，如果节点X的读锁或写锁已被获取：
// X->next（指针）不能被任何其他线程更改。
// X->next 不能被删除。
// 但 X->next 中的内容仍然可以更改。
  static bool
wormleaf_merge(struct wormleaf * const leaf1, struct wormleaf * const leaf2)
{
  debug_assert((leaf1->nr_keys + leaf2->nr_keys) <= WH_KPN); // 确认合并后不会溢出
  const bool leaf1_sorted = leaf1->nr_keys == leaf1->nr_sorted; // 记录leaf1是否已排序

  for (u32 i = 0; i < leaf2->nr_keys; i++) // 将leaf2的所有键插入leaf1
    wormleaf_insert_e13(leaf1, leaf2->hs[leaf2->ss[i]]);
  if (leaf1_sorted) // 如果leaf1原来是有序的
    leaf1->nr_sorted += leaf2->nr_sorted; // 更新有序数量
  return true;
}

// for undoing insertion under split_meta failure; leaf2 is still local
// remove the new key; merge keys in leaf2 into leaf1; free leaf2
// 用于在split_meta失败时撤销插入；leaf2仍然是本地的
// 移除新键；将leaf2中的键合并到leaf1；释放leaf2
  static void
wormleaf_split_undo(struct wormhole * const map, struct wormleaf * const leaf1,
    struct wormleaf * const leaf2, struct kv * const new)
{
  if (new) { // 如果有新键需要移除
    const struct entry13 e = entry13(wormhole_pkey(new->hashlo), ptr_to_u64(new));
    const u32 im1 = wormleaf_search_ih(leaf1, e); // 在leaf1中查找
    if (im1 < WH_KPN) {
      (void)wormleaf_remove_ih(leaf1, im1);
    } else { // 在leaf2中查找
      const u32 im2 = wormleaf_search_ih(leaf2, e);
      debug_assert(im2 < WH_KPN);
      (void)wormleaf_remove_ih(leaf2, im2);
    }
  }
  // 这个合并必须成功
  if (!wormleaf_merge(leaf1, leaf2))
    debug_die();
  // 保持这个以避免在wormleaf_free中触发假警报
  leaf2->leaflock.opaque = 0;
  wormleaf_free(map->slab_leaf, leaf2); // 释放leaf2
}
// }}} leaf-merge

// get/probe {{{
// 获取/探测操作

  // 获取一个键的值
  struct kv *
wormhole_get(struct wormref * const ref, const struct kref * const key, struct kv * const out)
{
  struct wormleaf * const leaf = wormhole_jump_leaf_read(ref, key); // 跳转到叶子并加读锁
  const u32 i = wormleaf_match_hs(leaf, key); // 在叶子中查找
  // 如果找到，通过mm.out复制值到out缓冲区
  struct kv * const tmp = (i < WH_KPN) ? ref->map->mm.out(wormleaf_kv_at_ih(leaf, i), out) : NULL;
  wormleaf_unlock_read(leaf); // 解锁
  return tmp;
}

  // 线程安全的get (带park/resume)
  struct kv *
whsafe_get(struct wormref * const ref, const struct kref * const key, struct kv * const out)
{
  wormhole_resume(ref); // 恢复QSBR
  struct kv * const ret = wormhole_get(ref, key, out);
  wormhole_park(ref); // 暂停QSBR
  return ret;
}

  // 非线程安全的get
  struct kv *
whunsafe_get(struct wormhole * const map, const struct kref * const key, struct kv * const out)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, key); // 不加锁跳转
  const u32 i = wormleaf_match_hs(leaf, key);
  return (i < WH_KPN) ? map->mm.out(wormleaf_kv_at_ih(leaf, i), out) : NULL;
}

  // 探测一个键是否存在
  bool
wormhole_probe(struct wormref * const ref, const struct kref * const key)
{
  struct wormleaf * const leaf = wormhole_jump_leaf_read(ref, key); // 跳转并加读锁
  const u32 i = wormleaf_match_hs(leaf, key); // 查找
  wormleaf_unlock_read(leaf); // 解锁
  return i < WH_KPN; // 返回是否找到
}

  // 线程安全的probe
  bool
whsafe_probe(struct wormref * const ref, const struct kref * const key)
{
  wormhole_resume(ref);
  const bool r = wormhole_probe(ref, key);
  wormhole_park(ref);
  return r;
}

  // 非线程安全的probe
  bool
whunsafe_probe(struct wormhole * const map, const struct kref * const key)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, key);
  return wormleaf_match_hs(leaf, key) < WH_KPN;
}
// }}} get/probe

// meta-split {{{
// 元数据分裂

// duplicate from meta1; only has one bit but will soon add a new bit
// 从meta1复制；只有一个位，但很快会添加一个新位
// 将半节点(half-node)扩展为全节点(full-node)
  static struct wormmeta *
wormmeta_expand(struct wormhmap * const hmap, struct wormmeta * const meta1)
{
  struct wormmeta * const meta2 = slab_alloc_unsafe(hmap->slab2); // 从带位图的slab分配
  if (meta2 == NULL)
    return NULL;

  memcpy(meta2, meta1, sizeof(*meta1)); // 复制内容
  for (u32 i = 0; i < WH_BMNR; i++)
    meta2->bitmap[i] = 0; // 初始化位图
  const u32 bitmin = wormmeta_bitmin_load(meta1);
  debug_assert(bitmin == wormmeta_bitmax_load(meta1));
  debug_assert(bitmin < WH_FO);
  // 设置唯一的位
  meta2->bitmap[bitmin >> 6u] |= (1lu << (bitmin & 0x3fu));

  wormhmap_replace(hmap, meta1, meta2); // 在哈希表中替换
  slab_free_unsafe(hmap->slab1, meta1); // 释放旧的元数据节点
  return meta2;
}

  // 设置元数据位图的辅助函数
  static struct wormmeta *
wormmeta_bm_set_helper(struct wormhmap * const hmap, struct wormmeta * const meta, const u32 id)
{
  debug_assert(id < WH_FO);
  const u32 bitmin = wormmeta_bitmin_load(meta);
  const u32 bitmax = wormmeta_bitmax_load(meta);
  if (bitmin < bitmax) { // 如果已经是全节点
    wormmeta_bm_set(meta, id); // 直接设置位
    return meta;
  } else if (id == bitmin) { // 如果位已存在
    return meta; // 什么都不做
  } else if (bitmin == WH_FO) { // 如果是空节点
    wormmeta_bitmin_store(meta, id); // 添加第一个位
    wormmeta_bitmax_store(meta, id);
    return meta;
  } else { // 需要从半节点扩展为全节点
    struct wormmeta * const meta2 = wormmeta_expand(hmap, meta);
    wormmeta_bm_set(meta2, id); // 在新节点上设置位
    return meta2;
  }
}

// return true if a new node is created
// 如果创建了新节点则返回true
// 在元数据树中触摸(创建或更新)一个节点
  static void
wormmeta_split_touch(struct wormhmap * const hmap, struct kv * const mkey,
    struct wormleaf * const leaf, const u32 alen)
{
  struct wormmeta * meta = wormhmap_get(hmap, mkey); // 查找元数据节点
  if (meta) { // 如果存在
    if (mkey->klen < alen) // 如果是中间节点
      meta = wormmeta_bm_set_helper(hmap, meta, mkey->kv[mkey->klen]); // 设置子节点对应的位
    if (wormmeta_lmost_load(meta) == leaf->next) // 更新最左叶子
      wormmeta_lmost_store(meta, leaf);
    else if (wormmeta_rmost_load(meta) == leaf->prev) // 更新最右叶子
      wormmeta_rmost_store(meta, leaf);
  } else { // 如果不存在，创建新节点
    const u32 bit = (mkey->klen < alen) ? mkey->kv[mkey->klen] : WH_FO;
    meta = wormmeta_alloc(hmap, leaf, mkey, alen, bit);
    debug_assert(meta);
    wormhmap_set(hmap, meta);
  }
}

  // 更新从a1到a2路径上的lpath指针
  static void
wormmeta_lpath_update(struct wormhmap * const hmap, const struct kv * const a1, const struct kv * const a2,
    struct wormleaf * const lpath)
{
  struct kv * const pbuf = hmap->pbuf; // 使用临时缓冲区
  kv_dup2_key(a2, pbuf); // 复制a2的键

  // 只需要更新a2自己的分支
  u32 i = kv_key_lcp(a1, a2) + 1; // 找到分叉点
  debug_assert(i <= pbuf->klen);
  wormhole_prefix(pbuf, i); // 设置前缀
  while (i < a2->klen) { // 遍历a2独有的前缀路径
    debug_assert(i <= hmap->maxplen);
    struct wormmeta * const meta = wormhmap_get(hmap, pbuf); // 查找元数据节点
    debug_assert(meta);
    wormmeta_lpath_store(meta, lpath); // 更新lpath

    i++;
    wormhole_prefix_inc1(pbuf); // 前缀增长
  }
}

// for leaf1, a leaf2 is already linked at its right side.
// this function updates the meta-map by moving leaf1 and hooking leaf2 at correct positions
// 对于leaf1，一个leaf2已经链接在它的右侧。
// 此函数通过移动leaf1和在正确位置挂接leaf2来更新元数据映射。
  static void
wormmeta_split(struct wormhmap * const hmap, struct wormleaf * const leaf,
    struct kv * const mkey)
{
  // 左分支
  struct wormleaf * const prev = leaf->prev;
  struct wormleaf * const next = leaf->next;
  u32 i = next ? kv_key_lcp(prev->anchor, next->anchor) : 0; // 找到与邻居的分叉点
  const u32 alen = leaf->anchor->klen; // 新叶子的锚点键长度

  // 保存键长
  const u32 mklen = mkey->klen;
  wormhole_prefix(mkey, i); // 设置前缀
  do {
    wormmeta_split_touch(hmap, mkey, leaf, alen); // 触摸路径上的节点
    if (i >= alen)
      break;
    i++;
    wormhole_prefix_inc1(mkey); // 前缀增长
  } while (true);

  // 调整maxplen; i是最后一次_touch()的前缀长度
  if (i > hmap->maxplen)
    hmap->maxplen = i;
  debug_assert(i <= UINT16_MAX);

  // 恢复键长
  mkey->klen = mklen;

  if (next) // 如果有后继节点
    wormmeta_lpath_update(hmap, leaf->anchor, next->anchor, leaf); // 更新lpath
}

// all locks will be released before returning
// 所有锁在返回前都会被释放
  static bool
wormhole_split_meta(struct wormref * const ref, struct wormleaf * const leaf2)
{
  struct kv * const mkey = wormhole_alloc_mkey(leaf2->anchor->klen); // 为新锚点分配元数据键
  if (unlikely(mkey == NULL))
    return false;
  kv_dup2_key(leaf2->anchor, mkey);

  struct wormhole * const map = ref->map;
  // metalock
  wormhmap_lock(map, ref); // 获取元数据锁

  // 检查slab预留
  const bool sr = wormhole_slab_reserve(map, mkey->klen);
  if (unlikely(!sr)) {
    wormhmap_unlock(map);
    wormhole_free_mkey(mkey);
    return false;
  }

  struct wormhmap * const hmap0 = wormhmap_load(map); // 当前哈希表
  struct wormhmap * const hmap1 = wormhmap_switch(map, hmap0); // 备用哈希表

  // 链接
  struct wormleaf * const leaf1 = leaf2->prev;
  leaf1->next = leaf2;
  if (leaf2->next)
    leaf2->next->prev = leaf2;

  // 更新版本
  const u64 v1 = wormhmap_version_load(hmap0) + 1;
  wormleaf_version_store(leaf1, v1);
  wormleaf_version_store(leaf2, v1);
  wormhmap_version_store(hmap1, v1);

  wormmeta_split(hmap1, leaf2, mkey); // 在备用哈希表上分裂元数据

  qsbr_update(&ref->qref, v1); // 更新QSBR

  // 切换哈希表
  wormhmap_store(map, hmap1);

  wormleaf_unlock_write(leaf1); // 解锁
  wormleaf_unlock_write(leaf2);

  qsbr_wait(map->qsbr, v1); // 等待所有线程看到新版本

  wormmeta_split(hmap0, leaf2, mkey); // 在旧哈希表上分裂元数据

  wormhmap_unlock(map); // 解锁元数据

  if (mkey->refcnt == 0) // 这是有可能的
    wormhole_free_mkey(mkey);
  return true;
}

// all locks (metalock + leaflocks) will be released before returning
// leaf1->lock (write) is already taken
// 所有锁（元数据锁+叶子锁）在返回前都会被释放
// leaf1的写锁已被获取
  static bool
wormhole_split_insert(struct wormref * const ref, struct wormleaf * const leaf1,
    struct kv * const new)
{
  struct wormleaf * const leaf2 = wormhole_split_leaf(ref->map, leaf1, new); // 分裂叶子
  if (unlikely(leaf2 == NULL)) { // 分裂失败
    wormleaf_unlock_write(leaf1);
    return false;
  }

  rwlock_lock_write(&(leaf2->leaflock)); // 锁住新叶子
  const bool rsm = wormhole_split_meta(ref, leaf2); // 分裂元数据
  if (unlikely(!rsm)) { // 元数据分裂失败
    // 撤销插入和合并；释放leaf2
    wormleaf_split_undo(ref->map, leaf1, leaf2, new);
    wormleaf_unlock_write(leaf1);
  }
  return rsm;
}

  // 非线程安全的元数据分裂
  static bool
whunsafe_split_meta(struct wormhole * const map, struct wormleaf * const leaf2)
{
  struct kv * const mkey = wormhole_alloc_mkey(leaf2->anchor->klen);
  if (unlikely(mkey == NULL))
    return false;
  kv_dup2_key(leaf2->anchor, mkey);

  const bool sr = wormhole_slab_reserve(map, mkey->klen);
  if (unlikely(!sr)) {
    wormhmap_unlock(map);
    wormhole_free_mkey(mkey);
    return false;
  }

  // 链接
  leaf2->prev->next = leaf2;
  if (leaf2->next)
    leaf2->next->prev = leaf2;

  for (u32 i = 0; i < 2; i++) // 在所有哈希表上分裂
    if (map->hmap2[i].pmap)
      wormmeta_split(&(map->hmap2[i]), leaf2, mkey);
  if (mkey->refcnt == 0) // 这是有可能的
    wormhole_free_mkey(mkey);
  return true;
}

  // 非线程安全的插入并分裂
  static bool
whunsafe_split_insert(struct wormhole * const map, struct wormleaf * const leaf1,
    struct kv * const new)
{
  struct wormleaf * const leaf2 = wormhole_split_leaf(map, leaf1, new); // 分裂叶子
  if (unlikely(leaf2 == NULL))
    return false;

  const bool rsm = whunsafe_split_meta(map, leaf2); // 分裂元数据
  if (unlikely(!rsm))  // 撤销插入，合并，释放leaf2
    wormleaf_split_undo(map, leaf1, leaf2, new);

  return rsm;
}
// }}} meta-split

// meta-merge {{{
// 元数据合并

// now it only contains one bit
// 现在它只包含一个位
// 将全节点收缩为半节点
  static struct wormmeta *
wormmeta_shrink(struct wormhmap * const hmap, struct wormmeta * const meta2)
{
  debug_assert(wormmeta_bitmin_load(meta2) == wormmeta_bitmax_load(meta2)); // 确认只有一个位
  struct wormmeta * const meta1 = slab_alloc_unsafe(hmap->slab1); // 从普通slab分配
  if (meta1 == NULL)
    return NULL;

  memcpy(meta1, meta2, sizeof(*meta1)); // 复制内容

  wormhmap_replace(hmap, meta2, meta1); // 替换
  slab_free_unsafe(hmap->slab2, meta2); // 释放
  return meta1;
}

  // 清除元数据位图位的辅助函数
  static void
wormmeta_bm_clear_helper(struct wormhmap * const hmap, struct wormmeta * const meta, const u32 id)
{
  if (wormmeta_bitmin_load(meta) == wormmeta_bitmax_load(meta)) { // 如果是半节点
    debug_assert(wormmeta_bitmin_load(meta) < WH_FO);
    wormmeta_bitmin_store(meta, WH_FO); // 清空
    wormmeta_bitmax_store(meta, WH_FO);
  } else { // 如果是全节点
    wormmeta_bm_clear(meta, id); // 清除位
    if (wormmeta_bitmin_load(meta) == wormmeta_bitmax_load(meta)) // 如果清除后变成半节点
      wormmeta_shrink(hmap, meta); // 收缩
  }
}

// all locks held
// 所有锁都已持有
  static void
wormmeta_merge(struct wormhmap * const hmap, struct wormleaf * const leaf)
{
  // leaf->next是合并后的新next，可以为NULL
  struct wormleaf * const prev = leaf->prev;
  struct wormleaf * const next = leaf->next;
  struct kv * const pbuf = hmap->pbuf;
  kv_dup2_key(leaf->anchor, pbuf); // 复制被删除叶子的锚点键
  u32 i = (prev && next) ? kv_key_lcp(prev->anchor, next->anchor) : 0; // 找到分叉点
  const u32 alen = leaf->anchor->klen;
  wormhole_prefix(pbuf, i);
  struct wormmeta * parent = NULL;
  do {
    debug_assert(i <= hmap->maxplen);
    struct wormmeta * meta = wormhmap_get(hmap, pbuf); // 查找元数据
    if (wormmeta_lmost_load(meta) == wormmeta_rmost_load(meta)) { // 如果是单孩子节点
      debug_assert(wormmeta_lmost_load(meta) == leaf);
      const u32 bitmin = wormmeta_bitmin_load(meta);
      wormhmap_del(hmap, meta); // 删除该元数据节点
      wormmeta_free(hmap, meta);
      if (parent) { // 从父节点中清除对应的位
        wormmeta_bm_clear_helper(hmap, parent, pbuf->kv[i-1]);
        parent = NULL;
      }
      if (bitmin == WH_FO) // 如果没有孩子了
        break;
    } else { // 调整lmost/rmost
      if (wormmeta_lmost_load(meta) == leaf)
        wormmeta_lmost_store(meta, next);
      else if (wormmeta_rmost_load(meta) == leaf)
        wormmeta_rmost_store(meta, prev);
      parent = meta;
    }

    if (i >= alen)
      break;
    i++;
    wormhole_prefix_inc1(pbuf); // 前缀增长
  } while (true);

  if (next) // 更新lpath
    wormmeta_lpath_update(hmap, leaf->anchor, next->anchor, prev);
}

// all locks (metalock + two leaflock) will be released before returning
// merge leaf2 to leaf1, removing all metadata to leaf2 and leaf2 itself
// 所有锁（元数据锁+两个叶子锁）在返回前都会被释放
// 将leaf2合并到leaf1，移除所有指向leaf2的元数据和leaf2本身
  static void
wormhole_meta_merge(struct wormref * const ref, struct wormleaf * const leaf1,
    struct wormleaf * const leaf2, const bool unlock_leaf1)
{
  debug_assert(leaf1->next == leaf2);
  debug_assert(leaf2->prev == leaf1);
  struct wormhole * const map = ref->map;

  wormhmap_lock(map, ref); // 获取元数据锁

  struct wormhmap * const hmap0 = wormhmap_load(map);
  struct wormhmap * const hmap1 = wormhmap_switch(map, hmap0);
  const u64 v1 = wormhmap_version_load(hmap0) + 1;

  leaf1->next = leaf2->next; // 更新链表
  if (leaf2->next)
    leaf2->next->prev = leaf1;

  wormleaf_version_store(leaf1, v1); // 更新版本
  wormleaf_version_store(leaf2, v1);
  wormhmap_version_store(hmap1, v1);

  wormmeta_merge(hmap1, leaf2); // 在备用哈希表上合并元数据

  qsbr_update(&ref->qref, v1); // 更新QSBR

  // 切换哈希表
  wormhmap_store(map, hmap1);

  if (unlock_leaf1)
    wormleaf_unlock_write(leaf1); // 解锁
  wormleaf_unlock_write(leaf2);

  qsbr_wait(map->qsbr, v1); // 等待

  wormmeta_merge(hmap0, leaf2); // 在旧哈希表上合并元数据
  // leaf2现在可以安全地移除了
  wormleaf_free(map->slab_leaf, leaf2);
  wormhmap_unlock(map);
}

// caller must acquire leaf->wlock and next->wlock
// all locks will be released when this function returns
// 调用者必须获取leaf和next的写锁
// 所有锁在此函数返回时都会被释放
  static bool
wormhole_meta_leaf_merge(struct wormref * const ref, struct wormleaf * const leaf)
{
  struct wormleaf * const next = leaf->next;
  debug_assert(next);

  // 再次检查
  if ((leaf->nr_keys + next->nr_keys) <= WH_KPN) {
    if (wormleaf_merge(leaf, next)) { // 合并叶子
      wormhole_meta_merge(ref, leaf, next, true); // 合并元数据
      return true;
    }
  }
  // 合并失败但没关系
  wormleaf_unlock_write(leaf);
  wormleaf_unlock_write(next);
  return false;
}

  // 非线程安全的元数据和叶子合并
  static void
whunsafe_meta_leaf_merge(struct wormhole * const map, struct wormleaf * const leaf1,
    struct wormleaf * const leaf2)
{
  debug_assert(leaf1->next == leaf2);
  debug_assert(leaf2->prev == leaf1);
  if (!wormleaf_merge(leaf1, leaf2)) // 合并叶子
    return;

  leaf1->next = leaf2->next; // 更新链表
  if (leaf2->next)
    leaf2->next->prev = leaf1;
  for (u32 i = 0; i < 2; i++) // 在所有哈希表上合并元数据
    if (map->hmap2[i].pmap)
      wormmeta_merge(&(map->hmap2[i]), leaf2);
  wormleaf_free(map->slab_leaf, leaf2); // 释放叶子
}
// }}} meta-merge

// put {{{
// 插入/更新操作

  // 插入或更新一个键值对
  bool
wormhole_put(struct wormref * const ref, struct kv * const kv)
{
  // 我们总是在SET时分配一个新项
  // 未来的优化可能会执行就地更新
  struct wormhole * const map = ref->map;
  struct kv * const new = map->mm.in(kv, map->mm.priv); // 通过内存管理器复制输入kv
  if (unlikely(new == NULL))
    return false;
  const struct kref kref = kv_kref(new); // 创建键引用

  struct wormleaf * const leaf = wormhole_jump_leaf_write(ref, &kref); // 跳转到叶子并加写锁
  // 更新
  const u32 im = wormleaf_match_hs(leaf, &kref); // 查找键
  if (im < WH_KPN) { // 如果找到，则为更新操作
    struct kv * const old = wormleaf_update(leaf, im, new); // 更新
    wormleaf_unlock_write(leaf); // 解锁
    map->mm.free(old, map->mm.priv); // 释放旧值
    return true;
  }

  // 插入
  if (likely(leaf->nr_keys < WH_KPN)) { // 如果叶子未满，直接插入
    wormleaf_insert(leaf, new);
    wormleaf_unlock_write(leaf);
    return true;
  }

  // split_insert会改变hmap
  // 所有锁都应在wormhole_split_insert()中释放
  const bool rsi = wormhole_split_insert(ref, leaf, new); // 叶子已满，需要分裂
  if (!rsi) // 如果分裂失败
    map->mm.free(new, map->mm.priv); // 释放新分配的kv
  return rsi;
}

  // 线程安全的put
  bool
whsafe_put(struct wormref * const ref, struct kv * const kv)
{
  wormhole_resume(ref);
  const bool r = wormhole_put(ref, kv);
  wormhole_park(ref);
  return r;
}

  // 非线程安全的put
  bool
whunsafe_put(struct wormhole * const map, struct kv * const kv)
{
  struct kv * const new = map->mm.in(kv, map->mm.priv);
  if (unlikely(new == NULL))
    return false;
  const struct kref kref = kv_kref(new);

  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, &kref);
  // 更新
  const u32 im = wormleaf_match_hs(leaf, &kref);
  if (im < WH_KPN) { // 覆盖
    struct kv * const old = wormleaf_update(leaf, im, new);
    map->mm.free(old, map->mm.priv);
    return true;
  }

  // 插入
  if (likely(leaf->nr_keys < WH_KPN)) { // 直接插入
    wormleaf_insert(leaf, new);
    return true;
  }

  // split_insert会改变hmap
  const bool rsi = whunsafe_split_insert(map, leaf, new);
  if (!rsi)
    map->mm.free(new, map->mm.priv);
  return rsi;
}

  // 合并操作 (Read-Modify-Write)
  bool
wormhole_merge(struct wormref * const ref, const struct kref * const kref,
    kv_merge_func uf, void * const priv)
{
  struct wormhole * const map = ref->map;
  struct wormleaf * const leaf = wormhole_jump_leaf_write(ref, kref); // 跳转并加写锁
  // 更新
  const u32 im = wormleaf_match_hs(leaf, kref); // 查找
  if (im < WH_KPN) { // 如果找到
    struct kv * const kv0 = wormleaf_kv_at_ih(leaf, im); // 获取旧值
    struct kv * const kv = uf(kv0, priv); // 调用用户提供的合并函数
    if ((kv == kv0) || (kv == NULL)) { // 如果没有替换
      wormleaf_unlock_write(leaf);
      return true;
    }

    struct kv * const new = map->mm.in(kv, map->mm.priv); // 复制新值
    if (unlikely(new == NULL)) { // mm错误
      wormleaf_unlock_write(leaf);
      return false;
    }

    struct kv * const old = wormleaf_update(leaf, im, new); // 更新
    wormleaf_unlock_write(leaf);
    map->mm.free(old, map->mm.priv); // 释放旧值
    return true;
  }

  struct kv * const kv = uf(NULL, priv); // 如果未找到，传入NULL调用合并函数
  if (kv == NULL) { // 如果无需插入
    wormleaf_unlock_write(leaf);
    return true;
  }

  struct kv * const new = map->mm.in(kv, map->mm.priv); // 复制新值
  if (unlikely(new == NULL)) { // mm错误
    wormleaf_unlock_write(leaf);
    return false;
  }

  // 插入
  if (likely(leaf->nr_keys < WH_KPN)) { // 如果叶子未满
    wormleaf_insert(leaf, new);
    wormleaf_unlock_write(leaf);
    return true;
  }

  // split_insert会改变hmap
  // 所有锁都应在wormhole_split_insert()中释放
  const bool rsi = wormhole_split_insert(ref, leaf, new); // 分裂并插入
  if (!rsi)
    map->mm.free(new, map->mm.priv);
  return rsi;
}

  // 线程安全的merge
  bool
whsafe_merge(struct wormref * const ref, const struct kref * const kref,
    kv_merge_func uf, void * const priv)
{
  wormhole_resume(ref);
  const bool r = wormhole_merge(ref, kref, uf, priv);
  wormhole_park(ref);
  return r;
}

  // 非线程安全的merge
  bool
whunsafe_merge(struct wormhole * const map, const struct kref * const kref,
    kv_merge_func uf, void * const priv)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, kref);
  // 更新
  const u32 im = wormleaf_match_hs(leaf, kref);
  if (im < WH_KPN) { // 更新
    struct kv * const kv0 = wormleaf_kv_at_ih(leaf, im);
    struct kv * const kv = uf(kv0, priv);
    if ((kv == kv0) || (kv == NULL))
      return true;

    struct kv * const new = map->mm.in(kv, map->mm.priv);
    if (unlikely(new == NULL))
      return false;

    struct kv * const old = wormleaf_update(leaf, im, new);
    map->mm.free(old, map->mm.priv);
    return true;
  }

  struct kv * const kv = uf(NULL, priv);
  if (kv == NULL) // 无需插入
    return true;

  struct kv * const new = map->mm.in(kv, map->mm.priv);
  if (unlikely(new == NULL)) // mm错误
    return false;

  // 插入
  if (likely(leaf->nr_keys < WH_KPN)) { // 直接插入
    wormleaf_insert(leaf, new);
    return true;
  }

  // split_insert会改变hmap
  const bool rsi = whunsafe_split_insert(map, leaf, new);
  if (!rsi)
    map->mm.free(new, map->mm.priv);
  return rsi;
}
// }}} put

// inplace {{{
// 就地操作

  // 就地读
  bool
wormhole_inpr(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv)
{
  struct wormleaf * const leaf = wormhole_jump_leaf_read(ref, key); // 跳转并加读锁
  const u32 im = wormleaf_match_hs(leaf, key);
  if (im < WH_KPN) { // 找到
    uf(wormleaf_kv_at_ih(leaf, im), priv); // 调用用户函数
    wormleaf_unlock_read(leaf);
    return true;
  } else { // 未找到
    uf(NULL, priv); // 传入NULL调用用户函数
    wormleaf_unlock_read(leaf);
    return false;
  }
}

  // 就地写
  bool
wormhole_inpw(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv)
{
  struct wormleaf * const leaf = wormhole_jump_leaf_write(ref, key); // 跳转并加写锁
  const u32 im = wormleaf_match_hs(leaf, key);
  if (im < WH_KPN) { // 找到
    uf(wormleaf_kv_at_ih(leaf, im), priv);
    wormleaf_unlock_write(leaf);
    return true;
  } else { // 未找到
    uf(NULL, priv);
    wormleaf_unlock_write(leaf);
    return false;
  }
}

  // 线程安全的就地读
  bool
whsafe_inpr(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv)
{
  wormhole_resume(ref);
  const bool r = wormhole_inpr(ref, key, uf, priv);
  wormhole_park(ref);
  return r;
}

  // 线程安全的就地写
  bool
whsafe_inpw(struct wormref * const ref, const struct kref * const key,
    kv_inp_func uf, void * const priv)
{
  wormhole_resume(ref);
  const bool r = wormhole_inpw(ref, key, uf, priv);
  wormhole_park(ref);
  return r;
}

  // 非线程安全的就地操作
  bool
whunsafe_inp(struct wormhole * const map, const struct kref * const key,
    kv_inp_func uf, void * const priv)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, key);
  const u32 im = wormleaf_match_hs(leaf, key);
  if (im < WH_KPN) { // 覆盖
    uf(wormleaf_kv_at_ih(leaf, im), priv);
    return true;
  } else {
    uf(NULL, priv);
    return false;
  }
}
// }}} put

// del {{{
// 删除操作

  // 删除后尝试与后继节点合并
  static void
wormhole_del_try_merge(struct wormref * const ref, struct wormleaf * const leaf)
{
  struct wormleaf * const next = leaf->next;
  // 如果当前叶子为空，或者与后继节点合并后大小满足阈值
  if (next && ((leaf->nr_keys == 0) || ((leaf->nr_keys + next->nr_keys) < WH_KPN_MRG))) {
    // 尝试合并，可能会因为加锁后大小变化而失败
    wormleaf_lock_write(next, ref); // 锁住后继节点
    (void)wormhole_meta_leaf_merge(ref, leaf);
    // 锁已在函数内释放；立即返回
  } else {
    wormleaf_unlock_write(leaf); // 否则只解锁当前叶子
  }
}

  // 删除一个键
  bool
wormhole_del(struct wormref * const ref, const struct kref * const key)
{
  struct wormleaf * const leaf = wormhole_jump_leaf_write(ref, key); // 跳转并加写锁
  const u32 im = wormleaf_match_hs(leaf, key); // 查找
  if (im < WH_KPN) { // 找到
    struct kv * const kv = wormleaf_remove_ih(leaf, im); // 移除
    wormhole_del_try_merge(ref, leaf); // 尝试合并
    debug_assert(kv);
    // 释放锁后释放内存
    struct wormhole * const map = ref->map;
    map->mm.free(kv, map->mm.priv);
    return true;
  } else { // 未找到
    wormleaf_unlock_write(leaf);
    return false;
  }
}

  // 线程安全的删除
  bool
whsafe_del(struct wormref * const ref, const struct kref * const key)
{
  wormhole_resume(ref);
  const bool r = wormhole_del(ref, key);
  wormhole_park(ref);
  return r;
}

  // 非线程安全的删除后尝试合并
  static void
whunsafe_del_try_merge(struct wormhole * const map, struct wormleaf * const leaf)
{
  const u32 n0 = leaf->prev ? leaf->prev->nr_keys : WH_KPN;
  const u32 n1 = leaf->nr_keys;
  const u32 n2 = leaf->next ? leaf->next->nr_keys : WH_KPN;

  if ((leaf->prev && (n1 == 0)) || ((n0 + n1) < WH_KPN_MRG)) { // 尝试与前驱合并
    whunsafe_meta_leaf_merge(map, leaf->prev, leaf);
  } else if ((leaf->next && (n1 == 0)) || ((n1 + n2) < WH_KPN_MRG)) { // 尝试与后继合并
    whunsafe_meta_leaf_merge(map, leaf, leaf->next);
  }
}

  // 非线程安全的删除
  bool
whunsafe_del(struct wormhole * const map, const struct kref * const key)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(map->hmap, key);
  const u32 im = wormleaf_match_hs(leaf, key);
  if (im < WH_KPN) { // 找到
    struct kv * const kv = wormleaf_remove_ih(leaf, im);
    debug_assert(kv);

    whunsafe_del_try_merge(map, leaf); // 尝试合并
    map->mm.free(kv, map->mm.priv);
    return true;
  }
  return false;
}

  // 范围删除
  u64
wormhole_delr(struct wormref * const ref, const struct kref * const start,
    const struct kref * const end)
{
  struct wormleaf * const leafa = wormhole_jump_leaf_write(ref, start); // 跳转到起始叶子
  wormleaf_sync_sorted(leafa); // 排序
  const u32 ia = wormleaf_seek(leafa, start); // 找到起始位置
  const u32 iaz = end ? wormleaf_seek_end(leafa, end) : leafa->nr_keys; // 找到结束位置
  if (iaz < ia) { // 如果end < start，什么都不做
    wormleaf_unlock_write(leafa);
    return 0;
  }
  u64 ndel = iaz - ia; // 已删除数量
  struct wormhole * const map = ref->map;
  wormleaf_delete_range(map, leafa, ia, iaz); // 在第一个叶子中删除
  if (leafa->nr_keys > ia) { // 如果范围没有超出第一个叶子
    wormhole_del_try_merge(ref, leafa); // 尝试合并
    return ndel;
  }

  while (leafa->next) { // 遍历后续叶子
    struct wormleaf * const leafx = leafa->next;
    wormleaf_lock_write(leafx, ref); // 锁住下一个叶子
    // 两个叶子节点已锁定
    wormleaf_sync_sorted(leafx);
    const u32 iz = end ? wormleaf_seek_end(leafx, end) : leafx->nr_keys; // 找到结束位置
    ndel += iz;
    wormleaf_delete_range(map, leafx, 0, iz); // 删除
    if (leafx->nr_keys == 0) { // 如果整个叶子都被删除了
      // 必须持有leaf1的锁才能进行下一次迭代
      wormhole_meta_merge(ref, leafa, leafx, false); // 合并，但不解锁leafa
    } else { // 如果部分删除，说明范围已结束
      (void)wormhole_meta_leaf_merge(ref, leafa); // 尝试合并leafa和它的新后继
      return ndel;
    }
  }
  wormleaf_unlock_write(leafa); // 解锁最后一个叶子
  return ndel;
}

  u64
whsafe_delr(struct wormref * const ref, const struct kref * const start,
    const struct kref * const end)
{
  wormhole_resume(ref);
  const u64 ret = wormhole_delr(ref, start, end);
  wormhole_park(ref);
  return ret;
}

  u64
whunsafe_delr(struct wormhole * const map, const struct kref * const start,
    const struct kref * const end)
{
  // first leaf
  struct wormhmap * const hmap = map->hmap;
  struct wormleaf * const leafa = wormhole_jump_leaf(hmap, start);
  wormleaf_sync_sorted(leafa);
  // last leaf
  struct wormleaf * const leafz = end ? wormhole_jump_leaf(hmap, end) : NULL;

  // select start/end on leafa
  const u32 ia = wormleaf_seek(leafa, start);
  const u32 iaz = end ? wormleaf_seek_end(leafa, end) : leafa->nr_keys;
  if (iaz < ia)
    return 0;

  wormleaf_delete_range(map, leafa, ia, iaz);
  u64 ndel = iaz - ia;

  if (leafa == leafz) { // one node only
    whunsafe_del_try_merge(map, leafa);
    return ndel;
  }

  // 0 or more nodes between leafa and leafz
  while (leafa->next != leafz) {
    struct wormleaf * const leafx = leafa->next;
    ndel += leafx->nr_keys;
    for (u32 i = 0; i < leafx->nr_keys; i++)
      map->mm.free(wormleaf_kv_at_is(leafx, i), map->mm.priv);
    leafx->nr_keys = 0;
    leafx->nr_sorted = 0;
    whunsafe_meta_leaf_merge(map, leafa, leafx);
  }
  // delete the smaller keys in leafz
  if (leafz) {
    wormleaf_sync_sorted(leafz);
    const u32 iz = wormleaf_seek_end(leafz, end);
    wormleaf_delete_range(map, leafz, 0, iz);
    ndel += iz;
    whunsafe_del_try_merge(map, leafa);
  }
  return ndel;
}
// }}} del

// iter {{{
// safe iter: safe sort with read-lock acquired
// unsafe iter: allow concurrent seek/skip
  static void
wormhole_iter_leaf_sync_sorted(struct wormleaf * const leaf)
{
  if (unlikely(leaf->nr_keys != leaf->nr_sorted)) {
    spinlock_lock(&(leaf->sortlock));
    wormleaf_sync_sorted(leaf);
    spinlock_unlock(&(leaf->sortlock));
  }
}

  struct wormhole_iter *
wormhole_iter_create(struct wormref * const ref)
{
  struct wormhole_iter * const iter = malloc(sizeof(*iter));
  if (iter == NULL)
    return NULL;
  iter->ref = ref;
  iter->map = ref->map;
  iter->leaf = NULL;
  iter->is = 0;
  return iter;
}

  static void
wormhole_iter_fix(struct wormhole_iter * const iter)
{
  if (!wormhole_iter_valid(iter))
    return;

  while (unlikely(iter->is >= iter->leaf->nr_sorted)) {
    struct wormleaf * const next = iter->leaf->next;
    if (likely(next != NULL)) {
      struct wormref * const ref = iter->ref;
      wormleaf_lock_read(next, ref);
      wormleaf_unlock_read(iter->leaf);

      wormhole_iter_leaf_sync_sorted(next);
    } else {
      wormleaf_unlock_read(iter->leaf);
    }
    iter->leaf = next;
    iter->is = 0;
    if (!wormhole_iter_valid(iter))
      return;
  }
}

  void
wormhole_iter_seek(struct wormhole_iter * const iter, const struct kref * const key)
{
  debug_assert(key);
  if (iter->leaf)
    wormleaf_unlock_read(iter->leaf);

  struct wormleaf * const leaf = wormhole_jump_leaf_read(iter->ref, key);
  wormhole_iter_leaf_sync_sorted(leaf);

  iter->leaf = leaf;
  iter->is = wormleaf_seek(leaf, key);
  wormhole_iter_fix(iter);
}

  void
whsafe_iter_seek(struct wormhole_iter * const iter, const struct kref * const key)
{
  wormhole_resume(iter->ref);
  wormhole_iter_seek(iter, key);
}

  bool
wormhole_iter_valid(struct wormhole_iter * const iter)
{
  return iter->leaf != NULL;
}

  static struct kv *
wormhole_iter_current(struct wormhole_iter * const iter)
{
  if (wormhole_iter_valid(iter)) {
    debug_assert(iter->is < iter->leaf->nr_sorted);
    struct kv * const kv = wormleaf_kv_at_is(iter->leaf, iter->is);
    return kv;
  }
  return NULL;
}

  struct kv *
wormhole_iter_peek(struct wormhole_iter * const iter, struct kv * const out)
{
  struct kv * const kv = wormhole_iter_current(iter);
  if (kv) {
    struct kv * const ret = iter->map->mm.out(kv, out);
    return ret;
  }
  return NULL;
}

  bool
wormhole_iter_kref(struct wormhole_iter * const iter, struct kref * const kref)
{
  struct kv * const kv = wormhole_iter_current(iter);
  if (kv) {
    kref_ref_kv(kref, kv);
    return true;
  }
  return false;
}

  bool
wormhole_iter_kvref(struct wormhole_iter * const iter, struct kvref * const kvref)
{
  struct kv * const kv = wormhole_iter_current(iter);
  if (kv) {
    kvref_ref_kv(kvref, kv);
    return true;
  }
  return false;
}

  void
wormhole_iter_skip1(struct wormhole_iter * const iter)
{
  if (wormhole_iter_valid(iter)) {
    iter->is++;
    wormhole_iter_fix(iter);
  }
}

  void
wormhole_iter_skip(struct wormhole_iter * const iter, const u32 nr)
{
  u32 todo = nr;
  while (todo && wormhole_iter_valid(iter)) {
    const u32 cap = iter->leaf->nr_sorted - iter->is;
    const u32 nskip = (cap < todo) ? cap : todo;
    iter->is += nskip;
    wormhole_iter_fix(iter);
    todo -= nskip;
  }
}

  struct kv *
wormhole_iter_next(struct wormhole_iter * const iter, struct kv * const out)
{
  struct kv * const ret = wormhole_iter_peek(iter, out);
  wormhole_iter_skip1(iter);
  return ret;
}

  bool
wormhole_iter_inp(struct wormhole_iter * const iter, kv_inp_func uf, void * const priv)
{
  struct kv * const kv = wormhole_iter_current(iter);
  uf(kv, priv); // call uf even if (kv == NULL)
  return kv != NULL;
}

  void
wormhole_iter_park(struct wormhole_iter * const iter)
{
  if (iter->leaf) {
    wormleaf_unlock_read(iter->leaf);
    iter->leaf = NULL;
  }
}

  void
whsafe_iter_park(struct wormhole_iter * const iter)
{
  wormhole_iter_park(iter);
  wormhole_park(iter->ref);
}

  void
wormhole_iter_destroy(struct wormhole_iter * const iter)
{
  if (iter->leaf)
    wormleaf_unlock_read(iter->leaf);
  free(iter);
}

  void
whsafe_iter_destroy(struct wormhole_iter * const iter)
{
  wormhole_park(iter->ref);
  wormhole_iter_destroy(iter);
}
// }}} iter

// unsafe iter {{{
  struct wormhole_iter *
whunsafe_iter_create(struct wormhole * const map)
{
  struct wormhole_iter * const iter = malloc(sizeof(*iter));
  if (iter == NULL)
    return NULL;
  iter->ref = NULL;
  iter->map = map;
  iter->leaf = NULL;
  iter->is = 0;
  whunsafe_iter_seek(iter, kref_null());
  return iter;
}

  static void
whunsafe_iter_fix(struct wormhole_iter * const iter)
{
  if (!wormhole_iter_valid(iter))
    return;

  while (unlikely(iter->is >= iter->leaf->nr_sorted)) {
    struct wormleaf * const next = iter->leaf->next;
    if (likely(next != NULL))
      wormhole_iter_leaf_sync_sorted(next);
    iter->leaf = next;
    iter->is = 0;
    if (!wormhole_iter_valid(iter))
      return;
  }
}

  void
whunsafe_iter_seek(struct wormhole_iter * const iter, const struct kref * const key)
{
  struct wormleaf * const leaf = wormhole_jump_leaf(iter->map->hmap, key);
  wormhole_iter_leaf_sync_sorted(leaf);

  iter->leaf = leaf;
  iter->is = wormleaf_seek(leaf, key);
  whunsafe_iter_fix(iter);
}

  void
whunsafe_iter_skip1(struct wormhole_iter * const iter)
{
  if (wormhole_iter_valid(iter)) {
    iter->is++;
    whunsafe_iter_fix(iter);
  }
}

  void
whunsafe_iter_skip(struct wormhole_iter * const iter, const u32 nr)
{
  u32 todo = nr;
  while (todo && wormhole_iter_valid(iter)) {
    const u32 cap = iter->leaf->nr_sorted - iter->is;
    const u32 nskip = (cap < todo) ? cap : todo;
    iter->is += nskip;
    whunsafe_iter_fix(iter);
    todo -= nskip;
  }
}

  struct kv *
whunsafe_iter_next(struct wormhole_iter * const iter, struct kv * const out)
{
  struct kv * const ret = wormhole_iter_peek(iter, out);
  whunsafe_iter_skip1(iter);
  return ret;
}

  void
whunsafe_iter_destroy(struct wormhole_iter * const iter)
{
  free(iter);
}
// }}} unsafe iter

// misc {{{
  struct wormref *
wormhole_ref(struct wormhole * const map)
{
  struct wormref * const ref = malloc(sizeof(*ref));
  if (ref == NULL)
    return NULL;
  ref->map = map;
  if (qsbr_register(map->qsbr, &(ref->qref)) == false) {
    free(ref);
    return NULL;
  }
  return ref;
}

  struct wormref *
whsafe_ref(struct wormhole * const map)
{
  struct wormref * const ref = wormhole_ref(map);
  if (ref)
    wormhole_park(ref);
  return ref;
}

  struct wormhole *
wormhole_unref(struct wormref * const ref)
{
  struct wormhole * const map = ref->map;
  qsbr_unregister(map->qsbr, &(ref->qref));
  free(ref);
  return map;
}

  inline void
wormhole_park(struct wormref * const ref)
{
  qsbr_park(&(ref->qref));
}

  inline void
wormhole_resume(struct wormref * const ref)
{
  qsbr_resume(&(ref->qref));
}

  inline void
wormhole_refresh_qstate(struct wormref * const ref)
{
  qsbr_update(&(ref->qref), wormhmap_version_load(wormhmap_load(ref->map)));
}

  static void
wormhole_clean_hmap(struct wormhole * const map)
{
  for (u32 x = 0; x < 2; x++) {
    if (map->hmap2[x].pmap == NULL)
      continue;
    struct wormhmap * const hmap = &(map->hmap2[x]);
    const u64 nr_slots = ((u64)(hmap->mask)) + 1;
    struct wormmbkt * const pmap = hmap->pmap;
    for (u64 s = 0; s < nr_slots; s++) {
      struct wormmbkt * const slot = &(pmap[s]);
      for (u32 i = 0; i < WH_BKT_NR; i++)
        if (slot->e[i])
          wormmeta_keyref_release(slot->e[i]);
    }

    slab_free_all(hmap->slab1);
    slab_free_all(hmap->slab2);
    memset(hmap->pmap, 0, hmap->msize);
    hmap->maxplen = 0;
  }
}

  static void
wormhole_free_leaf_keys(struct wormhole * const map, struct wormleaf * const leaf)
{
  const u32 nr = leaf->nr_keys;
  for (u32 i = 0; i < nr; i++) {
    void * const curr = wormleaf_kv_at_is(leaf, i);
    debug_assert(curr);
    map->mm.free(curr, map->mm.priv);
  }
  wormhole_free_akey(leaf->anchor);
}

  static void
wormhole_clean_helper(struct wormhole * const map)
{
  wormhole_clean_hmap(map);
  for (struct wormleaf * leaf = map->leaf0; leaf; leaf = leaf->next)
    wormhole_free_leaf_keys(map, leaf);
  slab_free_all(map->slab_leaf);
  map->leaf0 = NULL;
}

// unsafe
  void
wormhole_clean(struct wormhole * const map)
{
  wormhole_clean_helper(map);
  wormhole_create_leaf0(map);
}

  void
wormhole_destroy(struct wormhole * const map)
{
  wormhole_clean_helper(map);
  for (u32 i = 0; i < 2; i++) {
    struct wormhmap * const hmap = &map->hmap2[i];
    if (hmap->slab1)
      slab_destroy(hmap->slab1);
    if (hmap->slab2)
      slab_destroy(hmap->slab2);
    wormhmap_deinit(hmap);
  }
  qsbr_destroy(map->qsbr);
  slab_destroy(map->slab_leaf);
  free(map->pbuf);
  free(map);
}

  void
wormhole_fprint(struct wormhole * const map, FILE * const out)
{
  const u64 nr_slab_ul = slab_get_nalloc(map->slab_leaf);
  const u64 nr_slab_um11 = slab_get_nalloc(map->hmap2[0].slab1);
  const u64 nr_slab_um12 = slab_get_nalloc(map->hmap2[0].slab2);
  const u64 nr_slab_um21 = map->hmap2[1].slab1 ? slab_get_nalloc(map->hmap2[1].slab1) : 0;
  const u64 nr_slab_um22 = map->hmap2[1].slab2 ? slab_get_nalloc(map->hmap2[1].slab2) : 0;
  fprintf(out, "%s L-SLAB %lu M-SLAB [0] %lu+%lu [1] %lu+%lu\n",
      __func__, nr_slab_ul, nr_slab_um11, nr_slab_um12, nr_slab_um21, nr_slab_um22);
}
// }}} misc

// api {{{
const struct kvmap_api kvmap_api_wormhole = {
  .hashkey = true,
  .ordered = true,
  .threadsafe = true,
  .unique = true,
  .refpark = true,
  .put = (void *)wormhole_put,
  .get = (void *)wormhole_get,
  .probe = (void *)wormhole_probe,
  .del = (void *)wormhole_del,
  .inpr = (void *)wormhole_inpr,
  .inpw = (void *)wormhole_inpw,
  .merge = (void *)wormhole_merge,
  .delr = (void *)wormhole_delr,
  .iter_create = (void *)wormhole_iter_create,
  .iter_seek = (void *)wormhole_iter_seek,
  .iter_valid = (void *)wormhole_iter_valid,
  .iter_peek = (void *)wormhole_iter_peek,
  .iter_kref = (void *)wormhole_iter_kref,
  .iter_kvref = (void *)wormhole_iter_kvref,
  .iter_skip1 = (void *)wormhole_iter_skip1,
  .iter_skip = (void *)wormhole_iter_skip,
  .iter_next = (void *)wormhole_iter_next,
  .iter_inp = (void *)wormhole_iter_inp,
  .iter_park = (void *)wormhole_iter_park,
  .iter_destroy = (void *)wormhole_iter_destroy,
  .ref = (void *)wormhole_ref,
  .unref = (void *)wormhole_unref,
  .park = (void *)wormhole_park,
  .resume = (void *)wormhole_resume,
  .clean = (void *)wormhole_clean,
  .destroy = (void *)wormhole_destroy,
  .fprint = (void *)wormhole_fprint,
};

const struct kvmap_api kvmap_api_whsafe = {
  .hashkey = true,
  .ordered = true,
  .threadsafe = true,
  .unique = true,
  .put = (void *)whsafe_put,
  .get = (void *)whsafe_get,
  .probe = (void *)whsafe_probe,
  .del = (void *)whsafe_del,
  .inpr = (void *)whsafe_inpr,
  .inpw = (void *)whsafe_inpw,
  .merge = (void *)whsafe_merge,
  .delr = (void *)whsafe_delr,
  .iter_create = (void *)wormhole_iter_create,
  .iter_seek = (void *)whsafe_iter_seek,
  .iter_valid = (void *)wormhole_iter_valid,
  .iter_peek = (void *)wormhole_iter_peek,
  .iter_kref = (void *)wormhole_iter_kref,
  .iter_kvref = (void *)wormhole_iter_kvref,
  .iter_skip1 = (void *)wormhole_iter_skip1,
  .iter_skip = (void *)wormhole_iter_skip,
  .iter_next = (void *)wormhole_iter_next,
  .iter_inp = (void *)wormhole_iter_inp,
  .iter_park = (void *)whsafe_iter_park,
  .iter_destroy = (void *)whsafe_iter_destroy,
  .ref = (void *)whsafe_ref,
  .unref = (void *)wormhole_unref,
  .clean = (void *)wormhole_clean,
  .destroy = (void *)wormhole_destroy,
  .fprint = (void *)wormhole_fprint,
};

const struct kvmap_api kvmap_api_whunsafe = {
  .hashkey = true,
  .ordered = true,
  .unique = true,
  .put = (void *)whunsafe_put,
  .get = (void *)whunsafe_get,
  .probe = (void *)whunsafe_probe,
  .del = (void *)whunsafe_del,
  .inpr = (void *)whunsafe_inp,
  .inpw = (void *)whunsafe_inp,
  .merge = (void *)whunsafe_merge,
  .delr = (void *)whunsafe_delr,
  .iter_create = (void *)whunsafe_iter_create,
  .iter_seek = (void *)whunsafe_iter_seek,
  .iter_valid = (void *)wormhole_iter_valid,
  .iter_peek = (void *)wormhole_iter_peek,
  .iter_kref = (void *)wormhole_iter_kref,
  .iter_kvref = (void *)wormhole_iter_kvref,
  .iter_skip1 = (void *)whunsafe_iter_skip1,
  .iter_skip = (void *)whunsafe_iter_skip,
  .iter_next = (void *)whunsafe_iter_next,
  .iter_inp = (void *)wormhole_iter_inp,
  .iter_destroy = (void *)whunsafe_iter_destroy,
  .clean = (void *)wormhole_clean,
  .destroy = (void *)wormhole_destroy,
  .fprint = (void *)wormhole_fprint,
};

  static void *
wormhole_kvmap_api_create(const char * const name, const struct kvmap_mm * const mm, char ** args)
{
  (void)args;
  if ((!strcmp(name, "wormhole")) || (!strcmp(name, "whsafe"))) {
    return wormhole_create(mm);
  } else if (!strcmp(name, "whunsafe")) {
    return whunsafe_create(mm);
  } else {
    return NULL;
  }
}

__attribute__((constructor))
  static void
wormhole_kvmap_api_init(void)
{
  kvmap_api_register(0, "wormhole", "", wormhole_kvmap_api_create, &kvmap_api_wormhole);
  kvmap_api_register(0, "whsafe", "", wormhole_kvmap_api_create, &kvmap_api_whsafe);
  kvmap_api_register(0, "whunsafe", "", wormhole_kvmap_api_create, &kvmap_api_whunsafe);
}
// }}} api

// vim:fdm=marker
