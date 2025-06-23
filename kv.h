/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
/*
 * kv.h - 键值对 (Key-Value) 数据结构和操作接口
 *
 * 本文件定义了 RemixDB 中使用的键值对数据结构，包括：
 * - kv 结构体：存储键值对数据
 * - kref 结构体：键的引用结构
 * - kvref 结构体：键值对的引用结构
 * - 各种操作函数：创建、比较、复制、排序等
 * - 内存管理接口：用于不同内存管理策略
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// crc32c {{{ // CRC32C 校验和计算相关
#define KV_CRC32C_SEED ((0xDEADBEEFu)) // CRC32C 计算的种子值

// 计算数据的 CRC32C 校验和
  extern u32
kv_crc32c(const void * const ptr, u32 len);

// 将 32 位 CRC32C 扩展为 64 位哈希值
  extern u64
kv_crc32c_extend(const u32 crc32c);
// }}} crc32c

// kv {{{ // 键值对相关定义和函数

// struct {{{ // 数据结构定义
/*
 * 键值对结构体 - 存储一个键值对的完整信息
 *
 * 内部联合体名称可以忽略，主要结构为：
 * struct kv {
 *   u32 klen;     // 键的长度
 *   u32 vlen;     // 值的长度 (或引用计数)
 *   u64 hash;     // 键的哈希值
 *   u8 kv[];      // 键值数据：[key_data][value_data]
 * };
 */
struct kv {
  union { // 第一个 u64：存储键值长度信息
    u64 kvlen;    // 键值总长度 (klen + vlen)
    struct {
      u32 klen;   // 键长度
      union { u32 vlen; u32 refcnt; }; // 值长度 或 引用计数
    };
  };
  union {
    u64 hash;     // 键的哈希值
    u64 priv;     // 私有数据 (当不使用哈希时)
    void * privptr; // 私有指针
    struct { u32 hashlo; u32 hashhi; }; // 小端序：哈希值的低32位和高32位
    struct { u32 privlo; u32 privhi; }; // 私有数据的低32位和高32位
  };

  /** 这里的 kv[] 实际上是一个柔性数组成员，不分配空间, 表示键值对数据存储在此处。
    * 具体数据格式为：[key_data][value_data]，长度为 klen + vlen。
    * size_t total = sizeof(struct kv) + klen + vlen;
    * struct kv * item = malloc(total);
    */
  u8 kv[0];  // 键值数据：长度为 klen + vlen
} __attribute__((packed)); // 紧凑排列，无内存对齐

/*
 * 键引用结构体 - 引用一个键而不复制数据
 * 用于高效的键比较和查找操作
 */
struct kref {
  u32 len;        // 键的长度
  union { u32 hash32; u32 priv; }; // 32位哈希值 或 私有数据
  const u8 * ptr; // 指向键数据的只读指针
} __attribute__((packed));

/*
 * 键值对引用结构体 - 分别引用键和值而不复制数据
 * 用于高效访问键值对的各个部分
 */
struct kvref {
  const u8 * kptr; // 键数据的只读指针
  const u8 * vptr; // 值数据的只读指针
  struct kv hdr;   // 键值对头部信息 (注意：hdr.kv[] 无效)
};
// }}} struct

// kv {{{ // 键值对操作函数
// 键值对比较函数类型定义
typedef int  (*kv_kv_cmp_func)(const struct kv *, const struct kv *);

// === 大小计算函数 ===

// 获取键值对的总大小 (包括头部)
  extern size_t
kv_size(const struct kv * const kv);

// 获取键值对按指定对齐方式的大小
  extern size_t
kv_size_align(const struct kv * const kv, const u64 align);

// 获取键的大小 (仅包括键部分)
  extern size_t
key_size(const struct kv * const key);

// 获取键按指定对齐方式的大小
  extern size_t
key_size_align(const struct kv * const key, const u64 align);

// === 键值对构造和修改函数 ===

// 更新键值对的哈希值 (根据键计算)
  extern void
kv_update_hash(struct kv * const kv);

// 重新填充键值对的值部分
  extern void
kv_refill_value(struct kv * const kv, const void * const value, const u32 vlen);

// 重新填充整个键值对 (键和值)
  extern void
kv_refill(struct kv * const kv, const void * const key, const u32 klen,
    const void * const value, const u32 vlen);

// 使用字符串键重新填充键值对
  extern void
kv_refill_str(struct kv * const kv, const char * const key,
    const void * const value, const u32 vlen);

// 使用字符串键和字符串值重新填充键值对
  extern void
kv_refill_str_str(struct kv * const kv, const char * const key,
    const char * const value);

// 使用64位整数键重新填充键值对 (键以大端序存储)
  extern void
kv_refill_u64(struct kv * const kv, const u64 key, const void * const value, const u32 vlen);

  // 使用32位十六进制数作为键重新填充键值对
extern void
kv_refill_hex32(struct kv * const kv, const u32 hex, const void * const value, const u32 vlen);

// 使用64位十六进制数作为键重新填充键值对
  extern void
kv_refill_hex64(struct kv * const kv, const u64 hex, const void * const value, const u32 vlen);

// 使用64位十六进制数和指定键长度重新填充键值对
  extern void
kv_refill_hex64_klen(struct kv * const kv, const u64 hex, const u32 klen,
    const void * const value, const u32 vlen);

// 使用键引用重新填充键值对 (仅键部分)
  extern void
kv_refill_kref(struct kv * const kv, const struct kref * const kref);

// 使用键引用和值重新填充键值对
  extern void
kv_refill_kref_v(struct kv * const kv, const struct kref * const kref,
    const void * const value, const u32 vlen);

// === 键值对创建函数 ===

// 从键值对创建键引用
  extern struct kref
kv_kref(const struct kv * const key);

// 创建新的键值对 (分配内存)
  extern struct kv *
kv_create(const void * const key, const u32 klen, const void * const value, const u32 vlen);

// 使用字符串键创建键值对
  extern struct kv *
kv_create_str(const char * const key, const void * const value, const u32 vlen);

// 使用字符串键和字符串值创建键值对
  extern struct kv *
kv_create_str_str(const char * const key, const char * const value);

// 使用键引用创建键值对
  extern struct kv *
kv_create_kref(const struct kref * const kref, const void * const value, const u32 vlen);

// 返回一个静态的空键值对 (klen == 0)
  extern const struct kv *
kv_null(void);

// === 键值对复制函数 ===

// 复制键值对 (分配新内存)
  extern struct kv *
kv_dup(const struct kv * const kv);

// 仅复制键部分 (分配新内存)
  extern struct kv *
kv_dup_key(const struct kv * const kv);

// 复制键值对到指定位置
  extern struct kv *
kv_dup2(const struct kv * const from, struct kv * const to);

// 仅复制键到指定位置
  extern struct kv *
kv_dup2_key(const struct kv * const from, struct kv * const to);

// 复制键的前缀到指定位置
  extern struct kv *
kv_dup2_key_prefix(const struct kv * const from, struct kv * const to, const u32 plen);

// === 键值对比较和匹配函数 ===

// 比较两个键值对的键是否相等
  extern bool
kv_match(const struct kv * const key1, const struct kv * const key2);

// 比较两个键值对的键和哈希值是否相等
  extern bool
kv_match_hash(const struct kv * const key1, const struct kv * const key2);

// 比较两个键值对是否完全相等 (键和值都相等)
  extern bool
kv_match_full(const struct kv * const kv1, const struct kv * const kv2);

// 比较键值对与128字节格式的键值是否匹配
  extern bool
kv_match_kv128(const struct kv * const sk, const u8 * const kv128);

// 比较两个键值对的大小关系 (字典序)
  extern int
kv_compare(const struct kv * const kv1, const struct kv * const kv2);

// 比较键值对与128字节格式的键
  extern int
kv_k128_compare(const struct kv * const sk, const u8 * const k128);

// 比较键值对与128字节格式的键值对
  extern int
kv_kv128_compare(const struct kv * const sk, const u8 * const kv128);

// === 排序和分析函数 ===

// 对键值对数组进行快速排序
  extern void
kv_qsort(struct kv ** const kvs, const size_t nr);

// 计算两个键的最长公共前缀长度
  extern u32
kv_key_lcp(const struct kv * const key1, const struct kv * const key2);

// 跳过已知公共前缀，计算剩余的最长公共前缀长度
  extern u32
kv_key_lcp_skip(const struct kv * const key1, const struct kv * const key2, const u32 lcp0);

// 并行排序键值对数组
  extern void
kv_psort(struct kv ** const kvs, const u64 nr, const u64 tlo, const u64 thi);

// === 数据访问函数 ===

// 获取键值对的值指针 (可写)
  extern void *
kv_vptr(struct kv * const kv);

// 获取键值对的键指针 (可写)
  extern void *
kv_kptr(struct kv * const kv);

// 获取键值对的值指针 (只读)
  extern const void *
kv_vptr_c(const struct kv * const kv);

// 获取键值对的键指针 (只读)
  extern const void *
kv_kptr_c(const struct kv * const kv);

// === 调试和输出函数 ===

// 打印键值对信息到指定文件
  extern void
kv_print(const struct kv * const kv, const char * const cmd, FILE * const out);
// }}} kv

// mm {{{ // 内存管理 (Memory Management) 相关定义

// === 内存管理回调函数类型定义 ===

// 输入函数：为键值对创建私有副本 (用于 put 操作)
typedef struct kv * (* kvmap_mm_in_func)(struct kv * kv, void * priv);
// 输出函数：将私有副本复制到输出缓冲区 (用于 get 和 iter_peek 操作)
typedef struct kv * (* kvmap_mm_out_func)(struct kv * kv, struct kv * out);
// 释放函数：释放键值对内存 (用于 del 和 put 操作)
typedef void        (* kvmap_mm_free_func)(struct kv * kv, void * priv);

/*
 * 键值映射的内存管理结构体
 * 管理 kvmap 内部的键值对数据
 */
struct kvmap_mm {
  // 创建 "kv" 的私有副本 (参见 put() 函数)
  kvmap_mm_in_func in;
  // 将私有副本复制到 "out" (参见 get() 和 iter_peek() 函数)
  kvmap_mm_out_func out;
  // 释放键值对内存 (参见 del() 和 put() 函数)
  kvmap_mm_free_func free;
  void * priv;  // 私有数据指针
};

// === 预定义的内存管理函数 ===

// 空操作输入函数：直接返回原键值对，不创建副本
  extern struct kv *
kvmap_mm_in_noop(struct kv * const kv, void * const priv);

// 空操作输出函数：直接返回原键值对，不复制
  extern struct kv *
kvmap_mm_out_noop(struct kv * const kv, struct kv * const out);

// 空操作释放函数：不释放内存
  extern void
kvmap_mm_free_noop(struct kv * const kv, void * const priv);

// 复制输入函数：创建键值对的副本
  extern struct kv *
kvmap_mm_in_dup(struct kv * const kv, void * const priv);

// 复制输出函数：将键值对复制到输出缓冲区
  extern struct kv *
kvmap_mm_out_dup(struct kv * const kv, struct kv * const out);

// 标准释放函数：使用 free() 释放内存
  extern void
kvmap_mm_free_free(struct kv * const kv, void * const priv);

// === 预定义的内存管理策略 ===

// 默认内存管理：输入复制，输出复制，标准释放
extern const struct kvmap_mm kvmap_mm_dup; // in:Dup, out:Dup, free:Free
// 无复制输入，复制输出，标准释放
extern const struct kvmap_mm kvmap_mm_ndf; // in:Noop, out:Dup, free:Free
// }}} mm

// ref {{{ // 引用 (Reference) 相关函数

// 键引用与键值对比较函数类型定义
typedef int (*kref_kv_cmp_func)(const struct kref *, const struct kv *);

// === 键引用创建和初始化函数 ===

// 创建原始键引用 (仅设置指针和长度)
  extern void
kref_ref_raw(struct kref * const kref, const u8 * const ptr, const u32 len);

// 创建键引用并计算32位哈希值
  extern void
kref_ref_hash32(struct kref * const kref, const u8 * const ptr, const u32 len);

// 更新键引用的32位哈希值
  extern void
kref_update_hash32(struct kref * const kref);

// 从键值对创建键引用
  extern void
kref_ref_kv(struct kref * const kref, const struct kv * const kv);

// 从键值对创建键引用并计算32位哈希值
  extern void
kref_ref_kv_hash32(struct kref * const kref, const struct kv * const kv);

// === 键引用比较和匹配函数 ===

// 比较两个键引用是否相等
  extern bool
kref_match(const struct kref * const k1, const struct kref * const k2);

// 比较键引用与键值对的键是否相等
  extern bool
kref_kv_match(const struct kref * const kref, const struct kv * const k);

// 比较两个键引用的大小关系 (字典序)
  extern int
kref_compare(const struct kref * const kref1, const struct kref * const kref2);

// 比较键引用与键值对的键的大小关系
  extern int
kref_kv_compare(const struct kref * const kref, const struct kv * const k);

// === 键引用分析函数 ===

// 计算两个键引用的最长公共前缀长度
  extern u32
kref_lcp(const struct kref * const k1, const struct kref * const k2);

// 计算键引用与键值对键的最长公共前缀长度
  extern u32
kref_kv_lcp(const struct kref * const kref, const struct kv * const kv);

// 比较键引用与128字节格式的键
  extern int
kref_k128_compare(const struct kref * const sk, const u8 * const k128);

// 比较键引用与128字节格式的键值对
  extern int
kref_kv128_compare(const struct kref * const sk, const u8 * const kv128);

// === 特殊引用 ===

// 返回空键引用 (长度为0的键)
  extern const struct kref *
kref_null(void);

// === 键值对引用相关函数 ===

// 从键值对创建键值对引用
  extern void
kvref_ref_kv(struct kvref * const ref, struct kv * const kv);

// 将键值对引用复制为完整的键值对
  extern struct kv *
kvref_dup2_kv(struct kvref * const ref, struct kv * const to);

// 将键值对引用的键部分复制为键值对
  extern struct kv *
kvref_dup2_key(struct kvref * const ref, struct kv * const to);

// 比较键值对引用与键值对
  extern int
kvref_kv_compare(const struct kvref * const ref, const struct kv * const kv);
// }}} ref

// kv128 {{{ // 128字节格式编解码相关函数

// 估算键值对编码为128字节格式所需的大小
  extern size_t
kv128_estimate_kv(const struct kv * const kv);

// 将键值对编码为128字节格式
  extern u8 *
kv128_encode_kv(const struct kv * const kv, u8 * const out, size_t * const pesize);

// 从128字节格式解码为键值对
  extern struct kv *
kv128_decode_kv(const u8 * const ptr, struct kv * const out, size_t * const pesize);

// 获取128字节格式数据的大小
  extern size_t
kv128_size(const u8 * const ptr);
// }}} kv128

// }}} kv

// kvmap {{{ // 键值映射 (Key-Value Map) 接口

// kvmap_api {{{ // 键值映射API定义

// 就地操作函数类型：对当前键值对执行操作
typedef void (* kv_inp_func)(struct kv * const curr, void * const priv);

/*
 * 合并函数类型：用于读-修改-写操作
 * 合并函数应该：
 * 1: 如果原始键值对完全没有改变，返回 NULL
 * 2: 如果已就地应用更新，返回 kv0
 * 3: 如果必须替换原始键值对，返回不同的键值对
 * 在内存键值映射中，情况2==1，无需进一步操作
 * 在带内存表的持久键值存储中，如果 kv0 不来自内存表，情况2需要插入操作
 */
typedef struct kv * (* kv_merge_func)(struct kv * const kv0, void * const priv);

/*
 * 键值映射API结构体 - 定义键值映射的统一接口
 * 支持多种不同的键值存储实现
 */
struct kvmap_api {
  // === 特性标志 ===
  bool hashkey;    // true: 调用者需要在 kv/kref 中提供正确的哈希值
  bool ordered;    // true: 支持 iter_seek (有序遍历)
  bool threadsafe; // true: 支持线程安全访问
  bool readonly;   // true: 无 put() 和 del() 操作
  bool irefsafe;   // true: 迭代器的 kref/kvref 在 iter_seek/iter_skip/iter_park 后仍可安全访问
  bool unique;     // 提供唯一键，特别是对迭代器
  bool refpark;    // 引用支持 park() 和 resume()
  bool async;      // XXX 用于测试 KVell

  // === 基本操作函数 ===

  // 插入/更新：成功返回 true，错误返回 false
  // mm.in() 控制数据如何进入键值映射；默认内存管理器使用 malloc() 创建副本
  // mm.free() 控制被替换的旧键值对如何释放
  bool        (* put)     (void * const ref, struct kv * const kv);

  // 获取：搜索并返回键值对（如果找到），否则返回 NULL
  // 使用默认内存管理器：如果 out == NULL 则 malloc()；否则使用 out 作为缓冲区
  // 使用自定义 kvmap_mm：mm.out() 控制缓冲区；请谨慎使用
  // 调用者应使用返回的指针，即使提供了 out
  struct kv * (* get)     (void * const ref, const struct kref * const key, struct kv * const out);

  // 探测：找到返回 true，未找到返回 false
  bool        (* probe)   (void * const ref, const struct kref * const key);

  // 删除：删除了某项返回 true，未找到返回 false
  // mm.free() 控制被删除的旧键值对如何释放
  bool        (* del)     (void * const ref, const struct kref * const key);

  // 就地操作：如果键存在则执行就地操作；否则返回 false；即使键为 NULL，uf() 总是会执行
  // inpr/inpw 分别获取读/写锁
  // 注意：在 inpw() 中只能更改值
  bool        (* inpr)    (void * const ref, const struct kref * const key, kv_inp_func uf, void * const priv);
  bool        (* inpw)    (void * const ref, const struct kref * const key, kv_inp_func uf, void * const priv);

  // 合并：在旧/新键上执行 put+回调；另一个名称：读-修改-写
  // 成功返回 true；错误返回 false
  bool        (* merge)   (void * const ref, const struct kref * const key, kv_merge_func uf, void * const priv);

  // 范围删除：删除从 start（包含）到 end（不包含）的所有键
  u64         (* delr)    (void * const ref, const struct kref * const start, const struct kref * const end);

  // 持久化所有内容；仅适用于持久映射
  void        (* sync)    (void * const ref);

  // === 迭代器操作函数 ===
  // 线程安全迭代器的一般指导原则：
  // - 假设游标下的键是锁定/冻结/不可变的
  // - 一旦创建，必须调用 iter_seek 使其有效
  // - 引用的所有权转给迭代器，因此在 iter_destroy 前不应使用引用
  // - 基于一个引用创建和使用多个迭代器可能导致死锁

  void *      (* iter_create)   (void * const ref);                    // 创建迭代器
  void        (* iter_seek)     (void * const iter, const struct kref * const key);  // 移动游标到第一个 >= 搜索键的键
  bool        (* iter_valid)    (void * const iter);                   // 检查游标是否指向有效键
  struct kv * (* iter_peek)     (void * const iter, struct kv * const out);          // 返回当前键；如果 out != NULL 则复制到 out
  bool        (* iter_kref)     (void * const iter, struct kref * const kref);       // 类似 peek 但不复制；迭代器无效则返回 false
  bool        (* iter_kvref)    (void * const iter, struct kvref * const kvref);     // 类似 iter_kref 但也提供值
  u64         (* iter_retain)   (void * const iter);                   // 使当前迭代器的 kref 或 kvref 保持有效直到释放
  void        (* iter_release)  (void * const iter, const u64 opaque); // 释放保持
  void        (* iter_skip1)    (void * const iter);                   // 跳过一个元素
  void        (* iter_skip)     (void * const iter, const u32 nr);     // 跳过 nr 个元素
  struct kv * (* iter_next)     (void * const iter, struct kv * const out);          // iter_next == iter_peek + iter_skip1
  bool        (* iter_inp)      (void * const iter, kv_inp_func uf, void * const priv); // 如果当前键有效则执行就地操作
  void        (* iter_park)     (void * const iter);                   // 使迭代器无效以释放资源或锁
  void        (* iter_destroy)  (void * const iter);                   // 销毁迭代器

  // === 其他操作函数 ===
  void *      (* ref)     (void * map);        // 为映射创建引用（如果需要）；总是使用 kvmap_ref() 和 kvmap_unref()
  void *      (* unref)   (void * ref);        // 返回原始映射
  void        (* park)    (void * ref);        // 暂停访问而不 unref；稍后必须调用 resume
  void        (* resume)  (void * ref);        // 恢复引用的访问；必须与 park() 配对

  // === 不安全函数 ===
  void        (* clean)   (void * map);        // 清空映射
  void        (* destroy) (void * map);        // 删除所有内容
  void        (* fprint)  (void * map, FILE * const out); // 用于调试
};

/*
 * 键值映射API注册结构体
 * 用于注册不同的键值映射实现
 */
struct kvmap_api_reg {
  int nargs;                                    // 名称后的参数数量
  const char * name;                            // 实现名称
  const char * args_msg;                        // 参数说明信息
  // 多个API可能共享一个创建函数
  // 参数：名称（如 "rdb"），内存管理器（通常为 NULL），其余参数
  void * (*create)(const char *, const struct kvmap_mm *, char **);
  const struct kvmap_api * api;                 // API接口指针
};

// === API注册和帮助函数 ===

// 调用此函数注册键值映射API
  extern void
kvmap_api_register(const int nargs, const char * const name, const char * const args_msg,
    void * (*create)(const char *, const struct kvmap_mm *, char **), const struct kvmap_api * const api);

// 显示帮助信息
  extern void
kvmap_api_helper_message(void);

// API帮助函数
  extern int
kvmap_api_helper(int argc, char ** const argv, const struct kvmap_mm * const mm,
    const struct kvmap_api ** const api_out, void ** const map_out);
// }}} kvmap_api

// helpers {{{ // 辅助函数

// 就地操作：窃取键值对
  extern void
kvmap_inp_steal_kv(struct kv * const kv, void * const priv);

// === 引用管理辅助函数 ===
  extern void *
kvmap_ref(const struct kvmap_api * const api, void * const map);

  extern void *
kvmap_unref(const struct kvmap_api * const api, void * const ref);

// === 使用键值对作为参数的操作函数 ===
  extern struct kv *
kvmap_kv_get(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, struct kv * const out);

  extern bool
kvmap_kv_probe(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key);

  extern bool
kvmap_kv_put(const struct kvmap_api * const api, void * const ref,
    struct kv * const kv);

  extern bool
kvmap_kv_del(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key);

  extern bool
kvmap_kv_inpr(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_inp_func uf, void * const priv);

  extern bool
kvmap_kv_inpw(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_inp_func uf, void * const priv);

  extern bool
kvmap_kv_merge(const struct kvmap_api * const api, void * const ref,
    const struct kv * const key, kv_merge_func uf, void * const priv);

  extern u64
kvmap_kv_delr(const struct kvmap_api * const api, void * const ref,
    const struct kv * const start, const struct kv * const end);

// 使用键值对进行迭代器定位
  extern void
kvmap_kv_iter_seek(const struct kvmap_api * const api, void * const iter,
    const struct kv * const key);

// === 使用原始数据作为参数的操作函数 ===
  extern struct kv *
kvmap_raw_get(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, struct kv * const out);

  extern bool
kvmap_raw_probe(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr);

  extern bool
kvmap_raw_del(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr);

  extern bool
kvmap_raw_inpr(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, kv_inp_func uf, void * const priv);

  extern bool
kvmap_raw_inpw(const struct kvmap_api * const api, void * const ref,
    const u32 len, const u8 * const ptr, kv_inp_func uf, void * const priv);

// 使用原始数据进行迭代器定位
  extern void
kvmap_raw_iter_seek(const struct kvmap_api * const api, void * const iter,
    const u32 len, const u8 * const ptr);

// 转储所有键到文件描述符
  extern u64
kvmap_dump_keys(const struct kvmap_api * const api, void * const map, const int fd);
// }}} helpers

// }}} kvmap

// miter {{{ // 合并迭代器 (Merging Iterator)
/*
 * 通用合并迭代器 - 用于同时遍历多个键值映射
 *
 * 必需的API函数：
 * - iter_create, iter_seek, iter_peek, iter_skip, iter_destroy
 * - iter_kref, iter_kvref
 *
 * 可选的API函数（特定于API）：
 * - ref/unref
 * - iter_park
 * - resume/park （还需设置 api->refpark）
 *
 * 可选的API函数（性能优化）：
 * - api->unique （更快的 miter_skip_unique）
 * - iter_retain/iter_release （更少的 memcpy）
 */

struct miter; // 合并迭代器结构体（不透明）

// 创建合并迭代器
  extern struct miter *
miter_create(void);

// === 添加迭代器源 ===

// 调用者拥有引用和迭代器；合并迭代器不会销毁它们
// 与活跃的合并迭代器一起使用迭代器或引用可能导致未定义行为
  extern bool
miter_add_iter(struct miter * const miter, const struct kvmap_api * const api, void * const ref, void * const iter);

// 调用者拥有引用；合并迭代器将创建和销毁迭代器
// 与活跃的合并迭代器一起使用底层引用可能导致未定义行为
  extern void *
miter_add_ref(struct miter * const miter, const struct kvmap_api * const api, void * const ref);

// 合并迭代器将获取映射的引用，创建迭代器，并清理所有内容
// 注意在同一线程中使用另一个引用/迭代器
  extern void *
miter_add(struct miter * const miter, const struct kvmap_api * const api, void * const map);

// === 合并迭代器操作 ===

// 获取迭代器源的数量
  extern u32
miter_rank(struct miter * const miter);

// 定位到指定键
  extern void
miter_seek(struct miter * const miter, const struct kref * const key);

// 使用键值对定位
  extern void
miter_kv_seek(struct miter * const miter, const struct kv * const key);

// 检查是否有效
  extern bool
miter_valid(struct miter * const miter);

// 查看当前键值对
  extern struct kv *
miter_peek(struct miter * const miter, struct kv * const out);

// 获取当前键引用
  extern bool
miter_kref(struct miter * const miter, struct kref * const kref);

// 获取当前键值对引用
  extern bool
miter_kvref(struct miter * const miter, struct kvref * const kvref);

// 跳过一个元素
  extern void
miter_skip1(struct miter * const miter);

// 跳过多个元素
  extern void
miter_skip(struct miter * const miter, const u32 nr);

// 获取下一个键值对
  extern struct kv *
miter_next(struct miter * const miter, struct kv * const out);

// 跳过重复键（需要 api->unique）
  extern void
miter_skip_unique(struct miter * const miter);

// 获取下一个唯一键
  extern struct kv *
miter_next_unique(struct miter * const miter, struct kv * const out);

// 暂停迭代器
  extern void
miter_park(struct miter * const miter);

// 清理迭代器
  extern void
miter_clean(struct miter * const miter);

// 销毁迭代器
  extern void
miter_destroy(struct miter * const miter);
// }}} miter

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
