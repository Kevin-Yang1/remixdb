/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

/**
 * kv.c - 键值对操作实现文件
 *
 * 本文件实现了 RemixDB 中键值对（kv）数据结构的核心操作，包括：
 * - CRC32C 哈希计算
 * - 键值对的创建、复制、比较和排序
 * - 键引用（kref）和键值引用（kvref）操作
 * - kv128 编解码（变长整数编码）
 * - 键值映射（kvmap）API 抽象层
 * - 多路归并迭代器（miter）实现
 *
 * 这些功能为上层的 LSM 树、SST 文件等提供基础的键值对操作支持。
 */
#define _GNU_SOURCE

// headers {{{
#include <assert.h> // static_assert
#include <ctype.h>
#include "lib.h"
#include "ctypes.h"
#include "kv.h"
// }}} headers

// crc32c {{{
// CRC32C 哈希计算函数

/**
 * 计算键的 CRC32C 哈希值
 * @param ptr 数据指针
 * @param len 数据长度
 * @return CRC32C 哈希值（32位）
 */
  inline u32
kv_crc32c(const void * const ptr, u32 len)
{
  return crc32c_inc((const u8 *)ptr, len, KV_CRC32C_SEED);
}

/**
 * 将32位 CRC32C 哈希扩展为64位哈希
 * 高32位为原值的按位取反，低32位为原值
 * @param lo 32位 CRC32C 哈希值
 * @return 64位扩展后的哈希值
 */
  inline u64
kv_crc32c_extend(const u32 lo)
{
  const u64 hi = (u64)(~lo);
  return (hi << 32) | ((u64)lo);
}
// }}} crc32c

// kv {{{
// 键值对（kv）相关操作实现

// size {{{
// 大小计算函数

/**
 * 计算键值对占用的总内存大小
 * @param kv 键值对指针
 * @return 内存大小（包含结构体头部、键和值）
 */
  inline size_t
kv_size(const struct kv * const kv)
{
  return sizeof(*kv) + kv->klen + kv->vlen;
}

/**
 * 计算键值对按指定对齐方式的总内存大小
 * @param kv 键值对指针
 * @param align 对齐字节数（必须是2的幂）
 * @return 对齐后的内存大小
 */
  inline size_t
kv_size_align(const struct kv * const kv, const u64 align)
{
  debug_assert(align && ((align & (align - 1)) == 0));
  return (sizeof(*kv) + kv->klen + kv->vlen + (align - 1)) & (~(align - 1));
}

/**
 * 计算键（不包含值）占用的内存大小
 * @param key 键值对指针（当作键使用）
 * @return 内存大小（包含结构体头部和键）
 */
  inline size_t
key_size(const struct kv *const key)
{
  return sizeof(*key) + key->klen;
}

/**
 * 计算键按指定对齐方式的内存大小
 * @param key 键值对指针（当作键使用）
 * @param align 对齐字节数（必须是2的幂）
 * @return 对齐后的内存大小
 */
  inline size_t
key_size_align(const struct kv *const key, const u64 align)
{
  debug_assert(align && ((align & (align - 1)) == 0));
  return (sizeof(*key) + key->klen + (align - 1)) & (~(align - 1));
}
// }}} size

// construct {{{
// 键值对构造和修改函数

/**
 * 更新键值对的哈希值
 * 基于键的内容计算并设置哈希值
 * @param kv 键值对指针
 */
  inline void
kv_update_hash(struct kv * const kv)
{
  const u32 lo = kv_crc32c((const void *)kv->kv, kv->klen);
  kv->hash = kv_crc32c_extend(lo);
}

/**
 * 重新填充键值对的值部分
 * @param kv 键值对指针
 * @param value 新值的数据指针
 * @param vlen 新值的长度
 */
  inline void
kv_refill_value(struct kv * const kv, const void * const value, const u32 vlen)
{
  debug_assert((vlen == 0) || value);
  memcpy(&(kv->kv[kv->klen]), value, vlen);
  kv->vlen = vlen;
}

/**
 * 重新填充键值对的键和值
 * @param kv 键值对指针
 * @param key 新键的数据指针
 * @param klen 新键的长度
 * @param value 新值的数据指针
 * @param vlen 新值的长度
 */
  inline void
kv_refill(struct kv * const kv, const void * const key, const u32 klen,
    const void * const value, const u32 vlen)
{
  debug_assert(kv);
  kv->klen = klen;
  memcpy(&(kv->kv[0]), key, klen);
  kv_refill_value(kv, value, vlen);
  kv_update_hash(kv);
}

/**
 * 使用字符串键重新填充键值对
 * @param kv 键值对指针
 * @param key 字符串键
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
  inline void
kv_refill_str(struct kv * const kv, const char * const key,
    const void * const value, const u32 vlen)
{
  kv_refill(kv, key, (u32)strlen(key), value, vlen);
}

/**
 * 使用字符串键和字符串值重新填充键值对
 * @param kv 键值对指针
 * @param key 字符串键
 * @param value 字符串值
 */
  inline void
kv_refill_str_str(struct kv * const kv, const char * const key,
    const char * const value)
{
  kv_refill(kv, key, (u32)strlen(key), value, (u32)strlen(value));
}

/**
 * 使用64位整数键重新填充键值对
 * 64位键以大端字节序存储，确保正确的排序
 * @param kv 键值对指针
 * @param key 64位整数键
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
// the u64 key is filled in big-endian byte order for correct ordering
  inline void
kv_refill_u64(struct kv * const kv, const u64 key, const void * const value, const u32 vlen)
{
  kv->klen = sizeof(u64);
  *(u64 *)(kv->kv) = __builtin_bswap64(key); // bswap on little endian
  kv_refill_value(kv, value, vlen);
  kv_update_hash(kv);
}

/**
 * 使用32位十六进制字符串键重新填充键值对
 * @param kv 键值对指针
 * @param hex 32位十六进制数
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
  inline void
kv_refill_hex32(struct kv * const kv, const u32 hex, const void * const value, const u32 vlen)
{
  kv->klen = 8;
  strhex_32(kv->kv, hex);
  kv_refill_value(kv, value, vlen);
  kv_update_hash(kv);
}

/**
 * 使用64位十六进制字符串键重新填充键值对
 * @param kv 键值对指针
 * @param hex 64位十六进制数
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
  inline void
kv_refill_hex64(struct kv * const kv, const u64 hex, const void * const value, const u32 vlen)
{
  kv->klen = 16;
  strhex_64(kv->kv, hex);
  kv_refill_value(kv, value, vlen);
  kv_update_hash(kv);
}

/**
 * 使用64位十六进制字符串键重新填充键值对（指定键长度）
 * 如果指定长度大于16，会用'!'字符填充
 * @param kv 键值对指针
 * @param hex 64位十六进制数
 * @param klen 指定的键长度
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
  inline void
kv_refill_hex64_klen(struct kv * const kv, const u64 hex,
    const u32 klen, const void * const value, const u32 vlen)
{
  strhex_64(kv->kv, hex);
  if (klen > 16) {
    kv->klen = klen;
    memset(kv->kv + 16, '!', klen - 16);
  } else {
    kv->klen = 16;
  }
  kv_refill_value(kv, value, vlen);
  kv_update_hash(kv);
}

/**
 * 从键引用重新填充键值对（仅键，无值）
 * @param kv 键值对指针
 * @param kref 键引用指针
 */
  inline void
kv_refill_kref(struct kv * const kv, const struct kref * const kref)
{
  kv->klen = kref->len;
  kv->vlen = 0;
  kv->hash = kv_crc32c_extend(kref->hash32);
  memmove(kv->kv, kref->ptr, kref->len);
}

/**
 * 从键引用重新填充键值对（键和值）
 * @param kv 键值对指针
 * @param kref 键引用指针
 * @param value 值的数据指针
 * @param vlen 值的长度
 */
  inline void
kv_refill_kref_v(struct kv * const kv, const struct kref * const kref,
    const void * const value, const u32 vlen)
{
  kv->klen = kref->len;
  kv->vlen = vlen;
  kv->hash = kv_crc32c_extend(kref->hash32);
  memmove(kv->kv, kref->ptr, kref->len);
  memcpy(kv->kv + kv->klen, value, vlen);
}

/**
 * 从键值对创建键引用
 * @param key 键值对指针（当作键使用）
 * @return 键引用结构体
 */
  inline struct kref
kv_kref(const struct kv * const key)
{
  return (struct kref){.ptr = key->kv, .len = key->klen, .hash32 = key->hashlo};
}

/**
 * 创建新的键值对
 * @param key 键的数据指针
 * @param klen 键的长度
 * @param value 值的数据指针
 * @param vlen 值的长度
 * @return 新创建的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_create(const void * const key, const u32 klen, const void * const value, const u32 vlen)
{
  struct kv * const kv = malloc(sizeof(*kv) + klen + vlen);
  if (kv)
    kv_refill(kv, key, klen, value, vlen);
  return kv;
}

/**
 * 使用字符串键创建新的键值对
 * @param key 字符串键
 * @param value 值的数据指针
 * @param vlen 值的长度
 * @return 新创建的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_create_str(const char * const key, const void * const value, const u32 vlen)
{
  return kv_create(key, (u32)strlen(key), value, vlen);
}

/**
 * 使用字符串键和字符串值创建新的键值对
 * @param key 字符串键
 * @param value 字符串值
 * @return 新创建的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_create_str_str(const char * const key, const char * const value)
{
  return kv_create(key, (u32)strlen(key), value, (u32)strlen(value));
}

/**
 * 从键引用创建新的键值对
 * @param kref 键引用指针
 * @param value 值的数据指针
 * @param vlen 值的长度
 * @return 新创建的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_create_kref(const struct kref * const kref, const void * const value, const u32 vlen)
{
  return kv_create(kref->ptr, kref->len, value, vlen);
}

// 静态的空键值对，用于初始化
static struct kv __kv_null = {};

/**
 * 初始化空键值对的哈希值
 * 程序启动时自动调用
 */
__attribute__((constructor))
  static void
kv_null_init(void)
{
  kv_update_hash(&__kv_null);
}

/**
 * 获取空键值对的引用
 * @return 空键值对的常量指针
 */
  inline const struct kv *
kv_null(void)
{
  return &__kv_null;
}
// }}} construct

// dup {{{
// 键值对复制函数

/**
 * 复制键值对
 * @param kv 源键值对指针
 * @return 新复制的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_dup(const struct kv * const kv)
{
  if (kv == NULL)
    return NULL;

  const size_t sz = kv_size(kv);
  struct kv * const new = malloc(sz);
  if (new)
    memcpy(new, kv, sz);
  return new;
}

/**
 * 复制键值对的键部分（不包含值）
 * @param kv 源键值对指针
 * @return 新复制的键（vlen=0），失败返回NULL
 */
  inline struct kv *
kv_dup_key(const struct kv * const kv)
{
  if (kv == NULL)
    return NULL;

  const size_t sz = key_size(kv);
  struct kv * const new = malloc(sz);
  if (new) {
    memcpy(new, kv, sz);
    new->vlen = 0;
  }
  return new;
}

/**
 * 复制键值对到指定位置或分配新内存
 * @param from 源键值对指针
 * @param to 目标内存位置（NULL则分配新内存）
 * @return 复制的键值对指针，失败返回NULL
 */
  inline struct kv *
kv_dup2(const struct kv * const from, struct kv * const to)
{
  if (from == NULL)
    return NULL;
  const size_t sz = kv_size(from);
  struct kv * const new = to ? to : malloc(sz);
  if (new)
    memcpy(new, from, sz);
  return new;
}

/**
 * 复制键值对的键部分到指定位置或分配新内存
 * @param from 源键值对指针
 * @param to 目标内存位置（NULL则分配新内存）
 * @return 复制的键（vlen=0），失败返回NULL
 */
  inline struct kv *
kv_dup2_key(const struct kv * const from, struct kv * const to)
{
  if (from == NULL)
    return NULL;
  const size_t sz = key_size(from);
  struct kv * const new = to ? to : malloc(sz);
  if (new) {
    memcpy(new, from, sz);
    new->vlen = 0;
  }
  return new;
}

/**
 * 复制键的前缀部分到指定位置或分配新内存
 * @param from 源键值对指针
 * @param to 目标内存位置（NULL则分配新内存）
 * @param plen 前缀长度
 * @return 复制的键前缀（vlen=0），失败返回NULL
 */
  inline struct kv *
kv_dup2_key_prefix(const struct kv * const from, struct kv * const to, const u32 plen)
{
  if (from == NULL)
    return NULL;
  debug_assert(plen <= from->klen);
  const size_t sz = key_size(from) - from->klen + plen;
  struct kv * const new = to ? to : malloc(sz);
  if (new) {
    new->klen = plen;
    memcpy(new->kv, from->kv, plen);
    new->vlen = 0;
    kv_update_hash(new);
  }
  return new;
}
// }}} dup

// compare {{{
// 键值对比较和匹配函数

/**
 * 比较两个键长度
 * @param len1 第一个键的长度
 * @param len2 第二个键的长度
 * @return <0 如果len1<len2，>0 如果len1>len2，0 如果相等
 */
  static inline int
klen_compare(const u32 len1, const u32 len2)
{
  if (len1 < len2)
    return -1;
  else if (len1 > len2)
    return 1;
  else
    return 0;
}

/**
 * 比较两个键是否完全相同
 * 乐观比较：不检查哈希值，直接比较内容
 * @param key1 第一个键
 * @param key2 第二个键
 * @return true 如果键相同，false 否则
 */
// compare whether the two keys are identical
// optimistic: do not check hash
inline bool
kv_match(const struct kv * const key1, const struct kv * const key2)
{
  //cpu_prefetch0(((u8 *)key2) + 64);
  //return (key1->hash == key2->hash)
  //  && (key1->klen == key2->klen)
  //  && (!memcmp(key1->kv, key2->kv, key1->klen));
  return (key1->klen == key2->klen) && (!memcmp(key1->kv, key2->kv, key1->klen));
}

/**
 * 比较两个键是否完全相同
 * 悲观比较：先检查哈希值，如果不匹配则快速返回false
 * @param key1 第一个键
 * @param key2 第二个键
 * @return true 如果键相同，false 否则
 */
// compare whether the two keys are identical
// check hash first
// pessimistic: return false quickly if their hashes mismatch
  inline bool
kv_match_hash(const struct kv * const key1, const struct kv * const key2)
{
  return (key1->hash == key2->hash)
    && (key1->klen == key2->klen)
    && (!memcmp(key1->kv, key2->kv, key1->klen));
}

/**
 * 比较两个完整的键值对是否相同
 * 包括键、值和所有元数据
 * @param kv1 第一个键值对
 * @param kv2 第二个键值对
 * @return true 如果完全相同，false 否则
 */
  inline bool
kv_match_full(const struct kv * const kv1, const struct kv * const kv2)
{
  return (kv1->kvlen == kv2->kvlen)
    && (!memcmp(kv1, kv2, sizeof(*kv1) + kv1->klen + kv1->vlen));
}

/**
 * 比较键值对的键与kv128编码的键是否匹配
 * @param sk 键值对指针
 * @param kv128 kv128编码的数据
 * @return true 如果键匹配，false 否则
 */
  bool
kv_match_kv128(const struct kv * const sk, const u8 * const kv128)
{
  debug_assert(sk);
  debug_assert(kv128);

  u32 klen128 = 0;
  u32 vlen128 = 0;
  const u8 * const pdata = vi128_decode_u32(vi128_decode_u32(kv128, &klen128), &vlen128);
  (void)vlen128;
  return (sk->klen == klen128) && (!memcmp(sk->kv, pdata, klen128));
}

/**
 * 比较两个键值对的键的大小关系
 * @param kv1 第一个键值对
 * @param kv2 第二个键值对
 * @return <0 如果kv1<kv2，>0 如果kv1>kv2，0 如果相等
 */
  inline int
kv_compare(const struct kv * const kv1, const struct kv * const kv2)
{
  const u32 len = kv1->klen < kv2->klen ? kv1->klen : kv2->klen;
  const int cmp = memcmp(kv1->kv, kv2->kv, (size_t)len);
  return cmp ? cmp : klen_compare(kv1->klen, kv2->klen);
}

/**
 * 用于qsort和bsearch的比较函数
 * 比较两个键值对指针指向的键值对
 * @param p1 第一个键值对指针的指针
 * @param p2 第二个键值对指针的指针
 * @return 比较结果
 */
// for qsort and bsearch
  static int
kv_compare_ptrs(const void * const p1, const void * const p2)
{
  const struct kv * const * const pp1 = (typeof(pp1))p1;
  const struct kv * const * const pp2 = (typeof(pp2))p2;
  return kv_compare(*pp1, *pp2);
}

/**
 * 比较键值对的键与k128编码的键
 * @param sk 键值对指针
 * @param k128 k128编码的键数据
 * @return 比较结果
 */
  int
kv_k128_compare(const struct kv * const sk, const u8 * const k128)
{
  debug_assert(sk);
  const u32 klen1 = sk->klen;
  u32 klen2 = 0;
  const u8 * const ptr2 = vi128_decode_u32(k128, &klen2);
  debug_assert(ptr2);
  const u32 len = (klen1 < klen2) ? klen1 : klen2;
  const int cmp = memcmp(sk->kv, ptr2, len);
  return cmp ? cmp : klen_compare(klen1, klen2);
}

/**
 * 比较键值对的键与kv128编码的键
 * @param sk 键值对指针
 * @param kv128 kv128编码的数据
 * @return 比较结果
 */
  int
kv_kv128_compare(const struct kv * const sk, const u8 * const kv128)
{
  debug_assert(sk);
  const u32 klen1 = sk->klen;
  u32 klen2 = 0;
  u32 vlen2 = 0;
  const u8 * const ptr2 = vi128_decode_u32(vi128_decode_u32(kv128, &klen2), &vlen2);
  const u32 len = (klen1 < klen2) ? klen1 : klen2;
  const int cmp = memcmp(sk->kv, ptr2, len);
  return cmp ? cmp : klen_compare(klen1, klen2);
}

/**
 * 对键值对指针数组进行快速排序
 * @param kvs 键值对指针数组
 * @param nr 数组大小
 */
  inline void
kv_qsort(struct kv ** const kvs, const size_t nr)
{
  qsort(kvs, nr, sizeof(kvs[0]), kv_compare_ptrs);
}

/**
 * 计算两个键的最长公共前缀长度
 * @param key1 第一个键
 * @param key2 第二个键
 * @return 最长公共前缀的长度
 */
// return the length of longest common prefix of the two keys
  inline u32
kv_key_lcp(const struct kv * const key1, const struct kv * const key2)
{
  const u32 max = (key1->klen < key2->klen) ? key1->klen : key2->klen;
  return memlcp(key1->kv, key2->kv, max);
}

/**
 * 计算两个键的最长公共前缀长度（跳过已知的公共前缀）
 * @param key1 第一个键
 * @param key2 第二个键
 * @param lcp0 已知的公共前缀长度
 * @return 最长公共前缀的长度
 */
// return the length of longest common prefix of the two keys with a known lcp0
  inline u32
kv_key_lcp_skip(const struct kv * const key1, const struct kv * const key2, const u32 lcp0)
{
  const u32 max = (key1->klen < key2->klen) ? key1->klen : key2->klen;
  debug_assert(max >= lcp0);
  return lcp0 + memlcp(key1->kv+lcp0, key2->kv+lcp0, max-lcp0);
}
// }}}

// psort {{{
// 部分排序（partial sort）实现

/**
 * 交换数组中两个位置的键值对指针
 * @param kvs 键值对指针数组
 * @param i 第一个位置索引
 * @param j 第二个位置索引
 */
  static inline void
kv_psort_exchange(struct kv ** const kvs, const u64 i, const u64 j)
{
  if (i != j) {
    struct kv * const tmp = kvs[i];
    kvs[i] = kvs[j];
    kvs[j] = tmp;
  }
}

/**
 * 快速排序的分区操作
 * @param kvs 键值对指针数组
 * @param lo 低位索引
 * @param hi 高位索引
 * @return 分区点索引
 */
  static u64
kv_psort_partition(struct kv ** const kvs, const u64 lo, const u64 hi)
{
  if (lo >= hi)
    return lo;

  const u64 p = (lo+hi) >> 1;
  kv_psort_exchange(kvs, lo, p);
  u64 i = lo;
  u64 j = hi + 1;
  do {
    while (kv_compare(kvs[++i], kvs[lo]) < 0 && i < hi);
    while (kv_compare(kvs[--j], kvs[lo]) > 0);
    if (i >= j)
      break;
    kv_psort_exchange(kvs, i, j);
  } while (true);
  kv_psort_exchange(kvs, lo, j);
  return j;
}

/**
 * 部分排序的递归实现
 * 只对目标范围内的元素进行排序
 * @param kvs 键值对指针数组
 * @param lo 当前排序范围的低位索引
 * @param hi 当前排序范围的高位索引
 * @param tlo 目标范围的低位索引
 * @param thi 目标范围的高位索引
 */
  static void
kv_psort_rec(struct kv ** const kvs, const u64 lo, const u64 hi, const u64 tlo, const u64 thi)
{
  if (lo >= hi)
    return;
  const u64 c = kv_psort_partition(kvs, lo, hi);

  if (c > tlo) // go left
    kv_psort_rec(kvs, lo, c-1, tlo, thi);

  if (c < thi) // go right
    kv_psort_rec(kvs, c+1, hi, tlo, thi);
}

/**
 * 部分排序：只对指定范围内的元素进行排序
 * 用于只需要前K个最小元素的情况，避免全排序的开销
 * @param kvs 键值对指针数组
 * @param nr 数组总大小
 * @param tlo 目标范围的低位索引
 * @param thi 目标范围的高位索引
 */
  inline void
kv_psort(struct kv ** const kvs, const u64 nr, const u64 tlo, const u64 thi)
{
  debug_assert(tlo <= thi);
  debug_assert(thi < nr);
  kv_psort_rec(kvs, 0, nr-1, tlo, thi);
}
// }}} psort

// ptr {{{
// 指针访问函数

/**
 * 获取键值对中值部分的可写指针
 * @param kv 键值对指针
 * @return 值部分的指针
 */
  inline void *
kv_vptr(struct kv * const kv)
{
  return (void *)(&(kv->kv[kv->klen]));
}

/**
 * 获取键值对中键部分的可写指针
 * @param kv 键值对指针
 * @return 键部分的指针
 */
  inline void *
kv_kptr(struct kv * const kv)
{
  return (void *)(&(kv->kv[0]));
}

/**
 * 获取键值对中值部分的只读指针
 * @param kv 键值对指针
 * @return 值部分的常量指针
 */
  inline const void *
kv_vptr_c(const struct kv * const kv)
{
  return (const void *)(&(kv->kv[kv->klen]));
}

/**
 * 获取键值对中键部分的只读指针
 * @param kv 键值对指针
 * @return 键部分的常量指针
 */
  inline const void *
kv_kptr_c(const struct kv * const kv)
{
  return (const void *)(&(kv->kv[0]));
}
// }}} ptr

// print {{{
// 调试和输出函数

/**
 * 打印键值对信息
 * @param kv 键值对指针
 * @param cmd 格式命令字符串：
 *            - 第1个字符控制键的显示格式：'s'=字符串, 'x'=十六进制, 'd'=十进制
 *            - 第2个字符控制值的显示格式：'s'=字符串, 'x'=十六进制, 'd'=十进制
 *            - 'n' 表示结尾加换行符
 * @param out 输出文件流
 */
// cmd "KV" K and V can be 's': string, 'x': hex, 'd': dec, or else for not printing.
// n for newline after kv
  void
kv_print(const struct kv * const kv, const char * const cmd, FILE * const out)
{
  debug_assert(cmd);
  const u32 klen = kv->klen;
  fprintf(out, "#%016lx k[%3u]", kv->hash, klen);

  switch(cmd[0]) {
  case 's': fprintf(out, " %.*s", klen, kv->kv); break;
  case 'x': str_print_hex(out, kv->kv, klen); break;
  case 'd': str_print_dec(out, kv->kv, klen); break;
  default: break;
  }

  const u32 vlen = kv->vlen;
  switch (cmd[1]) {
  case 's': fprintf(out, "  v[%4u] %.*s", vlen, vlen, kv->kv+klen); break;
  case 'x': fprintf(out, "  v[%4u]", vlen); str_print_hex(out, kv->kv+klen, vlen); break;
  case 'd': fprintf(out, "  v[%4u]", vlen); str_print_dec(out, kv->kv+klen, vlen); break;
  default: break;
  }
  if (strchr(cmd, 'n'))
    fprintf(out, "\n");
}
// }}} print

// mm {{{
// 内存管理抽象层函数

/**
 * 无操作的输入内存管理函数
 * 直接返回原始键值对指针，不进行复制
 * @param kv 键值对指针
 * @param priv 私有数据（未使用）
 * @return 原始键值对指针
 */
  struct kv *
kvmap_mm_in_noop(struct kv * const kv, void * const priv)
{
  (void)priv;
  return kv;
}

/**
 * 无操作的输出内存管理函数
 * 直接返回原始键值对指针，不使用输出缓冲区
 * @param kv 键值对指针
 * @param out 输出缓冲区（未使用）
 * @return 原始键值对指针
 */
// copy-out
  struct kv *
kvmap_mm_out_noop(struct kv * const kv, struct kv * const out)
{
  (void)out;
  return kv;
}

/**
 * 无操作的释放内存管理函数
 * 不释放任何内存
 * @param kv 键值对指针（未使用）
 * @param priv 私有数据（未使用）
 */
  void
kvmap_mm_free_noop(struct kv * const kv, void * const priv)
{
  (void)kv;
  (void)priv;
}

/**
 * 复制输入的内存管理函数
 * 创建键值对的副本
 * @param kv 键值对指针
 * @param priv 私有数据（未使用）
 * @return 复制的键值对指针
 */
// copy-in
  struct kv *
kvmap_mm_in_dup(struct kv * const kv, void * const priv)
{
  (void)priv;
  return kv_dup(kv);
}

/**
 * 复制输出的内存管理函数
 * 将键值对复制到指定位置或分配新内存
 * @param kv 源键值对指针
 * @param out 输出缓冲区
 * @return 复制的键值对指针
 */
// copy-out
  struct kv *
kvmap_mm_out_dup(struct kv * const kv, struct kv * const out)
{
  return kv_dup2(kv, out);
}

/**
 * 标准的释放内存管理函数
 * 使用free()释放内存
 * @param kv 键值对指针
 * @param priv 私有数据（未使用）
 */
  void
kvmap_mm_free_free(struct kv * const kv, void * const priv)
{
  (void)priv;
  free(kv);
}

// 复制模式的内存管理器：输入复制，输出复制，使用free释放
const struct kvmap_mm kvmap_mm_dup = {
  .in = kvmap_mm_in_dup,
  .out = kvmap_mm_out_dup,
  .free = kvmap_mm_free_free,
  .priv = NULL,
};

// 无复制输入，复制输出，使用free释放的内存管理器
const struct kvmap_mm kvmap_mm_ndf = {
  .in = kvmap_mm_in_noop,
  .out = kvmap_mm_out_dup,
  .free = kvmap_mm_free_free,
  .priv = NULL,
};

// }}} mm

// kref {{{
// 键引用（key reference）操作函数

/**
 * 创建原始键引用（不计算哈希）
 * @param kref 键引用指针
 * @param ptr 键数据指针
 * @param len 键长度
 */
  inline void
kref_ref_raw(struct kref * const kref, const u8 * const ptr, const u32 len)
{
  kref->ptr = ptr;
  kref->len = len;
  kref->hash32 = 0;
}

/**
 * 创建键引用并计算哈希值
 * @param kref 键引用指针
 * @param ptr 键数据指针
 * @param len 键长度
 */
  inline void
kref_ref_hash32(struct kref * const kref, const u8 * const ptr, const u32 len)
{
  kref->ptr = ptr;
  kref->len = len;
  kref->hash32 = kv_crc32c(ptr, len);
}

/**
 * 更新键引用的哈希值
 * @param kref 键引用指针
 */
  inline void
kref_update_hash32(struct kref * const kref)
{
  kref->hash32 = kv_crc32c(kref->ptr, kref->len);
}

/**
 * 从键值对创建键引用
 * @param kref 键引用指针
 * @param kv 键值对指针
 */
  inline void
kref_ref_kv(struct kref * const kref, const struct kv * const kv)
{
  kref->ptr = kv->kv;
  kref->len = kv->klen;
  kref->hash32 = kv->hashlo;
}

/**
 * 从键值对创建键引用并重新计算哈希值
 * @param kref 键引用指针
 * @param kv 键值对指针
 */
  inline void
kref_ref_kv_hash32(struct kref * const kref, const struct kv * const kv)
{
  kref->ptr = kv->kv;
  kref->len = kv->klen;
  kref->hash32 = kv_crc32c(kv->kv, kv->klen);
}

/**
 * 比较两个键引用是否相同
 * @param k1 第一个键引用
 * @param k2 第二个键引用
 * @return true 如果相同，false 否则
 */
  inline bool
kref_match(const struct kref * const k1, const struct kref * const k2)
{
  return (k1->len == k2->len) && (!memcmp(k1->ptr, k2->ptr, k1->len));
}

/**
 * 比较键引用和键值对的键是否匹配
 * @param kref 键引用指针
 * @param k 键值对指针
 * @return true 如果匹配，false 否则
 */
// match a kref and a key
  inline bool
kref_kv_match(const struct kref * const kref, const struct kv * const k)
{
  return (kref->len == k->klen) && (!memcmp(kref->ptr, k->kv, kref->len));
}

/**
 * 比较两个键引用的大小关系
 * @param kref1 第一个键引用
 * @param kref2 第二个键引用
 * @return <0 如果kref1<kref2，>0 如果kref1>kref2，0 如果相等
 */
  inline int
kref_compare(const struct kref * const kref1, const struct kref * const kref2)
{
  const u32 len = kref1->len < kref2->len ? kref1->len : kref2->len;
  const int cmp = memcmp(kref1->ptr, kref2->ptr, (size_t)len);
  return cmp ? cmp : klen_compare(kref1->len, kref2->len);
}

/**
 * 比较键引用和键值对的键的大小关系
 * @param kref 键引用指针
 * @param k 键值对指针
 * @return 比较结果
 */
// compare a kref and a key
  inline int
kref_kv_compare(const struct kref * const kref, const struct kv * const k)
{
  debug_assert(kref);
  debug_assert(k);
  const u32 len = kref->len < k->klen ? kref->len : k->klen;
  const int cmp = memcmp(kref->ptr, k->kv, (size_t)len);
  return cmp ? cmp : klen_compare(kref->len, k->klen);
}

/**
 * 计算两个键引用的最长公共前缀长度
 * @param k1 第一个键引用
 * @param k2 第二个键引用
 * @return 最长公共前缀的长度
 */
  inline u32
kref_lcp(const struct kref * const k1, const struct kref * const k2)
{
  const u32 max = (k1->len < k2->len) ? k1->len : k2->len;
  return memlcp(k1->ptr, k2->ptr, max);
}

/**
 * 计算键引用和键值对的键的最长公共前缀长度
 * @param kref 键引用指针
 * @param kv 键值对指针
 * @return 最长公共前缀的长度
 */
  inline u32
kref_kv_lcp(const struct kref * const kref, const struct kv * const kv)
{
  const u32 max = (kref->len < kv->klen) ? kref->len : kv->klen;
  return memlcp(kref->ptr, kv->kv, max);
}

/**
 * 比较键引用和k128编码的键
 * @param sk 键引用指针
 * @param k128 k128编码的键数据
 * @return 比较结果
 */
// klen, key, ...
  inline int
kref_k128_compare(const struct kref * const sk, const u8 * const k128)
{
  debug_assert(sk);
  const u32 klen1 = sk->len;
  u32 klen2 = 0;
  const u8 * const ptr2 = vi128_decode_u32(k128, &klen2);
  debug_assert(ptr2);
  const u32 len = (klen1 < klen2) ? klen1 : klen2;
  const int cmp = memcmp(sk->ptr, ptr2, len);
  return cmp ? cmp : klen_compare(klen1, klen2);
}

/**
 * 比较键引用和kv128编码的键
 * @param sk 键引用指针
 * @param kv128 kv128编码的数据
 * @return 比较结果
 */
// klen, vlen, key, ...
  inline int
kref_kv128_compare(const struct kref * const sk, const u8 * const kv128)
{
  debug_assert(sk);
  const u32 klen1 = sk->len;
  u32 klen2 = 0;
  u32 vlen2 = 0;
  const u8 * const ptr2 = vi128_decode_u32(vi128_decode_u32(kv128, &klen2), &vlen2);
  const u32 len = (klen1 < klen2) ? klen1 : klen2;
  const int cmp = memcmp(sk->ptr, ptr2, len);
  return cmp ? cmp : klen_compare(klen1, klen2);
}

// 静态的空键引用
static struct kref __kref_null = {.hash32 = KV_CRC32C_SEED};

/**
 * 获取空键引用的指针
 * @return 空键引用的常量指针
 */
  inline const struct kref *
kref_null(void)
{
  return &__kref_null;
}
// }}} kref

// kvref {{{
// 键值引用（key-value reference）操作函数

/**
 * 从键值对创建键值引用
 * 键值引用包含分离的键指针、值指针和头部信息
 * @param ref 键值引用指针
 * @param kv 键值对指针
 */
  inline void
kvref_ref_kv(struct kvref * const ref, struct kv * const kv)
{
  ref->kptr = kv->kv;
  ref->vptr = kv->kv + kv->klen;
  ref->hdr = *kv;
}

/**
 * 从键值引用复制出完整的键值对
 * @param ref 键值引用指针
 * @param to 目标内存位置（NULL则分配新内存）
 * @return 复制的键值对指针，失败返回NULL
 */
  struct kv *
kvref_dup2_kv(struct kvref * const ref, struct kv * const to)
{
  if (ref == NULL)
    return NULL;
  const size_t sz = sizeof(*to) + ref->hdr.klen + ref->hdr.vlen;
  struct kv * const new = to ? to : malloc(sz);
  if (new == NULL)
    return NULL;

  *new = ref->hdr;
  memcpy(new->kv, ref->kptr, new->klen);
  memcpy(new->kv + new->klen, ref->vptr, new->vlen);
  return new;
}

/**
 * 从键值引用复制出键部分
 * @param ref 键值引用指针
 * @param to 目标内存位置（NULL则分配新内存）
 * @return 复制的键（vlen=0），失败返回NULL
 */
  struct kv *
kvref_dup2_key(struct kvref * const ref, struct kv * const to)
{
  if (ref == NULL)
    return NULL;
  const size_t sz = sizeof(*to) + ref->hdr.klen;
  struct kv * const new = to ? to : malloc(sz);
  if (new == NULL)
    return NULL;

  *new = ref->hdr;
  memcpy(new->kv, ref->kptr, new->klen);
  return new;
}

/**
 * 比较键值引用和键值对的键
 * @param ref 键值引用指针
 * @param kv 键值对指针
 * @return 比较结果
 */
  int
kvref_kv_compare(const struct kvref * const ref, const struct kv * const kv)
{
  const u32 len = ref->hdr.klen < kv->klen ? ref->hdr.klen : kv->klen;
  const int cmp = memcmp(ref->kptr, kv->kv, (size_t)len);
  return cmp ? cmp : klen_compare(ref->hdr.klen, kv->klen);
}
// }}} kvref

// kv128 {{{
// kv128 编解码函数（变长整数编码）

/**
 * 估算键值对编码为kv128格式的大小
 * @param kv 键值对指针
 * @return 估算的编码大小
 */
// estimate the encoded size
  inline size_t
kv128_estimate_kv(const struct kv * const kv)
{
  return vi128_estimate_u32(kv->klen) + vi128_estimate_u32(kv->vlen) + kv->klen + kv->vlen;
}

/**
 * 将键值对编码为kv128格式
 * 格式：[klen][vlen][key][value]，长度使用变长整数编码
 * @param kv 键值对指针
 * @param out 输出缓冲区（NULL则分配新内存）
 * @param pesize 输出编码后的实际大小（可为NULL）
 * @return 编码后的数据指针，失败返回NULL
 */
// create a kv128 from kv
  u8 *
kv128_encode_kv(const struct kv * const kv, u8 * const out, size_t * const pesize)
{
  u8 * const ptr = out ? out : malloc(kv128_estimate_kv(kv));
  if (!ptr)
    return NULL;

  u8 * const pdata = vi128_encode_u32(vi128_encode_u32(ptr, kv->klen), kv->vlen);
  memcpy(pdata, kv->kv, kv->klen + kv->vlen);

  if (pesize)
    *pesize = (size_t)(pdata - ptr) + kv->klen + kv->vlen;
  return ptr; // return the head of the encoded kv128
}

/**
 * 将kv128格式解码为键值对
 * @param ptr kv128编码的数据指针
 * @param out 输出键值对缓冲区（NULL则分配新内存）
 * @param pesize 输出解码后的数据大小（可为NULL）
 * @return 解码后的键值对指针，失败返回NULL
 */
// dup kv128 to a kv
  struct kv *
kv128_decode_kv(const u8 * const ptr, struct kv * const out, size_t * const pesize)
{
  u32 klen, vlen;
  const u8 * const pdata = vi128_decode_u32(vi128_decode_u32(ptr, &klen), &vlen);
  struct kv * const ret = out ? out : malloc(sizeof(struct kv) + klen + vlen);
  if (ret)
    kv_refill(ret, pdata, klen, pdata + klen, vlen);

  if (pesize)
    *pesize = (size_t)(pdata - ptr) + klen + vlen;
  return ret; // return the kv
}

/**
 * 计算kv128编码数据的总大小
 * @param ptr kv128编码的数据指针
 * @return 编码数据的总大小
 */
  inline size_t
kv128_size(const u8 * const ptr)
{
  u32 klen, vlen;
  const u8 * const pdata = vi128_decode_u32(vi128_decode_u32(ptr, &klen), &vlen);
  return ((size_t)(pdata - ptr)) + klen + vlen;
}
// }}} kv128

// }}} kv

// kvmap {{{
// 键值映射（kvmap）抽象层实现

// registry {{{
// API注册管理系统

// 增加MAX值如果需要更多注册项
// increase MAX if need more
#define KVMAP_API_MAX ((32))
static struct kvmap_api_reg kvmap_api_regs[KVMAP_API_MAX];
static u64 kvmap_api_regs_nr = 0;

/**
 * 注册键值映射API实现
 * @param nargs 参数数量
 * @param name API名称
 * @param args_msg 参数说明信息
 * @param create 创建函数指针
 * @param api API接口指针
 */
  void
kvmap_api_register(const int nargs, const char * const name, const char * const args_msg,
    void * (*create)(const char *, const struct kvmap_mm *, char **), const struct kvmap_api * const api)
{
  if (kvmap_api_regs_nr < KVMAP_API_MAX) {
    kvmap_api_regs[kvmap_api_regs_nr].nargs = nargs;
    kvmap_api_regs[kvmap_api_regs_nr].name = name;
    kvmap_api_regs[kvmap_api_regs_nr].args_msg = args_msg;
    kvmap_api_regs[kvmap_api_regs_nr].create = create;
    kvmap_api_regs[kvmap_api_regs_nr].api = api;
    kvmap_api_regs_nr++;
  } else {
    fprintf(stderr, "%s failed to register [%s]\n", __func__, name);
  }
}

/**
 * 显示API帮助信息
 * 列出所有已注册的键值映射API及其用法
 */
  void
kvmap_api_helper_message(void)
{
  fprintf(stderr, "%s Usage: api <map-type> <param1> ...\n", __func__);
  for (u64 i = 0; i < kvmap_api_regs_nr; i++) {
    fprintf(stderr, "%s example: api %s %s\n", __func__,
        kvmap_api_regs[i].name, kvmap_api_regs[i].args_msg);
  }
}

/**
 * API辅助函数，根据命令行参数创建相应的键值映射
 * @param argc 参数数量
 * @param argv 参数数组
 * @param mm 内存管理器
 * @param api_out 输出API指针
 * @param map_out 输出映射对象指针
 * @return 成功返回消耗的参数数量，失败返回-1
 */
  int
kvmap_api_helper(int argc, char ** const argv, const struct kvmap_mm * const mm,
    const struct kvmap_api ** const api_out, void ** const map_out)
{
  // "api" "name" "arg1", ...
  if (argc < 2 || strcmp(argv[0], "api") != 0)
    return -1;

  for (u64 i = 0; i < kvmap_api_regs_nr; i++) {
    const struct kvmap_api_reg * const reg = &kvmap_api_regs[i];
    if (0 != strcmp(argv[1], reg->name))
      continue;

    if ((argc - 2) < reg->nargs)
      return -1;

    void * const map = reg->create(argv[1], mm, argv + 2); // skip "api" "name"
    if (map) {
      *api_out = reg->api;
      *map_out = map;
      return 2 + reg->nargs;
    } else {
      return -1;
    }
  }

  // no match
  return -1;
}
// }}} registry

// misc {{{
// 杂项辅助函数

/**
 * 键值对输入处理函数：窃取键值对指针
 * 将键值对指针保存到私有数据中，用于获取内部键值对
 * @param kv 键值对指针
 * @param priv 私有数据指针（存储键值对指针的地址）
 */
  void
kvmap_inp_steal_kv(struct kv * const kv, void * const priv)
{
  // steal the kv pointer out so we don't need a dangerous get_key_interanl()
  if (priv)
    *(struct kv **)priv = kv;
}

/**
 * 获取键值映射的引用
 * 如果API提供了ref函数则调用，否则直接返回原指针
 * @param api API接口指针
 * @param map 映射对象指针
 * @return 引用指针
 */
  inline void *
kvmap_ref(const struct kvmap_api * const api, void * const map)
{
  return api->ref ? api->ref(map) : map;
}

/**
 * 释放键值映射的引用
 * 如果API提供了unref函数则调用，否则直接返回原指针
 * @param api API接口指针
 * @param ref 引用指针
 * @return 原始映射对象指针（通常不被调用者使用）
 */
// return the original map pointer; usually unused by caller
  inline void *
kvmap_unref(const struct kvmap_api * const api, void * const ref)
{
  return api->unref ? api->unref(ref) : ref;
}
// }}} misc

// kvmap_kv_op {{{
  inline struct kv *
kvmap_kv_get(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, struct kv * const out)
{
  const struct kref kref = kv_kref(key);
  return api->get(ref, &kref, out);
}

  inline bool
kvmap_kv_probe(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key)
{
  const struct kref kref = kv_kref(key);
  return api->probe(ref, &kref);
}

  inline bool
kvmap_kv_put(const struct kvmap_api * const api, void * const ref,
    struct kv * const kv)
{
  return api->put(ref, kv);
}

  inline bool
kvmap_kv_del(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key)
{
  const struct kref kref = kv_kref(key);
  return api->del(ref, &kref);
}

  inline bool
kvmap_kv_inpr(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_inp_func uf, void * const priv)
{
  const struct kref kref = kv_kref(key);
  return api->inpr(ref, &kref, uf, priv);
}

  inline bool
kvmap_kv_inpw(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_inp_func uf, void * const priv)
{
  const struct kref kref = kv_kref(key);
  return api->inpw(ref, &kref, uf, priv);
}

  inline bool
kvmap_kv_merge(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_merge_func uf, void * const priv)
{
  const struct kref kref = kv_kref(key);
  return api->merge(ref, &kref, uf, priv);
}

  inline u64
kvmap_kv_delr(const struct kvmap_api * const api, void * const ref,
    const struct kv * const start, const struct kv * const end)
{
  const struct kref kref0 = kv_kref(start);
  if (end) {
    const struct kref krefz = kv_kref(end);
    return api->delr(ref, &kref0, &krefz);
  } else {
    return api->delr(ref, &kref0, NULL);
  }
}

  inline void
kvmap_kv_iter_seek(const struct kvmap_api * const api, void * const iter,
    const struct kv * const key)
{
  const struct kref kref = kv_kref(key);
  api->iter_seek(iter, &kref);
}
// }}} kvmap_kv_op

// kvmap_raw_op {{{
  inline struct kv *
kvmap_raw_get(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, struct kv * const out)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  return api->get(ref, &kref, out);
}

  inline bool
kvmap_raw_probe(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  return api->probe(ref, &kref);
}

  inline bool
kvmap_raw_del(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  return api->del(ref, &kref);
}

  inline bool
kvmap_raw_inpr(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, kv_inp_func uf, void * const priv)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  return api->inpr(ref, &kref, uf, priv);
}

  inline bool
kvmap_raw_inpw(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, kv_inp_func uf, void * const priv)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  return api->inpw(ref, &kref, uf, priv);
}

  inline void
kvmap_raw_iter_seek(const struct kvmap_api * const api, void * const iter,
    const u32 len, const u8 * const ptr)
{
  const struct kref kref = {.ptr = ptr, .len = len,
    .hash32 = api->hashkey ? kv_crc32c(ptr, len) : 0};
  api->iter_seek(iter, &kref);
}
// }}}} kvmap_raw_op

// dump {{{
  u64
kvmap_dump_keys(const struct kvmap_api * const api, void * const map, const int fd)
{
  void * const ref = kvmap_ref(api, map);
  void * const iter = api->iter_create(ref);
  api->iter_seek(iter, kref_null());
  u64 i = 0;
  while (api->iter_valid(iter)) {
    struct kvref kvref;
    api->iter_kvref(iter, &kvref);
    dprintf(fd, "%010lu [%3u] %.*s [%u]\n", i, kvref.hdr.klen, kvref.hdr.klen, kvref.kptr, kvref.hdr.vlen);
    i++;
    api->iter_skip1(iter);
  }
  api->iter_destroy(iter);
  kvmap_unref(api, ref);
  return i;
}
// }}} dump

// }}} kvmap

// miter {{{
// 多路归并迭代器（merging iterator）实现
// 用于合并多个有序数据流，常用于LSM树的合并操作

// 迭代器流结构（最小堆）
struct miter_stream { // minheap
  struct kref kref;           // 当前键引用
  const struct kvmap_api * api; // API接口
  void * ref;                 // 映射引用
  void * iter;                // 迭代器
  u32 rank;                   // 流的优先级（rank越高优先级越高）
  bool private_ref;           // 是否拥有引用的所有权
  bool private_iter;          // 是否拥有迭代器的所有权
};

// 最大支持的流数量
#define MITER_MAX_STREAMS ((18))

// 多路归并迭代器主结构
struct miter {
  u32 nway;                   // 当前流的数量
  u32 parked;                 // 暂停状态标志（0/1）
  struct kref kref0;          // 保存的键引用（用于skip_unique）
  void * ptr0;                // 键数据缓冲区
  size_t len0;                // 缓冲区分配大小
  // mh[0] 用于保存skip_unique期间的最后一个流
  struct miter_stream * mh[1+MITER_MAX_STREAMS]; // 最小堆数组
};

/**
 * 堆结构说明：
 *       [X]
 *      |    |
 *    [2X]  [2X+1]
 */

/**
 * 创建多路归并迭代器
 * @return 新创建的迭代器指针，失败返回NULL
 */
  struct miter *
miter_create(void)
{
  struct miter * const miter = calloc(1, sizeof(*miter));
  return miter;
}

/**
 * 交换子节点和父节点（用于堆操作）
 * @param miter 多路归并迭代器
 * @param cidx 子节点索引
 */
// swap child (cidx) with its parent
  static inline void
miter_swap(struct miter * const miter, const u32 cidx)
{
  debug_assert(cidx > 1);
  struct miter_stream * const tmp = miter->mh[cidx];
  miter->mh[cidx] = miter->mh[cidx>>1];
  miter->mh[cidx>>1] = tmp;
}

/**
 * 判断是否应该交换父子节点
 * 基于键值比较和优先级排序
 * @param sp 父节点流指针
 * @param sc 子节点流指针
 * @return true 如果应该交换，false 否则
 */
  static bool
miter_should_swap(struct miter_stream * const sp, struct miter_stream * const sc)
{
  if (sp->kref.ptr == NULL)
    return true;
  if (sc->kref.ptr == NULL)
    return false;

  const int c = kref_compare(&sp->kref, &sc->kref);
  if (c > 0)
    return true;
  else if (c < 0)
    return false;
  return sp->rank < sc->rank; // high rank == high priority
}

/**
 * 向上调整堆（当键可能向上移动时调用）
 * @param miter 多路归并迭代器
 * @param idx 起始索引
 */
// call upheap when a key may move up
  static void
miter_upheap(struct miter * const miter, u32 idx)
{
  while (idx > 1) {
    struct miter_stream * sp = miter->mh[idx>>1];
    struct miter_stream * sc = miter->mh[idx];
    if (sc->kref.ptr == NULL)
      return; // +inf
    if (miter_should_swap(sp, sc))
      miter_swap(miter, idx);
    else
      return;
    idx >>= 1;
  }
}

/**
 * 向下调整堆
 * @param miter 多路归并迭代器
 * @param idx 起始索引
 */
  static void
miter_downheap(struct miter * const miter, u32 idx)
{
  while ((idx<<1) <= miter->nway) {
    struct miter_stream * sl = miter->mh[idx<<1];
    u32 idxs = idx << 1;
    if ((idx<<1) < miter->nway) { // has sr
      struct miter_stream * sr = miter->mh[(idx<<1) + 1];
      if (miter_should_swap(sl, sr))
        idxs++;
    }

    if (miter_should_swap(miter->mh[idx], miter->mh[idxs]))
      miter_swap(miter, idxs);
    else
      return;
    idx = idxs;
  }
}

  static void
miter_stream_fix(struct miter_stream * const s)
{
  const bool r = s->api->iter_kref(s->iter, &s->kref);
  if (!r)
    s->kref.ptr = NULL;
}

  static void
miter_stream_skip(struct miter_stream * const s)
{
  s->api->iter_skip1(s->iter);
  miter_stream_fix(s);
}

  static bool
miter_stream_add(struct miter * const miter, const struct kvmap_api * const api,
    void * const ref, void * const iter, const bool private_ref, const bool private_iter)
{
  const u32 way = miter->nway + 1;

  if (miter->mh[way] == NULL)
    miter->mh[way] = malloc(sizeof(struct miter_stream));

  struct miter_stream * const s = miter->mh[way];
  if (s == NULL)
    return false;

  s->kref.ptr = NULL;
  s->iter = iter;
  s->ref = ref;
  s->api = api;
  s->rank = miter->nway; // rank starts with 0
  s->private_ref = private_ref;
  s->private_iter = private_iter;
  miter->nway = way; // +1
  return true;
}

  bool
miter_add_iter(struct miter * const miter, const struct kvmap_api * const api, void * const ref, void * const iter)
{
  if (miter->nway >= MITER_MAX_STREAMS)
    return NULL;

  return miter_stream_add(miter, api, ref, iter, false, false);
}

  void *
miter_add_ref(struct miter * const miter, const struct kvmap_api * const api, void * const ref)
{
  if (miter->nway >= MITER_MAX_STREAMS)
    return NULL;

  void * const iter = api->iter_create(ref);
  if (iter == NULL)
    return NULL;

  const bool r = miter_stream_add(miter, api, ref, iter, false, true);
  if (!r) {
    api->iter_destroy(iter);
    return NULL;
  }
  return iter;
}

// add lower-level stream first, and then moving up
// add(s1); add(s2);
// if two keys in s1 and s2 are equal, the key in s2 will be poped out first
// return the iter created for map; caller should not edit iter while miter is active
  void *
miter_add(struct miter * const miter, const struct kvmap_api * const api, void * const map)
{
  if (miter->nway >= MITER_MAX_STREAMS)
    return NULL;

  void * const ref = kvmap_ref(api, map);
  if (ref == NULL)
    return NULL;

  void * const iter = api->iter_create(ref);
  if (iter == NULL) {
    kvmap_unref(api, ref);
    return NULL;
  }

  const bool r = miter_stream_add(miter, api, ref, iter, true, true);
  if (!r) {
    api->iter_destroy(iter);
    kvmap_unref(api, ref);
    return NULL;
  }
  return iter;
}

  u32
miter_rank(struct miter * const miter)
{
  if (!miter_valid(miter))
    return UINT32_MAX;
  return miter->mh[1]->rank;
}

  static void
miter_resume(struct miter * const miter)
{
  if (miter->parked) {
    miter->parked = 0;
    for (u32 i = 1; i <= miter->nway; i++) {
      struct miter_stream * const s = miter->mh[i];
      if (s->api->refpark)
        s->api->resume(s->ref);
    }
  }
}

  void
miter_seek(struct miter * const miter, const struct kref * const key)
{
  miter_resume(miter);
  for (u32 i = 1; i <= miter->nway; i++) {
    struct miter_stream * const s = miter->mh[i];
    s->api->iter_seek(s->iter, key);
    miter_stream_fix(s);
  }
  for (u32 i = 2; i <= miter->nway; i++)
    miter_upheap(miter, i);
}

  void
miter_kv_seek(struct miter * const miter, const struct kv * const key)
{
  const struct kref s = kv_kref(key);
  miter_seek(miter, &s);
}

  bool
miter_valid(struct miter * const miter)
{
  return miter->nway && miter->mh[1]->kref.ptr;
}

  static bool
miter_valid_1(struct miter * const miter)
{
  return miter->nway != 0;
}

  struct kv *
miter_peek(struct miter * const miter, struct kv * const out)
{
  if (!miter_valid_1(miter))
    return NULL;

  struct miter_stream * const s = miter->mh[1];
  return s->api->iter_peek(s->iter, out);
}

  bool
miter_kref(struct miter * const miter, struct kref * const kref)
{
  if (!miter_valid_1(miter))
    return false;

  struct miter_stream * const s = miter->mh[1];
  return s->api->iter_kref(s->iter, kref);
}

  bool
miter_kvref(struct miter * const miter, struct kvref * const kvref)
{
  if (!miter_valid_1(miter))
    return false;

  struct miter_stream * const s = miter->mh[1];
  return s->api->iter_kvref(s->iter, kvref);
}

  void
miter_skip1(struct miter * const miter)
{
  if (miter_valid(miter)) {
    miter_stream_skip(miter->mh[1]);
    miter_downheap(miter, 1);
  }
}

  void
miter_skip(struct miter * const miter, const u32 nr)
{
  for (u32 i = 0; i < nr; i++) {
    if (!miter_valid(miter))
      return;
    miter_stream_skip(miter->mh[1]);
    miter_downheap(miter, 1);
  }
}

  struct kv *
miter_next(struct miter * const miter, struct kv * const out)
{
  if (!miter_valid_1(miter))
    return NULL;
  struct kv * const ret = miter_peek(miter, out);
  miter_skip1(miter);
  return ret;
}

  static u64
miter_retain_key0(struct miter * const miter)
{
  struct miter_stream * const s0 = miter->mh[1];
  const struct kvmap_api * const api0 = s0->api;
  if (api0->iter_retain) { // no copy
    miter->kref0 = s0->kref;
    miter->mh[0] = s0;
    return api0->iter_retain(s0->iter);
  } else {
    struct kref * const kref = &s0->kref;
    if (unlikely(kref->len > miter->len0)) {
      const size_t len1 = miter->len0 + PGSZ;
      miter->ptr0 = realloc(miter->ptr0, len1);
      miter->len0 = len1;
      debug_assert(miter->ptr0);
    }

    miter->kref0.len = kref->len;
    miter->kref0.hash32 = kref->hash32;
    miter->kref0.ptr = miter->ptr0;
    memcpy(miter->ptr0, kref->ptr, kref->len);
    miter->mh[0] = NULL;
    return 0;
  }
}

  static void
miter_release_key0(struct miter * const miter, const u64 opaque)
{
  struct miter_stream * const s0 = miter->mh[0];
  if (s0) {
    const struct kvmap_api * const api0 = s0->api;
    if (api0->iter_release)
      api0->iter_release(s0->iter, opaque);
  }
}

/**
 * 跳过当前键的所有重复项
 * 在多路归并中，当多个流有相同键时，只保留优先级最高的
 */
  void
miter_skip_unique(struct miter * const miter)
{
  if (!miter_valid(miter))
    return;

  const u64 opaque = miter_retain_key0(miter); // save the current key to kref0
  struct miter_stream * const s0 = miter->mh[1];
  const bool unique0 = s0->api->unique;
  do {
    miter_skip1(miter);
    if (!miter_valid(miter))
      break;
    // try to avoid cmp with unique stream
    if (unique0 && (miter->mh[1] == s0))
      break;
  } while (kref_compare(&miter->kref0, &(miter->mh[1]->kref)) == 0);
  miter_release_key0(miter, opaque);
}

/**
 * 获取下一个唯一键的键值对
 * @param miter 多路归并迭代器
 * @param out 输出缓冲区
 * @return 键值对指针，无更多数据返回NULL
 */
  struct kv *
miter_next_unique(struct miter * const miter, struct kv * const out)
{
  if (!miter_valid(miter))
    return NULL;
  struct kv * const ret = miter_peek(miter, out);
  miter_skip_unique(miter);
  return ret;
}

/**
 * 暂停所有流的迭代器
 * 用于释放资源或进入休眠状态
 * @param miter 多路归并迭代器
 */
  void
miter_park(struct miter * const miter)
{
  for (u32 i = 1; i <= miter->nway; i++) {
    struct miter_stream * const s = miter->mh[i];
    // park the iter
    if (s->api->iter_park)
      s->api->iter_park(s->iter);
    s->kref.ptr = NULL;
    // park ref
    if (s->api->refpark) {
      s->api->park(s->ref);
      miter->parked = 1;
    }
  }
}

/**
 * 清理多路归并迭代器的所有流
 * 释放迭代器和引用，但保留主结构
 * @param miter 多路归并迭代器
 */
  void
miter_clean(struct miter * const miter)
{
  miter_resume(miter); // resume refs if parked
  for (u32 i = 1; i <= miter->nway; i++) {
    struct miter_stream * const s = miter->mh[i];
    const struct kvmap_api * const api = s->api;
    if (s->private_iter)
      api->iter_destroy(s->iter);
    if (s->private_ref)
      kvmap_unref(api, s->ref);
  }
  miter->nway = 0;
}

/**
 * 销毁多路归并迭代器
 * 释放所有资源和内存
 * @param miter 多路归并迭代器
 */
  void
miter_destroy(struct miter * const miter)
{
  miter_clean(miter);
  for (u32 i = 1; i <= MITER_MAX_STREAMS; i++) {
    if (miter->mh[i]) {
      free(miter->mh[i]);
      miter->mh[i] = NULL;
    } else {
      break;
    }
  }
  if (miter->ptr0)
    free(miter->ptr0);
  free(miter);
}
// }}} miter

// vim:fdm=marker
