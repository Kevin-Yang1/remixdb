/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

#include "xdb.h"
#include "ctypes.h"
#include "kv.h"
#include "wh.h"
#include "sst.h"
#include "blkio.h"

// defs {{{ // 定义区域开始
#define XDB_COMP_CONC ((4)) // 最大压缩线程数
#define XDB_REJECT_SIZE_SHIFT ((4)) // 拒绝大小移位 (用于计算最大拒绝大小，例如 1/16)
#define WAL_BLKSZ ((PGSZ << 6)) // WAL 块大小 (通常 PGSZ 是 4KB, 所以这里是 256KB)
// }}} defs // 定义区域结束

// struct {{{ // 结构体定义区域开始
// 内存表对，用于管理不同版本的内存表 (WMT 和 IMT)
struct mt_pair {
  union {
    void * wmt; // 指向可写内存表 (Write MemTable)
    struct wormhole * wmt_wh; // Wormhole 实现的可写内存表
  };
  union {
    void * imt; // 指向不可变内存表 (Immutable MemTable)
    struct wormhole * imt_wh; // Wormhole 实现的不可变内存表
  };
  struct mt_pair * next; // 指向下一个版本的 mt_pair
};

// 预写日志 (Write-Ahead Log) 结构体
struct wal {
  u8 * buf;           // 当前写入的缓冲区 (通过 wring_acquire() 获取)
  u64 bufoff;         // 缓冲区内当前的偏移量 (字节)
  u64 woff;           // 文件中的写入偏移量 (PGSZ 的倍数)
  u64 soff;           // 上次同步的文件偏移量
  u64 write_user;     // 用户写入字节数统计 (追加时更新)
  u64 write_nbytes;   // 实际写入 WAL 文件字节数统计 (追加时更新)
  u64 version;        // WAL 版本号 (压缩时改变)

  int fds[2];         // 两个 WAL 文件描述符 (用于轮换)
  struct wring * wring; // 写环形缓冲区 (用于异步 I/O)
  u64 maxsz;          // WAL 文件最大大小
};

// XDB 数据库主结构体
struct xdb {
  // 第一行，确保高频访问成员在同一缓存行
  // volatile 告诉编译器：这个变量的值可能在程序不知道的情况下被改变（例如多线程、硬件、信号处理等），所以每次都必须从内存中重新读取，不允许优化缓存
  struct mt_pair * volatile mt_view; // 指向当前活动的内存表视图 (mt_pair)
  u64 padding1[7];                  // 缓存行填充

  u64 mtsz;                         // 当前内存表大小 (写者频繁更改)
  struct wal wal;                   // WAL 结构体 (写者频繁更改)
  // 非频繁访问成员
  void * mt1;                       // 内存表实例 1
  void * mt2;                       // 内存表实例 2
  u32 nr_workers;                   // 压缩工作线程数
  u32 co_per_worker;                // 每个压缩工作线程的协程数
  char * worker_cores;              // 压缩工作线程绑核配置
  pthread_t comp_pid;               // 压缩线程的线程 ID

  // 只读成员 (初始化后基本不变)
  u64 max_mtsz;                     // 内存表最大大小
  u64 max_rejsz;                    // 最大拒绝大小 (用于压缩时决定哪些分区需要合并回 WMT)
  struct qsbr * qsbr;               // QSBR (Quiescent State-Based Reclamation) 实例，用于无锁内存回收
  struct msstz * z;                 // 多层 SSTable 管理器 (Zone)
  struct mt_pair mt_views[4];       // 预分配的内存表视图 (用于版本切换)
  int logfd;                        // 日志文件描述符
  volatile bool running;            // 数据库运行状态标志
  bool tags;                        // 是否使用哈希标签 (用于加速点查)
  bool padding2[2];                 // 填充

  u64 padding3[7];                  // 缓存行填充
  spinlock lock;                    // 用于保护共享数据的自旋锁
};

// XDB 数据库引用结构体 (每个线程持有一个)
struct xdb_ref {
  struct xdb * xdb;                 // 指向 XDB 主结构体
  struct msstv * v;                 // SSTable 版本视图
  struct msstv_ref * vref;          // SSTable 版本视图的引用

  union {
    void * imt_ref;                 // 不可变内存表的引用
    struct wormhole * imt_ref_raw;
  };
  union {
    void * wmt_ref;                 // 可写内存表的引用
    struct wormref * wmt_ref_wh;
  };
  union {
    struct mt_pair * mt_view;       // 当前线程使用的内存表视图
    struct qsbr_ref qref;           // QSBR 引用 (用于线程注册)
  };
};

// XDB 迭代器结构体
struct xdb_iter {
  struct xdb_ref * db_ref;          // 指向数据库引用
  struct mt_pair * mt_view;         // 创建迭代器时使用的内存表视图版本
  struct miter * miter;             // 多路归并迭代器
  struct coq * coq_parked;          // 停放的协程队列 (用于迭代器 park/resume)
};
// }}} struct // 结构体定义区域结束

// misc {{{ // 杂项函数区域开始
// 定义 WMT 和 IMT 使用的 kvmap API
// wmt_api 只是一个用于调用的指向 kvmap_api_wormhole 的指针，实际实现在kvmap_api_wormhole中
// 加 static 表示这个变量只在当前这个 .c 文件中可见和有效
static const struct kvmap_api * wmt_api = &kvmap_api_wormhole; // 可写内存表使用 wormhole API
static const struct kvmap_api * imt_api = &kvmap_api_whunsafe; // 不可变内存表使用 whunsafe API (通常更快，因其不可变)

// XDB 加锁
  static inline void
xdb_lock(struct xdb * const xdb)
{
  spinlock_lock(&xdb->lock);
}

// XDB 解锁
  static inline void
xdb_unlock(struct xdb * const xdb)
{
  spinlock_unlock(&xdb->lock);
}

// 检查内存表或 WAL 是否已满
  static inline bool
xdb_mt_wal_full(struct xdb * const xdb)
{
  // 内存表已满 或 WAL 已满
  // 当此条件为真时：写者必须等待；压缩应该开始
  return (xdb->mtsz >= xdb->max_mtsz) || (xdb->wal.woff >= xdb->wal.maxsz);
}
// }}} misc // 杂项函数区域结束

// wal {{{ // WAL 相关函数区域开始
// 将 WAL 缓冲区刷新到磁盘 (持有锁时调用)
  static void
wal_flush(struct wal * const wal)
{
  if (wal->bufoff == 0) // 如果缓冲区为空，则无需刷新
    return;

  const size_t wsize = bits_round_up(wal->bufoff, 12); // 将写入大小向上取整到页边界 (通常是4KB的倍数)
  debug_assert(wsize <= WAL_BLKSZ);
  memset(wal->buf + wal->bufoff, 0, wsize - wal->bufoff); // 将缓冲区尾部未使用的部分清零
  wring_write_partial(wal->wring, (off_t)wal->woff, wal->buf, 0, (u32)wsize); // 通过 wring 异步写入数据
  wal->buf = wring_acquire(wal->wring); // 获取新的写缓冲区
  debug_assert(wal->buf);
  wal->bufoff = 0; // 重置缓冲区偏移
  wal->woff += wsize; // 更新文件写入偏移
  wal->write_nbytes += wsize; // 更新写入字节数统计

#define XDB_SYNC_SIZE ((1lu<<26)) // 定义同步阈值：64MB
  if ((wal->woff - wal->soff) >= XDB_SYNC_SIZE) { // 如果未同步的数据量达到阈值
    // 将 fsync 操作加入队列，但不等待其完成 (非用户请求的同步)
    wring_fsync(wal->wring);
    wal->soff = wal->woff; // 更新上次同步偏移
  }
}

// 刷新 WAL 缓冲区并同步到磁盘 (必须持有锁时调用)
  static void
wal_flush_sync(struct wal * const wal)
{
  wal_flush(wal); // 先刷新缓冲区

  if (wal->woff != wal->soff) { // 如果存在未同步的数据
    wring_fsync(wal->wring); // 执行 fsync
    wal->soff = wal->woff; // 更新上次同步偏移
  }
}

// 等待所有 WAL I/O 操作完成
  static void
wal_io_complete(struct wal * const wal)
{
  wring_flush(wal->wring); // 刷新 wring 中的所有挂起操作并等待完成
}

// 刷新 WAL 缓冲区，同步到磁盘，并等待操作完成
  static void
wal_flush_sync_wait(struct wal * const wal)
{
  wal_flush_sync(wal); // 刷新并同步
  // 等待完成
  wal_io_complete(wal);
}

// 向 WAL 追加一条 KV 记录 (必须在持有 xdb->lock 时调用)
  static void
wal_append(struct wal * const wal, const struct kv * const kv)
{
  debug_assert(kv);
  // 估计 KV 记录在 WAL 中的大小 (包括 CRC32C校验和)
  const size_t estsz = sst_kv_vi128_estimate(kv) + sizeof(u32);
  if ((estsz + wal->bufoff) > WAL_BLKSZ) // 如果当前缓冲区不足以容纳新记录
    wal_flush(wal); // 刷新当前缓冲区

  debug_assert(wal->buf);
  // 将 KV 记录编码到缓冲区
  u8 * const ptr = sst_kv_vi128_encode(wal->buf + wal->bufoff, kv);
  // 在值的后面写入键的 CRC 校验和
  *(u32 *)ptr = kv->hashlo;
  wal->bufoff += estsz; // 更新缓冲区偏移
  debug_assert(wal->bufoff <= WAL_BLKSZ);
}

// 打开 WAL 文件
  static bool
wal_open(struct wal * const wal, const char * const path)
{
  char * const fn = malloc(strlen(path) + 10); // 分配文件名缓冲区
  if (!fn)
    return false;

  sprintf(fn, "%s/wal1", path); // 构建 wal1 文件路径
  const int fd1 = open(fn, O_RDWR|O_CREAT, 00644); // 打开或创建 wal1
  if (fd1 < 0) {
    fprintf(stderr, "%s open %s failed\n", __func__, fn);
    goto fail_open1;
  }
  wal->fds[0] = fd1;

  sprintf(fn, "%s/wal2", path); // 构建 wal2 文件路径
  const int fd2 = open(fn, O_RDWR|O_CREAT, 00644); // 打开或创建 wal2
  if (fd2 < 0) {
    fprintf(stderr, "%s open %s failed\n", __func__, fn);
    goto fail_open2;
  }
  wal->fds[1] = fd2;

  // fd 可以在恢复期间被替换
  wal->wring = wring_create(fd1, WAL_BLKSZ, 32); // 创建 wring 实例 (初始使用 fd1)
  if (!wal->wring)
    goto fail_wring;

  wal->buf = wring_acquire(wal->wring); // 获取初始写缓冲区
  if (!wal->buf)
    goto fail_buf;

  free(fn); // 释放文件名缓冲区
  return true;

fail_buf: // 缓冲区获取失败处理
  wring_destroy(wal->wring);
  wal->wring = NULL;
fail_wring: // wring 创建失败处理
  close(fd2);
fail_open2: // fd2 打开失败处理
  close(fd1);
fail_open1: // fd1 打开失败处理
  free(fn);
  return false;
}

// 切换 WAL 文件 (必须持有锁时调用)
// 返回旧 WAL 文件的大小
  static u64
wal_switch(struct wal * const wal, const u64 version)
{
  wal_flush_sync_wait(wal); // 确保当前 WAL 数据已完全写入并同步
  const u64 woff0 = wal->woff; // 保存旧 WAL 的写入偏移 (即大小)
  // bufoff 已经被 wal_flush_sync_wait 置为 0
  wal->woff = 0; // 重置新 WAL 的写入偏移
  wal->soff = 0; // 重置新 WAL 的同步偏移

  // 交换文件描述符
  const int fd1 = wal->fds[0];
  wal->fds[0] = wal->fds[1];
  wal->fds[1] = fd1;
  wring_update_fd(wal->wring, wal->fds[0]); // 更新 wring 使用的文件描述符为新的 wal->fds[0]

  memcpy(wal->buf, &version, sizeof(version)); // 在新 WAL 文件开头写入版本号
  wal->bufoff = sizeof(version); // 更新缓冲区偏移
  wal->version = version; // 更新 WAL 版本号

  return woff0; // 返回旧 WAL 文件的大小
}

// 关闭 WAL
  static void
wal_close(struct wal * const wal)
{
  wal_flush_sync_wait(wal); // 确保所有数据已写入并同步
  wring_destroy(wal->wring); // 销毁 wring (销毁操作会调用 wring_flush)

  close(wal->fds[0]); // 关闭文件描述符
  close(wal->fds[1]);
}
// }}} wal // WAL 相关函数区域结束

// kv-alloc {{{ // KV 分配相关函数区域开始
// 为时间戳 (删除标记) 创建一个新的 KV 对象
// 分配一个额外的字节用于引用计数 (虽然这里没直接用，但可能是通用KV结构的一部分)
  static struct kv *
xdb_new_ts(const struct kref * const kref)
{
  const size_t sz = sizeof(struct kv) + kref->len; // 大小只包含键，没有值
  struct kv * const new = malloc(sz);
  debug_assert(new);
  new->klen = kref->len;
  new->vlen = SST_VLEN_TS; // 特殊的值长度，标记为时间戳 (删除)
  memcpy(new->kv, kref->ptr, kref->len); // 复制键内容
  new->hash = kv_crc32c_extend(kref->hash32); // 扩展哈希值 (具体原因待查，可能与内部哈希格式有关)
  return new;
}

// 复制一个现有的 KV 对象
  static struct kv *
xdb_dup_kv(const struct kv * const kv)
{
  const size_t sz = sst_kv_size(kv); // 获取 KV 对象的实际大小
  struct kv * const new = malloc(sz);
  debug_assert(new);
  memcpy(new, kv, sz); // 完整复制 KV 对象内容
  return new;
}
// }}} kv-alloc // KV 分配相关函数区域结束

// xdb_ref {{{ // XDB 引用管理区域开始
// 进入 XDB 引用临界区 (恢复 WMT 引用)
  static inline void
xdb_ref_enter(struct xdb_ref * const ref)
{
  if (ref->wmt_ref) // 如果存在 WMT 引用
    wmt_api->resume(ref->wmt_ref); // 恢复 WMT 引用 (例如，协程切换回来时)
}

// 离开 XDB 引用临界区 (停放 WMT 引用)
  static inline void
xdb_ref_leave(struct xdb_ref * const ref)
{
  if (ref->wmt_ref) // 如果存在 WMT 引用
    wmt_api->park(ref->wmt_ref); // 停放 WMT 引用 (例如，协程切换出去时)
}

// 释放 XDB 引用持有的所有资源
  static void
xdb_unref_all(struct xdb_ref * const ref)
{
  if (ref->v) { // 如果持有 SSTable 版本视图
    msstv_unref(ref->vref); // 释放 SSTable 版本视图的引用
    msstz_putv(ref->xdb->z, ref->v); // 将版本视图归还给 Zone 管理器
    ref->v = NULL;
    ref->vref = NULL;
  }

  if (ref->imt_ref) { // 如果持有 IMT 引用
    kvmap_unref(imt_api, ref->imt_ref); // 释放 IMT 引用
    ref->imt_ref = NULL;
  }

  if (ref->wmt_ref) { // 如果持有 WMT 引用
    kvmap_unref(wmt_api, ref->wmt_ref); // 释放 WMT 引用
    ref->wmt_ref = NULL;
  }
  cpu_cfence(); // CPU 内存屏障，确保之前的写操作对其他核心可见
  ref->mt_view = NULL; // 清空内存表视图指针
  // 不需要清除内存，因为这些是指针
}

// 获取 XDB 引用所需的所有资源 (调用此函数前必须已释放所有旧资源)
  static void
xdb_ref_all(struct xdb_ref * const ref)
{
  ref->mt_view = ref->xdb->mt_view; // 获取当前 XDB 的内存表视图
  ref->v = msstz_getv(ref->xdb->z); // 从 Zone 管理器获取最新的 SSTable 版本视图
  ref->vref = msstv_ref(ref->v); // 获取该版本视图的引用

  ref->wmt_ref = kvmap_ref(wmt_api, ref->mt_view->wmt); // 获取当前 WMT 的引用
  debug_assert(ref->wmt_ref);

  if (ref->mt_view->imt) { // 如果当前内存表视图包含 IMT
    ref->imt_ref = kvmap_ref(imt_api, ref->mt_view->imt); // 获取 IMT 的引用
    debug_assert(ref->imt_ref);
  }
  xdb_ref_leave(ref); // 初始时停放 WMT 引用
}

// 更新 XDB 引用的版本信息 (如果 XDB 的主版本已更新)
  static inline void
xdb_ref_update_version(struct xdb_ref * const ref)
{
  if (unlikely(ref->xdb->mt_view != ref->mt_view)) { // 如果 XDB 主视图与当前引用视图不一致
    xdb_unref_all(ref); // 释放旧版本的所有资源
    xdb_ref_all(ref);   // 获取新版本的所有资源
  }
}

// 创建并返回一个新的 XDB 引用
  struct xdb_ref *
xdb_ref(struct xdb * const xdb)
{
  struct xdb_ref * ref = calloc(1, sizeof(*ref)); // 分配并清零 XDB 引用结构体
  ref->xdb = xdb; // 指向 XDB 主结构体
  qsbr_register(xdb->qsbr, &ref->qref); // 向 QSBR 注册当前线程
  xdb_ref_all(ref); // 获取初始资源
  return ref;
}

// 释放一个 XDB 引用
  struct xdb *
xdb_unref(struct xdb_ref * const ref)
{
  struct xdb * xdb = ref->xdb; // 保存 XDB 主结构体指针
  xdb_unref_all(ref); // 释放引用持有的所有资源
  qsbr_unregister(xdb->qsbr, &ref->qref); // 从 QSBR 注销当前线程
  free(ref); // 释放 XDB 引用结构体本身
  return xdb;
}
// }}} xdb_ref // XDB 引用管理区域结束

// reinsert {{{ // 重插入逻辑区域开始 (用于将压缩时拒绝的键重新插入 WMT)
// 重插入合并操作的上下文结构体
struct xdb_reinsert_merge_ctx {
  struct kv * kv;   // 当前要重新插入的 KV 对象
  struct xdb * xdb; // XDB 主结构体指针
};

// 用于重插入的合并函数 (kv_merge_func 的实现)
  static struct kv *
xdb_mt_reinsert_func(struct kv * const kv0, void * const priv)
{
  struct xdb_reinsert_merge_ctx * const ctx = priv;
  if (kv0 == NULL) { // 如果 WMT 中不存在该键 (即新插入)
    struct kv * const ret = xdb_dup_kv(ctx->kv); // 复制要插入的 KV 对象
    debug_assert(ret);
    const size_t incsz = sst_kv_size(ret); // 计算增加的大小
    struct xdb * const xdb = ctx->xdb;
    xdb_lock(xdb); // 加锁保护共享数据
    xdb->mtsz += incsz; // 更新内存表大小
    wal_append(&xdb->wal, ret); // 将操作追加到 WAL
    xdb_unlock(xdb); // 解锁
    return ret; // 返回新插入的 KV 对象
  } else { // 如果 WMT 中已存在该键，则不覆盖 (重插入逻辑通常是针对 IMT 中未被 SST 接受的键)
    return kv0;
  }
}

// 将 IMT 中被拒绝的键重新插入到 WMT；vlen == 1 标记一个被拒绝的分区
  static void
xdb_reinsert_rejected(struct xdb * const xdb, void * const wmt_map, void * const imt_map, struct kv ** const anchors)
{
  void * const wmt_ref = kvmap_ref(wmt_api, wmt_map); // 获取 WMT 引用
  void * const rej_ref = kvmap_ref(imt_api, imt_map); // 获取 IMT (作为拒绝键来源) 的引用
  void * const rej_iter = imt_api->iter_create(rej_ref); // 创建 IMT 的迭代器
  struct xdb_reinsert_merge_ctx ctx = {.xdb = xdb}; // 初始化重插入上下文

  for (u32 i = 0; anchors[i]; i++) { // 遍历锚点 (SSTable 分区点)
    if (anchors[i]->vlen == 0) // 跳过被接受的分区 (vlen == 0 表示接受)
      continue;
    // 找到当前被拒绝分区的结束点
    if (anchors[i+1]) {
      struct kv * const kz = anchors[i+1]; // 下一个锚点作为结束键
      struct kref krefz;
      kref_ref_kv_hash32(&krefz, kz);
      imt_api->iter_seek(rej_iter, &krefz); // 定位迭代器到分区结束点之前
    }
    // peek 和 next 不会创建副本；参见 mm_mt.out
    struct kv * const end = anchors[i+1] ? imt_api->iter_peek(rej_iter, NULL) : NULL; // 获取分区结束键 (如果存在)
    struct kv * const k0 = anchors[i]; // 当前分区的起始键
    struct kref kref0;
    kref_ref_kv_hash32(&kref0, k0);
    imt_api->iter_seek(rej_iter, &kref0); // 定位迭代器到分区起始键
    while (imt_api->iter_valid(rej_iter)) { // 遍历分区内的键
      struct kv * const curr = imt_api->iter_next(rej_iter, NULL); // 获取下一个键 (无副本)
      if (curr == end) // 如果到达分区末尾，则停止
        break;

      if (!curr) // 不应该发生
        debug_die();
      ctx.kv = curr; // 设置上下文中的当前 KV
      // 将当前键合并到 WMT
      bool s = kvmap_kv_merge(wmt_api, wmt_ref, curr, xdb_mt_reinsert_func, &ctx);
      if (!s) // 合并失败则终止
        debug_die();
    }
  }
  imt_api->iter_destroy(rej_iter); // 销毁 IMT 迭代器
  kvmap_unref(imt_api, rej_ref);   // 释放 IMT 引用
  kvmap_unref(wmt_api, wmt_ref);   // 释放 WMT 引用
}
// }}} reinsert // 重插入逻辑区域结束

// comp {{{ // 压缩逻辑区域开始
// 压缩过程:
//   -** 持有 xdb 锁
//       - 将内存表模式从 wmt-only 切换到 wmt+imt (非常快)
//       - 同步刷新并切换日志文件
//   -** 释放 xdb 锁
//   - 等待 QSBR，确保所有用户已离开现在的 imt
//   - 保存旧版本，直到新版本准备好访问
//   - 调用 msstz_comp 执行 SSTable 压缩
//   - 释放 WAL 中的数据 (旧 WAL 中的数据已被处理)
//   - 对于每个被拒绝的键，如果它仍然是最新的，则将其重新插入到 wmt 并追加到新的 WAL
//       -** 对每个新的被拒绝键，持有/释放 xdb 锁
//   -** 持有 xdb 锁
//       - 刷新新的 WAL 并发送异步 fsync (非阻塞)
//   -** 释放 xdb 锁
//   - 释放锚点数组并释放旧版本
//   - 切换回正常模式 (仅 wmt)，因为 imt 中的键要么在 wmt 中，要么在 SSTable 分区中
//   - 等待 QSBR，确保所有用户已离开 imt
//   - 清理 imt (将在下一次压缩中用作新的 wmt)；TODO: 这步开销较大
//   -** 持有 xdb 锁
//       - 等待 fsync 完成；这确保了被拒绝的键已安全写入新的 WAL
//   -** 释放 xdb 锁
//   - 截断旧的 WAL；其所有数据已安全存储在 SSTable Zone 或新的 WAL 中
//   - 完成
  static void
xdb_do_comp(struct xdb * const xdb, const u64 max_rejsz)
{
  const double t0 = time_sec(); // 记录开始时间
  xdb_lock(xdb); // 加锁

  // 切换内存表视图 (mt_view)
  struct mt_pair * const v_comp = xdb->mt_view->next; // 获取下一个视图 (通常是 WMT+IMT 模式)
  xdb->mt_view = v_comp; // 将 XDB 的当前视图切换到压缩视图

  // 切换日志文件
  const u64 walsz0 = wal_switch(&xdb->wal, msstz_version(xdb->z) + 1); // 切换 WAL，版本号与下一个 SSTable Zone 版本匹配
  const u64 mtsz0 = xdb->mtsz; // 保存旧的内存表大小
  xdb->mtsz = 0; // 在持有锁的情况下重置内存表大小 (新的 WMT 开始计数)

  xdb_unlock(xdb); // 解锁

  void * const wmt_map = v_comp->wmt; // 当前的 WMT (在压缩视图中)
  void * const imt_map = v_comp->imt; // 当前的 IMT (即旧的 WMT，将被压缩)
  // 解锁状态
  qsbr_wait(xdb->qsbr, (u64)v_comp); // 等待所有线程离开旧的视图 (v_comp 之前的视图)

  struct msstv * const oldv = msstz_getv(xdb->z); // 获取当前的 SSTable 版本视图，并保持其存活
  const double t_prep = time_sec(); // 记录准备阶段结束时间

  // 执行 SSTable 压缩
  msstz_comp(xdb->z, imt_api, imt_map, xdb->nr_workers, xdb->co_per_worker, max_rejsz);
  const double t_comp = time_sec(); // 记录压缩阶段结束时间

  struct kv ** const anchors = msstv_anchors(oldv); // 获取旧版本视图的锚点 (用于重插入)
  xdb_reinsert_rejected(xdb, wmt_map, imt_map, anchors); // 将被拒绝的键重新插入 WMT
  const double t_reinsert = time_sec(); // 记录重插入阶段结束时间

  // 刷新并同步新的 WAL：旧的 WAL 将被截断
  xdb_lock(xdb);
  wal_flush_sync(&xdb->wal);
  xdb_unlock(xdb);

  free(anchors); // 释放锚点数组
  msstz_putv(xdb->z, oldv); // 归还旧的 SSTable 版本视图

  struct mt_pair * const v_normal = v_comp->next; // 获取下一个视图 (通常是 WMT-only 模式)
  xdb->mt_view = v_normal; // 切换回正常视图
  qsbr_wait(xdb->qsbr, (u64)v_normal); // 等待所有线程离开压缩视图 (v_comp)
  const double t_wait2 = time_sec(); // 记录第二次等待结束时间

  // QSBR 等待之后
  imt_api->clean(imt_map); // 清理 IMT (它将成为下一次压缩的 WMT)
  const double t_clean = time_sec(); // 记录清理阶段结束时间

  xdb_lock(xdb);
  wal_io_complete(&xdb->wal); // 等待新的 WAL 同步完成
  xdb_unlock(xdb);

  // I/O 完成后截断旧的 WAL
  logger_printf(xdb->logfd, "%s discard wal fd %d sz0 %lu\n", __func__, xdb->wal.fds[1], walsz0);
  ftruncate(xdb->wal.fds[1], 0); // 截断旧的 WAL 文件 (fds[1] 现在是旧的)
  fdatasync(xdb->wal.fds[1]);    // 确保截断操作持久化
  const double t_sync = time_sec(); // 记录同步截断操作结束时间

  // I/O 统计
  const size_t usr_write = xdb->wal.write_user;         // 用户写入字节数
  const size_t wal_write = xdb->wal.write_nbytes;       // WAL 实际写入字节数
  const size_t sst_write = msstz_stat_writes(xdb->z);   // SSTable 写入字节数
  const size_t sst_read = msstz_stat_reads(xdb->z);     // SSTable 读取字节数 (逻辑读，可能远大于物理读)
  // 写放大 (WA), 读放大 (RA)
  const double sys_wa = (double)(wal_write + sst_write) / (double)usr_write; // 系统写放大
  const double comp_ra = (double)sst_read / (double)usr_write;               // 压缩读放大

  const u64 mb = 1lu<<20; // 1MB
  logger_printf(xdb->logfd, "%s mtsz %lu walsz %lu write-mb usr %lu wal %lu sst %lu WA %.4lf comp-read-mb %lu RA %.4lf\n",
      __func__, mtsz0, walsz0, usr_write/mb, wal_write/mb, sst_write/mb, sys_wa, sst_read/mb, comp_ra);
  logger_printf(xdb->logfd, "%s times-ms total %.3lf prep %.3lf comp %.3lf reinsert %.3lf wait2 %.3lf clean %.3lf sync %.3lf\n",
      __func__, (t_sync-t0)*1000.0, (t_prep-t0)*1000.0, (t_comp-t_prep)*1000.0, (t_reinsert-t_comp)*1000.0, (t_wait2-t_reinsert)*1000.0, (t_clean-t_wait2)*1000.0, (t_sync-t_clean)*1000.0);
}

// 绑定压缩工作线程到指定 CPU核心
  static void
xdb_compaction_worker_pin(struct xdb * const xdb)
{
  if (!strcmp(xdb->worker_cores, "auto")) { // 自动检测模式
    u32 cores[64];
    const u32 ncores = process_getaffinity_list(64, cores); // 获取当前进程的 CPU 亲和性列表
    if (ncores < xdb->nr_workers)
      logger_printf(xdb->logfd, "%s WARNING: too few cores: %u cores < %u workers\n", __func__, ncores, xdb->nr_workers);

    const u32 nr = (ncores < XDB_COMP_CONC) ? ncores : XDB_COMP_CONC; // 确定实际使用的核心数
    if (nr == 0) { // 理论上不应发生
      logger_printf(xdb->logfd, "%s no cores\n", __func__);
    } else if (nr < ncores) { // 如果需要更新亲和性列表 (例如，只使用部分核心)
      u32 cpus[XDB_COMP_CONC];
      for (u32 i = 0; i < nr; i++)
        cpus[i] = cores[ncores - nr + i]; // 选择亲和性列表中的最后几个核心
      thread_setaffinity_list(nr, cpus); // 设置线程的 CPU 亲和性
      logger_printf(xdb->logfd, "%s cpus %u first %u (auto)\n", __func__, nr, cpus[0]);
    } else { // 继承父进程的亲和性设置
      logger_printf(xdb->logfd, "%s inherited\n", __func__);
    }
  } else if (strcmp(xdb->worker_cores, "dont")) { // 如果不是 "dont" (即指定了核心列表)
    char ** const tokens = strtoks(xdb->worker_cores, ","); // 解析核心列表字符串
    u32 nr = 0;
    u32 cpus[XDB_COMP_CONC];
    while ((nr < XDB_COMP_CONC) && tokens[nr]) {
      cpus[nr] = a2u32(tokens[nr]); // 将字符串转换为核心 ID
      nr++;
    }
    free(tokens);
    thread_setaffinity_list(nr, cpus); // 设置线程的 CPU 亲和性
    logger_printf(xdb->logfd, "%s pinning cpus %u arg %s\n", __func__, nr, xdb->worker_cores);
  } else { // "dont" 模式，不进行绑定
    logger_printf(xdb->logfd, "%s unpinned (dont)\n", __func__);
  }
  thread_set_name(pthread_self(), "xdb_comp"); // 设置线程名称为 "xdb_comp"
}

// XDB 压缩工作线程主函数
  static void *
xdb_compaction_worker(void * const ptr)
{
  struct xdb * const xdb = (typeof(xdb))ptr; // 获取 XDB 实例指针
  xdb_compaction_worker_pin(xdb); // 绑定 CPU 核心

  while (true) { // 主循环
    // 当数据库正在运行且不需要压缩时
    const u64 t0 = time_nsec();
    // 等待直到 (1) 内存表已满 或 (2) 日志文件已满
    while (xdb->running && !xdb_mt_wal_full(xdb))
      usleep(10000); // 休眠 10 毫秒 (原为 10 微秒，改为 10 毫秒以减少 CPU 占用)

    if (!xdb->running) // 如果数据库已停止运行，则退出循环
      break;

    const u64 dt = time_diff_nsec(t0); // 计算等待时间
    logger_printf(xdb->logfd, "%s compaction worker wait-ms %lu\n", __func__, dt / 1000000);
    xdb_do_comp(xdb, xdb->max_rejsz); // 执行压缩操作
  }

  pthread_exit(NULL); // 线程退出
}
// }}} comp // 压缩逻辑区域结束

// recover {{{ // 恢复逻辑区域开始
// WAL 中 KV 记录的临时结构体 (用于解码)
struct wal_kv {
  struct kref kref; // 键引用
  u32 vlen;         // 值长度
  u32 kvlen;        // 键值总长度 (不含头部)
};
// WAL 记录格式: [klen-vi128, vlen-vi128, key-data, value-data, crc32c-of-key]
// 如果成功，返回解码数据后的指针；否则返回 NULL
  static const u8 *
wal_vi128_decode(const u8 * ptr, const u8 * const end, struct wal_kv * const wal_kv)
{
  // 确保至少有足够字节解码 klen 和 vlen (vi128 最多10字节)
  const u32 safelen = (u32)((end - ptr) < 10 ? (end - ptr) : 10);
  u32 count = 0; // 计算 vi128 编码的数字个数
  for (u32 i = 0; i < safelen; i++) {
    if ((ptr[i] & 0x80) == 0) // vi128 中，字节最高位为0表示数字结束
      count++;
  }
  // 至少需要能解码出 klen 和 vlen 两个数字
  if (count < 2)
    return NULL;

  u32 klen, vlen;
  ptr = vi128_decode_u32(ptr, &klen); // 解码键长度
  ptr = vi128_decode_u32(ptr, &vlen); // 解码值长度
  const u32 kvlen_data = klen + (vlen & SST_VLEN_MASK); // 实际键值数据长度 (vlen 可能包含标记位)

  // 检查数据长度是否超出 WAL 缓冲区末尾
  if ((ptr + kvlen_data + sizeof(u32)) > end) // key_data + value_data + crc32c_of_key
    return NULL;

  // 校验和检查 (只校验键)
  const u32 sum1 = kv_crc32c(ptr, klen); // 计算键数据的 CRC32C
  const u32 sum2 = *(const u32 *)(ptr + kvlen_data); // 读取记录中存储的 CRC32C
  if (sum1 != sum2) // 如果校验和不匹配
    return NULL;

  wal_kv->kref.len = klen;
  wal_kv->kref.hash32 = sum2; // 使用记录中的校验和作为哈希值 (通常是键的哈希)
  wal_kv->kref.ptr = ptr;     // 指向键数据
  wal_kv->vlen = vlen;        // 保存原始值长度 (可能包含标记)
  wal_kv->kvlen = kvlen_data; // 保存键值数据总长度
  return ptr + kvlen_data + sizeof(u32); // 返回下一条记录的起始位置
}

// XDB 恢复合并操作的上下文结构体
struct xdb_recover_merge_ctx {
  struct kv * newkv; // 新的 KV 对象 (从 WAL 中恢复的)
  u64 mtsz;          // 当前内存表大小 (用于更新)
};

// 用于恢复的合并函数 (kv_merge_func 的实现)
// 在持有锁的情况下调用
  static struct kv *
xdb_recover_update_func(struct kv * const kv0, void * const priv)
{
  struct xdb_recover_merge_ctx * const ctx = priv;
  const size_t newsz = sst_kv_size(ctx->newkv); // 新 KV 对象的大小
  const size_t oldsz = kv0 ? sst_kv_size(kv0) : 0; // 旧 KV 对象的大小 (如果存在)
  const size_t diffsz = newsz - oldsz; // 大小差异
  debug_assert(ctx->mtsz >= oldsz); // 确保 mtsz 不会因减去 oldsz 而变为负数
  ctx->mtsz += diffsz; // 更新内存表大小
  return ctx->newkv; // 返回新的 KV 对象 (覆盖旧的)
}

// 从指定的 WAL 文件描述符恢复数据到内存表 (xdb->mt1)
// 使用 xdb->mt1, xdb->mtsz, xdb->z (用于日志记录)
  static u64
xdb_recover_fd(struct xdb * const xdb, const int fd)
{
  const u64 fsize = fdsize(fd); // 获取文件大小
  if (!fsize) // 如果文件为空，则无需恢复
    return 0;

  // 将 WAL 文件映射到内存
  u8 * const mem = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mem == MAP_FAILED) // 映射失败则中止恢复
    return 0;

  void * const wmt_ref = wmt_api->ref(xdb->mt1); // 获取内存表 (mt1) 的引用
  const u8 * iter = mem + sizeof(u64); // 跳过文件开头的版本号
  const u8 * const end = mem + fsize; // 文件末尾指针
  u64 nkeys = 0; // 恢复的键计数
  struct xdb_recover_merge_ctx ctx = {.mtsz = xdb->mtsz}; // 初始化恢复上下文

  while ((iter < end) && ((*iter) == 0)) // 跳过头部的填充零
    iter++;
  while (iter < end) { // 遍历 WAL 文件内容
    struct wal_kv wal_kv;
    const u8 * const iter1 = wal_vi128_decode(iter, end, &wal_kv); // 解码一条 WAL 记录

    // 如果解码失败 (例如到达文件末尾或数据损坏)，则停止
    if (!iter1)
      break;

    // 将解码的 WAL 记录插入到内存表
    struct kv * const kv = malloc(sizeof(struct kv) + wal_kv.kvlen); // 分配 KV 对象内存
    debug_assert(kv);
    kv->klen = wal_kv.kref.len;
    kv->vlen = wal_kv.vlen;
    kv->hash = kv_crc32c_extend(wal_kv.kref.hash32);
    memcpy(kv->kv, wal_kv.kref.ptr, wal_kv.kvlen); // 复制键值数据
    ctx.newkv = kv; // 设置上下文中的新 KV
    // 合并到内存表
    bool s = wmt_api->merge(wmt_ref, &wal_kv.kref, xdb_recover_update_func, &ctx);
    if (!s) // 合并失败则终止
      debug_die();

    iter = iter1; // 更新迭代器指针到下一条记录
    nkeys++;
    // 跳过记录间的填充零
    while ((iter < end) && ((*iter) == 0))
      iter++;
  }

  xdb->mtsz = ctx.mtsz; // 更新 XDB 的内存表大小
  wmt_api->unref(wmt_ref); // 释放内存表引用
  munmap(mem, fsize); // 解除内存映射
  const u64 rsize = (u64)(iter - mem); // 实际读取和处理的字节数
  logger_printf(xdb->logfd, "%s fd %d fsize %lu rsize %lu nkeys %lu\n", __func__, fd, fsize, rsize, nkeys);
  return rsize; // 返回处理的字节数
}

// XDB WAL 恢复主逻辑 (xdb 的 wal 成员必须已初始化为零)
  static void
xdb_wal_recover(struct xdb * const xdb)
{
  struct wal * const wal = &xdb->wal;
  u64 vs[2] = {}; // 用于存储两个 WAL 文件的版本号
  for (u32 i = 0; i < 2; i++) {
    if (fdsize(wal->fds[i]) > sizeof(u64)) // 如果文件大小足够包含版本号
      pread(wal->fds[i], &vs[i], sizeof(vs[i]), 0); // 读取版本号
  }

  const bool two = vs[0] && vs[1]; // 检查两个 WAL 文件是否都有有效的版本号
  const u64 v0 = msstz_version(xdb->z); // 获取当前 SSTable Zone 的版本号
  debug_assert(v0); // Zone 版本号不应为0
  logger_printf(xdb->logfd, "%s wal1 %lu wal2 %lu zv %lu\n", __func__, vs[0], vs[1], v0);

  // 目标是首先恢复较新的 WAL (fds[0])，如果需要，再恢复较旧的 (fds[1])
  // 然后继续使用 fds[0]，因为它可能仍然是半满的
  if (vs[0] < vs[1]) { // 如果 wal1 的版本号小于 wal2 (即 wal2 更新)
    logger_printf(xdb->logfd, "%s use wal2 %lu\n", __func__, vs[1]);
    wal->version = vs[1]; // 将当前 WAL 版本设置为 wal2 的版本
    // 交换 fds，使 fds[0] 指向较新的 WAL (即原来的 wal2)
    const int fd1_tmp = wal->fds[0];
    wal->fds[0]= wal->fds[1];
    wal->fds[1] = fd1_tmp;
    wring_update_fd(wal->wring, wal->fds[0]); // 更新 wring 使用的文件描述符
  } else { // wal1 较新或与 wal2 版本相同 (或 wal2 无效)
    logger_printf(xdb->logfd, "%s use wal1 %lu\n", __func__, vs[0]);
    wal->version = vs[0]; // 将当前 WAL 版本设置为 wal1 的版本
  }

  debug_assert(wal->wring && wal->buf); // 确保 wring 和缓冲区已初始化

  if (two) { // 如果两个 WAL 文件都有效 (通常发生在崩溃前正在切换 WAL)
    if (vs[0] == vs[1]) // 两个 WAL 版本号不应相同
      debug_die();
    // 恢复较旧的 WAL (现在是 fds[1])，然后恢复较新的 WAL (fds[0])
    const u64 r1 = xdb_recover_fd(xdb, wal->fds[1]); // 扫描较旧的
    const u64 r0 = xdb_recover_fd(xdb, wal->fds[0]); // 扫描较新的
    // 将所有恢复的数据压缩到 SSTable Zone，不拒绝任何分区
    msstz_comp(xdb->z, imt_api, xdb->mt1, xdb->nr_workers, xdb->co_per_worker, 0);
    // 新版本已安全存盘，可以清空 WAL 文件
    ftruncate(wal->fds[1], 0); fdatasync(wal->fds[1]);
    ftruncate(wal->fds[0], 0); fdatasync(wal->fds[0]);
    imt_api->clean(xdb->mt1); // 清理内存表 (mt1)
    xdb->mtsz = 0; // 重置内存表大小
    // 开始一个新的 WAL
    const u64 v1 = msstz_version(xdb->z); // 获取压缩后的新 Zone 版本
    memcpy(wal->buf, &v1, sizeof(v1)); // 在 WAL 缓冲区写入新版本号
    wal->bufoff = sizeof(v1);
    wal->version = v1;
    logger_printf(xdb->logfd, "%s wal comp zv0 %lu zv1 %lu rec %lu %lu mtsz %lu fd0 %d\n",
        __func__, v0, v1, r1, r0, xdb->mtsz, wal->fds[0]);
  } else { // 只有一个有效 WAL 或两个都无效
    const u64 rsize = xdb_recover_fd(xdb, wal->fds[0]); // 尝试从 fds[0] 恢复
    if (rsize == 0) { // 如果 fds[0] 为空或恢复失败，则为新的空 WAL 文件设置版本
      memcpy(wal->buf, &v0, sizeof(v0)); // 使用当前 Zone 版本
      wal->bufoff = sizeof(v0);
      wal->version = v0;
      logger_printf(xdb->logfd, "%s wal empty v %lu mtsz %lu fd %d\n", __func__, v0, xdb->mtsz, wal->fds[0]);
    } else { // 如果成功从 fds[0] 恢复了数据，则重用现有的 WAL
      // 只有一个 WAL 时：WAL 版本应小于等于 Zone 版本
      if (wal->version > v0)
        debug_die();
      // woff 必须页对齐
      wal->woff = bits_round_up(rsize, 12); // 将写入偏移向上取整到页边界
      if (wal->woff > rsize) { // 如果存在间隙，需要用零填充
        const u64 nr_zeros = wal->woff - rsize;
        u8 zeroes[PGSZ]; // 假设 PGSZ 足够小
        memset(zeroes, 0, nr_zeros > PGSZ ? PGSZ : nr_zeros); // 避免溢出
        // 此处应循环写入，如果 nr_zeros > PGSZ
        for (u64 written = 0; written < nr_zeros; ) {
            u64 to_write = nr_zeros - written;
            if (to_write > PGSZ) to_write = PGSZ;
            pwrite(wal->fds[0], zeroes, to_write, (off_t)(rsize + written));
            written += to_write;
        }
        fdatasync(wal->fds[0]);
      }
      logger_printf(xdb->logfd, "%s wal rsize %lu woff %lu mtsz %lu fd %d\n", __func__, rsize, wal->woff, xdb->mtsz, wal->fds[0]);
    }
    ftruncate(wal->fds[1], 0); // 无论如何都截断第二个 WAL 文件 (fds[1])
    fdatasync(wal->fds[1]);
  }
  wal->soff = wal->woff; // 将同步偏移设置为当前写入偏移
}
// }}} recover // 恢复逻辑区域结束

// open close {{{ // 打开/关闭数据库函数区域开始
// 打开 XDB 数据库
  struct xdb *
xdb_open(const char * const dir,             // 数据库目录
    const size_t cache_size_mb,         // SSTable 缓存大小 (MB)
    const size_t mt_size_mb,            // 内存表大小 (MB)
    const size_t wal_size_mb,           // WAL 文件大小 (MB)
    const bool ckeys,                   // 是否为 SSTable 生成压缩键 (ckeys)
    const bool tags,                    // 是否使用哈希标签
    const u32 nr_workers,               // 压缩工作线程数
    const u32 co_per_worker,            // 每个压缩工作线程的协程数
    const char * const worker_cores)    // 压缩工作线程绑核配置字符串
{
  mkdir(dir, 00755); // 创建数据库目录 (如果不存在)
  struct xdb * const xdb = yalloc(sizeof(*xdb)); // 分配 XDB 主结构体内存 64字节对齐 (典型缓存行大小)
  if (!xdb)
    return NULL;

  memset(xdb, 0, sizeof(*xdb)); // 初始化为零

  // 定义内存表使用的内存管理回调 (这里使用 no-op，表示由 wormhole 内部管理)
  const struct kvmap_mm mm_mt = { .in = kvmap_mm_in_noop, .out = kvmap_mm_out_noop, .free = kvmap_mm_free_free};

  xdb->mt1 = wormhole_create(&mm_mt); // 创建内存表实例 1
  xdb->mt2 = wormhole_create(&mm_mt); // 创建内存表实例 2

  // 初始化内存表视图链表 (用于版本切换)
  // 视图0: WMT=mt1, IMT=NULL (正常模式) -> 指向视图1
  xdb->mt_views[0] = (struct mt_pair){.wmt = xdb->mt1, .next = &xdb->mt_views[1]};
  // 视图1: WMT=mt2, IMT=mt1 (压缩 mt1 模式) -> 指向视图2
  xdb->mt_views[1] = (struct mt_pair){.wmt = xdb->mt2, .imt = xdb->mt1, .next = &xdb->mt_views[2]};
  // 视图2: WMT=mt2, IMT=NULL (正常模式) -> 指向视图3
  xdb->mt_views[2] = (struct mt_pair){.wmt = xdb->mt2, .next = &xdb->mt_views[3]};
  // 视图3: WMT=mt1, IMT=mt2 (压缩 mt2 模式) -> 指向视图0 (形成环)
  xdb->mt_views[3] = (struct mt_pair){.wmt = xdb->mt1, .imt = xdb->mt2, .next = &xdb->mt_views[0]};
  xdb->mt_view = xdb->mt_views; // 初始视图为 mt_views[0]

  xdb->z = msstz_open(dir, cache_size_mb, ckeys, tags); // 打开 SSTable Zone 管理器
  xdb->qsbr = qsbr_create(); // 创建 QSBR 实例

  // 只是一个警告
  if ((mt_size_mb * 2) > wal_size_mb)
    fprintf(stderr, "%s wal_size < mt_size*2\n", __func__);

  // 设置大小参数
  xdb->max_mtsz = mt_size_mb << 20; // 最大内存表大小 (字节)
  xdb->wal.maxsz = wal_size_mb << 20; // 最大 WAL 文件大小 (字节)
  xdb->max_rejsz = xdb->max_mtsz >> XDB_REJECT_SIZE_SHIFT; // 最大拒绝大小

  spinlock_init(&xdb->lock); // 初始化自旋锁
  xdb->nr_workers = nr_workers; // 设置压缩工作线程数
  xdb->co_per_worker = co_per_worker; // 设置每个工作线程的协程数
  xdb->worker_cores = strdup(worker_cores); // 复制绑核配置字符串
  xdb->logfd = msstz_logfd(xdb->z); // 获取 Zone 管理器的日志文件描述符
  xdb->running = true; // 设置数据库运行状态为 true
  xdb->tags = tags;    // 设置是否使用标签

  const bool wal_ok = wal_open(&xdb->wal, dir); // 打开 WAL 文件
  // 检查所有关键组件是否初始化成功
  const bool all_ok = xdb->mt1 && xdb->mt2 && xdb->z && xdb->qsbr && wal_ok;
  if (all_ok) {
    xdb_wal_recover(xdb); // 执行 WAL 恢复 (恢复过程不应出错)

    // 启动主压缩工作线程
    pthread_create(&xdb->comp_pid, NULL, xdb_compaction_worker, xdb); // 应该返回 0 表示成功
    return xdb; // 返回 XDB 实例指针
  } else { // 如果初始化失败
    if (xdb->mt1) wmt_api->destroy(xdb->mt1);
    if (xdb->mt2) wmt_api->destroy(xdb->mt2);
    if (xdb->z) msstz_destroy(xdb->z);
    if (xdb->qsbr) qsbr_destroy(xdb->qsbr);
    if (wal_ok) wal_close(&xdb->wal);
    if (xdb->worker_cores) free(xdb->worker_cores);
    free(xdb); // 释放 XDB 主结构体内存
    return NULL;
  }
}

// 关闭并销毁 XDB 数据库
  void
xdb_close(struct xdb * xdb)
{
  xdb->running = false; // 设置运行状态为 false，通知压缩线程退出
  pthread_join(xdb->comp_pid, NULL); // 等待压缩线程结束

  // 假设所有用户线程已离开
  qsbr_destroy(xdb->qsbr); // 销毁 QSBR 实例

  msstz_destroy(xdb->z); // 销毁 SSTable Zone 管理器
  wal_close(&xdb->wal); // 关闭 WAL
  wmt_api->destroy(xdb->mt1); // 销毁内存表实例 1
  wmt_api->destroy(xdb->mt2); // 销毁内存表实例 2
  free(xdb->worker_cores); // 释放绑核配置字符串内存
  free(xdb); // 释放 XDB 主结构体内存
}
// }}} open close // 打开/关闭数据库函数区域结束

// get probe {{{ // Get/Probe 操作函数区域开始
// Get 操作的辅助信息结构体
struct xdb_get_info {
  struct kv * out; // 用户提供的输出缓冲区 (可选)
  struct kv * ret; // 返回的 KV 对象指针 (如果找到且非删除标记)
};

// 用于 kvmap_api 的 inpr (in-place read) 回调函数 (Get 操作)
  static void
xdb_inp_get(struct kv * const kv, void * const priv)
{
  // 查看此键时进行复制，以避免 get 返回后出现一致性问题
  struct xdb_get_info * const info = (typeof(info))priv;
  if (kv && kv->vlen != SST_VLEN_TS) { // 如果找到键且不是删除标记
    info->ret = kvmap_mm_out_ts(kv, info->out); // 复制 KV 到输出缓冲区 (如果提供) 或新分配内存
  } else { // 未找到或为删除标记
    info->ret = NULL;
  }
}

// 从数据库获取指定键的值
  struct kv *
xdb_get(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const out)
{
  xdb_ref_update_version(ref); // 更新线程的数据库版本视图
  xdb_ref_enter(ref); // 进入临界区 (恢复 WMT 引用)

  // 首先在 WMT (可写内存表) 中查找
  struct xdb_get_info info = {out, NULL};
  if (wmt_api->inpr(ref->wmt_ref, kref, xdb_inp_get, &info)) { // 如果 WMT 处理了请求 (找到或确定不存在于 WMT)
    xdb_ref_leave(ref); // 离开临界区
    return info.ret; // 返回结果
  }
  xdb_ref_leave(ref); // 离开临界区

  // 如果 WMT 中未找到，则在 IMT (不可变内存表) 中查找
  if (ref->imt_ref) {
    if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_get, &info))
      return info.ret;
  }
  // 如果内存表中都未找到，则在 SSTables 中查找
  return msstv_get_ts(ref->vref, kref, out);
}

// 用于 kvmap_api 的 inpr 回调函数 (Probe 操作)
  static void
xdb_inp_probe(struct kv * const kv, void * const priv)
{
  // 仅判断键是否存在且非删除标记
  *(bool *)priv = kv && (kv->vlen != SST_VLEN_TS);
}

// 探测数据库中是否存在指定的键 (不返回值)
  bool
xdb_probe(struct xdb_ref * const ref, const struct kref * const kref)
{
  xdb_ref_update_version(ref); // 更新线程的数据库版本视图
  xdb_ref_enter(ref); // 进入临界区

  bool is_valid;
  // 首先在 WMT 中探测
  if (wmt_api->inpr(ref->wmt_ref, kref, xdb_inp_probe, &is_valid)) {
    xdb_ref_leave(ref); // 离开临界区
    return is_valid; // 返回结果
  }
  xdb_ref_leave(ref); // 离开临界区

  // 如果 WMT 中未找到，则在 IMT 中探测
  if (ref->imt_ref) {
    if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_probe, &is_valid))
      return is_valid;
  }
  // 如果内存表中都未找到，则在 SSTables 中探测
  return msstv_probe_ts(ref->vref, kref);
}
// }}} get probe // Get/Probe 操作函数区域结束

// put del {{{ // Put/Delete 操作函数区域开始
// 写操作进入前的等待逻辑 (如果内存表或 WAL 已满)
  static void
xdb_write_enter(struct xdb_ref * const ref)
{
  struct xdb * const xdb = ref->xdb;
  while (xdb_mt_wal_full(xdb)) { // 当内存表或 WAL 满时循环等待
    xdb_ref_update_version(ref); // 尝试更新版本 (可能其他线程已完成压缩)
    usleep(10000); // 休眠 10 毫秒 (原为 10 微秒)
  }
}

// 内存表合并操作的上下文结构体
struct xdb_mt_merge_ctx {
  struct kv * newkv;        // 要合并的新 KV 对象
  struct xdb * xdb;         // XDB 主结构体指针
  struct mt_pair * mt_view; // 操作时预期的内存表视图
  bool success;             // 操作是否成功
};

// 用于内存表更新的合并函数 (kv_merge_func 的实现)
// 在持有锁的情况下调用
  static struct kv *
xdb_mt_update_func(struct kv * const kv0, void * const priv)
{
  struct xdb_mt_merge_ctx * const ctx = priv; // 合并上下文
  struct xdb * const xdb = ctx->xdb;
  const size_t newsz = sst_kv_size(ctx->newkv); // 新 KV 对象的大小
  const size_t oldsz = kv0 ? sst_kv_size(kv0) : 0; // 旧 KV 对象的大小 (如果存在)
  const size_t diffsz = newsz - oldsz; // 大小差异

  xdb_lock(xdb); // 加锁保护共享数据
  if (unlikely(xdb->mt_view != ctx->mt_view)) { // 检查操作期间内存表视图是否已改变 (例如发生压缩切换)
    // 如果视图已改变，则中止操作
    xdb_unlock(xdb);
    return NULL; // 返回 NULL 表示操作失败，需要重试
  }
  debug_assert(xdb->mtsz >= oldsz);
  xdb->mtsz += diffsz; // 更新内存表大小
  xdb->wal.write_user += newsz; // 更新用户写入字节数统计
  wal_append(&xdb->wal, ctx->newkv); // 将新 KV 追加到 WAL

  xdb_unlock(xdb); // 解锁
  ctx->success = true; // 标记操作成功
  return ctx->newkv; // 返回新 KV 对象
}

// 通用的更新操作 (用于 Put 和 Delete)
  static bool
xdb_update(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const newkv)
{
  debug_assert(kref && newkv);
  xdb_write_enter(ref); // 等待写条件满足 (内存表/WAL 未满)

  struct xdb_mt_merge_ctx ctx = {newkv, ref->xdb, NULL, false}; // 初始化合并上下文
  bool s; // 操作结果
  do {
    xdb_ref_update_version(ref); // 更新线程的数据库版本视图
    xdb_ref_enter(ref); // 进入临界区
    ctx.mt_view = ref->mt_view; // 记录当前操作的内存表视图
    // 尝试将 newkv 合并到 WMT
    s = wmt_api->merge(ref->wmt_ref, kref, xdb_mt_update_func, &ctx);
    xdb_ref_leave(ref); // 离开临界区
  } while (s && !ctx.success); // 如果 merge 调用成功但内部更新失败 (视图改变)，则重试
  return s; // 返回操作是否成功
}

// 向数据库插入或更新一个键值对
  bool
xdb_put(struct xdb_ref * const ref, const struct kv * const kv)
{
  struct kv * const newkv = xdb_dup_kv(kv); // 复制用户提供的 KV 对象
  if (!newkv) // 复制失败
    return false;

  struct kref kref;
  kref_ref_kv(&kref, kv); // 从 KV 对象创建键引用
  return xdb_update(ref, &kref, newkv); // 执行更新操作
}

// 从数据库删除一个键
  bool
xdb_del(struct xdb_ref * const ref, const struct kref * const kref)
{
  struct kv * const ts_kv = xdb_new_ts(kref); // 创建一个删除标记 (时间戳 KV)
  if (!ts_kv) // 创建失败
    return false;

  return xdb_update(ref, kref, ts_kv); // 执行更新操作 (写入删除标记)
}

// 将 WAL 缓冲区的数据同步到磁盘
  void
xdb_sync(struct xdb_ref * const ref)
{
  struct xdb * const xdb = ref->xdb;
  xdb_lock(xdb); // 加锁
  wal_flush_sync_wait(&xdb->wal); // 刷新、同步并等待 WAL 操作完成
  xdb_unlock(xdb); // 解锁
}
// }}} put del // Put/Delete 操作函数区域结束

// merge {{{ // Merge (Read-Modify-Write) 操作函数区域开始
// 获取旧值的辅助函数 (用于 Merge 操作)
// 调用者需要释放返回的 KV 对象
  static struct kv *
xdb_merge_get_old(struct xdb_ref * const ref, const struct kref * const kref)
{
  struct xdb_get_info info = {NULL, NULL}; // 不使用输出缓冲区
  // 首先在 IMT 中查找 (因为 WMT 中的值会在主合并逻辑中处理)
  if (ref->imt_ref) {
    if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_get, &info))
      return info.ret;
  }
  // 如果 IMT 中未找到，则在 SSTables 中查找
  struct kv * const ret = msstv_get_ts(ref->vref, kref, NULL);
  if (ret) // 如果找到，确保哈希值正确 (msstv_get_ts 可能不填充完整哈希)
    ret->hash = kv_crc32c_extend(kref->hash32);
  return ret;
}

// Read-Modify-Write (RMW) 操作的上下文结构体
struct xdb_rmw_ctx {
  struct xdb_mt_merge_ctx mt_ctx; // 内存表合并上下文 (newkv, xdb, mt_view, success)
  kv_merge_func uf;               // 用户提供的合并函数 (User Function)
  void * const priv;              // 用户提供的私有数据 (传递给 uf)
  struct kv * oldkv;              // 从 IMT 或 SSTable 获取的旧值 (仅用于 func2)
  bool merged;                    // 标记合并操作是否已完成 (仅用于 func1)
};

// RMW 的核心合并逻辑 (kv_merge_func 的实现)
  static struct kv *
xdb_merge_merge_func(struct kv * const kv0, void * const priv)
{
  struct xdb_rmw_ctx * const ctx = priv;
  // kv0 是从 WMT 中获取的当前值，ctx->oldkv 是从 IMT/SST 获取的旧值
  // 如果 kv0 存在，则使用 kv0；否则使用 ctx->oldkv
  struct kv * const oldkv_for_uf = kv0 ? kv0 : ctx->oldkv;
  struct kv * const ukv = ctx->uf(oldkv_for_uf, ctx->priv); // 调用用户合并函数生成新值

  if (ukv == NULL) { // 如果用户函数返回 NULL，表示只读或删除操作完成
    ctx->merged = true; // 标记合并完成
    return NULL; // 返回 NULL 给 WMT 的 merge，表示不修改 WMT 中的当前项 (或删除)
  }

  // 如果用户函数返回了新值，则准备更新 WMT
  // 如果 ukv 与 kv0 不同 (即用户函数分配了新内存)，则需要复制 ukv
  struct kv * const newkv_to_wmt = (ukv != kv0 && ukv != oldkv_for_uf) ? xdb_dup_kv(ukv) : ukv; // 避免重复复制
  ctx->mt_ctx.newkv = newkv_to_wmt; // 设置内存表合并上下文的新 KV

  // 调用通用的内存表更新函数
  struct kv * const ret = xdb_mt_update_func(kv0, &ctx->mt_ctx);
  if (ctx->mt_ctx.success) // 如果内存表更新成功
    ctx->merged = true; // 标记合并完成
  else if (newkv_to_wmt != ukv && newkv_to_wmt != kv0) // 如果更新失败且 newkv_to_wmt 是新分配的
    free(newkv_to_wmt); // 释放它

  // 如果用户函数返回的 ukv 与 kv0 或 oldkv_for_uf 不同，且与 newkv_to_wmt 也不同（这种情况较少，但为了安全）
  // 且 ukv 不是由 xdb_mt_update_func 返回的（它会自己管理内存），则可能需要释放 ukv
  // 但通常用户函数要么返回旧指针，要么返回新分配的指针，要么返回NULL
  // 此处假设用户函数返回的新分配指针如果未被 xdb_mt_update_func 接管，则由调用者（或此函数更外层）处理
  // 或者用户函数返回的指针是 kv0 或 oldkv_for_uf (表示原地修改或无需修改)

  return ret; // 返回给 WMT 的 merge 函数
}

// RMW 的第一阶段合并函数 (仅当键在 WMT 中找到时才执行合并)
// kv_merge_func 的实现
  static struct kv *
xdb_merge_merge_func1(struct kv * const kv0, void * const priv)
{
  struct xdb_rmw_ctx * const ctx = priv;
  if (kv0 == NULL) { // 如果在 WMT 中未找到该键
    ctx->mt_ctx.success = true; // 标记 WMT 操作完成 (虽然未修改)，merged 保持 false
    return NULL; // 返回 NULL，表示不在 WMT 中进行操作
  }

  // 如果在 WMT 中找到，则执行标准合并逻辑
  return xdb_merge_merge_func(kv0, priv);
}

// 执行 Read-Modify-Write (Merge) 操作
  bool
xdb_merge(struct xdb_ref * const ref, const struct kref * const kref, kv_merge_func uf, void * const priv)
{
  debug_assert(kref && uf);
  xdb_write_enter(ref); // 等待写条件满足

  struct xdb_rmw_ctx ctx = {.mt_ctx = {.xdb = ref->xdb}, .uf = uf, .priv = priv, .oldkv = NULL, .merged = false};

  bool s; // 操作结果
  // 第一阶段：尝试在 WMT 中合并
  do {
    xdb_ref_update_version(ref);
    xdb_ref_enter(ref);
    ctx.mt_ctx.mt_view = ref->mt_view;
    // 使用 func1，仅当键在 WMT 中存在时才调用用户合并函数
    s = wmt_api->merge(ref->wmt_ref, kref, xdb_merge_merge_func1, &ctx);
    xdb_ref_leave(ref);
  } while (s && !ctx.mt_ctx.success); // 如果 WMT merge 成功但内部更新失败 (视图改变)，则重试

  if (ctx.merged || (!s)) // 如果已在 WMT 中合并完成，或 WMT merge 调用失败
    return s; // 返回结果
  // 至此，键不在 WMT 中，或者在 WMT 中但用户函数未执行 (因为 func1 返回 NULL)
  // merged 仍然是 false, mt_ctx.success 是 true (因为 func1 中设置了)
  ctx.mt_ctx.success = false; // 重置 success 标志，准备第二阶段

  // 第二阶段：如果键不在 WMT 中，则从 IMT/SST 获取旧值，然后与 WMT 合并
  do {
    xdb_ref_update_version(ref);
    ctx.oldkv = xdb_merge_get_old(ref, kref); // 从 IMT/SST 获取旧值
    xdb_ref_enter(ref);
    ctx.mt_ctx.mt_view = ref->mt_view;
    // 使用标准 merge_func，它会考虑 ctx.oldkv
    s = wmt_api->merge(ref->wmt_ref, kref, xdb_merge_merge_func, &ctx);
    xdb_ref_leave(ref);
    free(ctx.oldkv); // 释放从 IMT/SST 获取的旧值 (如果存在)
    ctx.oldkv = NULL;
  } while (s && !ctx.merged); // 如果 WMT merge 成功但内部合并未完成 (视图改变)，则重试
  return s; // 返回最终操作结果
}
// }}} merge // Merge 操作函数区域结束

// iter {{{ // 迭代器相关函数区域开始
// 为多路归并迭代器 (miter) 添加数据库各层级的引用
  static void
xdb_iter_miter_ref(struct xdb_iter * const iter)
{
  struct xdb_ref * const ref = iter->db_ref;
  iter->mt_view = ref->mt_view; // 记录迭代器创建时使用的内存表视图

  // 添加 SSTable 版本视图的引用
  miter_add_ref(iter->miter, &kvmap_api_msstv_ts, ref->vref);

  // 如果存在 IMT，添加 IMT 的引用
  if (ref->imt_ref)
    miter_add_ref(iter->miter, imt_api, ref->imt_ref);

  // 添加 WMT 的引用
  miter_add_ref(iter->miter, wmt_api, ref->wmt_ref);
}

// 更新迭代器的版本信息 (如果数据库版本已更新)
  static void
xdb_iter_update_version(struct xdb_iter * const iter)
{
  struct xdb_ref * const ref = iter->db_ref;
  // 如果迭代器视图与 ref 视图一致，且 ref 视图与 XDB 主视图一致，则无需更新
  if ((ref->mt_view == ref->xdb->mt_view) && (iter->mt_view == ref->mt_view))
    return;

  miter_clean(iter->miter); // 清理 miter 中的旧引用
  xdb_ref_update_version(ref); // 更新 db_ref 的版本
  xdb_iter_miter_ref(iter); // 为 miter 添加新版本的引用
  // 获取新版本
}

// 创建一个新的 XDB 迭代器
  struct xdb_iter *
xdb_iter_create(struct xdb_ref * const ref)
{
  struct xdb_iter * const iter = calloc(1, sizeof(*iter)); // 分配迭代器结构体
  iter->miter = miter_create(); // 创建多路归并迭代器实例
  iter->db_ref = ref; // 保存数据库引用

  xdb_ref_update_version(ref); // 更新版本信息
  xdb_iter_miter_ref(iter); // 为 miter 添加引用
  xdb_iter_park(iter); // 初始时停放迭代器 (释放可能持有的锁)
  return iter;
}

// 跳过迭代器当前位置的删除标记 (时间戳 KV)
  static void
xdb_iter_skip_ts(struct xdb_iter * const iter)
{
  struct kvref kvref;
  do {
    if (miter_kvref(iter->miter, &kvref) == false) // 获取当前 KV 引用失败 (迭代器无效)
      return;
    if (kvref.hdr.vlen != SST_VLEN_TS) // 如果不是删除标记，则停止
      break;
    miter_skip_unique(iter->miter); // 跳过当前唯一的键 (包括所有版本)
  } while (true);
}

// 停放迭代器 (释放其可能持有的资源，如锁)
  void
xdb_iter_park(struct xdb_iter * const iter)
{
  miter_park(iter->miter); // 停放 miter

  if (iter->coq_parked) { // 如果之前停放了协程队列
    coq_install(iter->coq_parked); // 恢复协程队列
    iter->coq_parked = NULL;
  }
}

// 将迭代器定位到指定的键 (或大于等于该键的第一个键)
  void
xdb_iter_seek(struct xdb_iter * const iter, const struct kref * const key)
{
  xdb_iter_update_version(iter); // 更新迭代器版本

  struct coq * const coq = coq_current(); // 获取当前协程队列
  if (coq) { // 如果在协程环境中
    iter->coq_parked = coq; // 保存当前协程队列
    coq_uninstall(); // 暂时卸载协程队列 (miter 内部可能不支持协程切换)
  }

  miter_seek(iter->miter, key); // 定位 miter
  xdb_iter_skip_ts(iter); // 跳过可能的删除标记
}

// 检查迭代器当前是否指向一个有效的 KV 对
  bool
xdb_iter_valid(struct xdb_iter * const iter)
{
  return miter_valid(iter->miter);
}

// 获取迭代器当前指向的 KV 对 (假设迭代器有效)
// 返回的 KV 对象是新分配的 (如果 out 为 NULL) 或复制到 out 中
  struct kv *
xdb_iter_peek(struct xdb_iter * const iter, struct kv * const out)
{
  struct kvref kvref;
  if (!miter_kvref(iter->miter, &kvref)) // 获取当前 KV 引用失败
    return NULL;

  // 此处不应看到删除标记 (已被 xdb_iter_skip_ts 跳过)
  debug_assert(kvref.hdr.vlen != SST_VLEN_TS);
  return sst_kvref_dup2_kv(&kvref, out); // 从 KV 引用复制数据
}

// 获取迭代器当前指向的键引用
  bool
xdb_iter_kref(struct xdb_iter * const iter, struct kref * const kref)
{
  return miter_kref(iter->miter, kref);
}

// 获取迭代器当前指向的键值引用
  bool
xdb_iter_kvref(struct xdb_iter * const iter, struct kvref * const kvref)
{
  return miter_kvref(iter->miter, kvref);
}

// 将迭代器向前移动一个唯一的键
  void
xdb_iter_skip1(struct xdb_iter * const iter)
{
  miter_skip_unique(iter->miter); // 跳过当前唯一键
  xdb_iter_skip_ts(iter); // 跳过可能的删除标记
}

// 将迭代器向前移动 n 个唯一的键
  void
xdb_iter_skip(struct xdb_iter * const iter, const u32 n)
{
  for (u32 i = 0; i < n; i++) {
    miter_skip_unique(iter->miter);
    xdb_iter_skip_ts(iter);
    if (!miter_valid(iter->miter)) break; // 如果中途迭代器失效，则停止
  }
}

// 获取迭代器当前指向的 KV 对，并将迭代器向前移动一个唯一的键
  struct kv *
xdb_iter_next(struct xdb_iter * const iter, struct kv * const out)
{
  struct kv * const kv = xdb_iter_peek(iter, out); // 获取当前 KV
  if (kv) xdb_iter_skip1(iter); // 如果成功获取，则移动迭代器
  return kv;
}

// 销毁 XDB 迭代器
  void
xdb_iter_destroy(struct xdb_iter * const iter)
{
  miter_destroy(iter->miter); // 销毁 miter

  if (iter->coq_parked) { // 如果有停放的协程队列
    coq_install(iter->coq_parked); // 恢复它
    iter->coq_parked = NULL;
  }

  free(iter); // 释放迭代器结构体内存
}
// }}} iter // 迭代器相关函数区域结束

// api {{{ // kvmap API 实现区域开始
// XDB 的 kvmap_api 接口实现
const struct kvmap_api kvmap_api_xdb = {
  .hashkey = true,        // 是否基于哈希键 (部分操作是)
  .ordered = true,        // 是否有序
  .threadsafe = true,     // 是否线程安全
  .unique = true,         // 键是否唯一
  .get = (void*)xdb_get,
  .probe = (void*)xdb_probe,
  .put = (void*)xdb_put,
  .del = (void*)xdb_del,
  .merge = (void*)xdb_merge,
  .sync = (void*)xdb_sync,
  .ref = (void*)xdb_ref,
  .unref = (void*)xdb_unref,
  .destroy = (void*)xdb_close, // destroy 对应 xdb_close

  .iter_create = (void*)xdb_iter_create,
  .iter_seek = (void*)xdb_iter_seek,
  .iter_valid = (void*)xdb_iter_valid,
  .iter_peek = (void*)xdb_iter_peek,
  .iter_kref = (void*)xdb_iter_kref,
  .iter_kvref = (void*)xdb_iter_kvref,
  .iter_skip1 = (void*)xdb_iter_skip1,
  .iter_skip = (void*)xdb_iter_skip,
  .iter_next = (void*)xdb_iter_next,
  .iter_park = (void*)xdb_iter_park,
  .iter_destroy = (void*)xdb_iter_destroy,
};

// kvmap API 的创建函数 (用于通过名称和参数创建 XDB 实例)
  static void *
xdb_kvmap_api_create(const char * const name, const struct kvmap_mm * const mm, char ** const args)
{
  (void)mm; // XDB 不使用外部内存管理器
  if (!strcmp(name, "xdb")) { // 标准 XDB 创建
    const char * const dir = args[0];
    const size_t cache_size_mb = a2u64(args[1]);
    const size_t mt_size_mb = a2u64(args[2]);
    const size_t wal_size_mb = (strcmp(args[3], "auto") == 0) ? (mt_size_mb << 1) : a2u64(args[3]); // WAL 大小，auto 表示 mt_size*2
    const bool ckeys = args[4][0] != '0'; // 是否使用压缩键
    const bool tags = args[5][0] != '0';  // 是否使用标签
    const u32 nr_workers = (strcmp(args[6], "auto") == 0) ? 4 : a2u32(args[6]); // 工作线程数
    const u32 co_per_worker = (strcmp(args[7], "auto") == 0) ? (ckeys ? 1 : 4) : a2u32(args[7]); // 每工作线程协程数
    const char * const worker_cores = args[8]; // 绑核配置
    return xdb_open(dir, cache_size_mb, mt_size_mb, wal_size_mb, ckeys, tags, nr_workers, co_per_worker, worker_cores);

  } else if (!strcmp(name, "xdbauto")) { // 简化的 XDB 创建 (使用一些默认值)
    const char * const dir = args[0];
    const size_t cache_size_mb = a2u64(args[1]);
    const size_t mt_size_mb = a2u64(args[2]);
    const bool tags = args[3][0] != '0';
    // 使用默认的 wal_size, ckeys, nr_workers, co_per_worker, worker_cores
    return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, true, tags, 4, 1, "auto");
  }
  return NULL; // 名称不匹配
}

// 构造函数属性，在 main 函数执行前调用，用于注册 XDB 的 kvmap API
__attribute__((constructor))
  static void
xdb_kvmap_api_init(void)
{
  kvmap_api_register(9, "xdb", "<path> <cache-mb> <mt-mb> <wal-mb/auto> <ckeys(0/1)> <tags(0/1)>"
      " <nr-workers/auto> <co-per-worker/auto> <worker-cores/auto/dont>",
      xdb_kvmap_api_create, &kvmap_api_xdb);

  kvmap_api_register(4, "xdbauto", "<path> <cache-mb> <mt-mb> <tags(0/1)>",
      xdb_kvmap_api_create, &kvmap_api_xdb);
}
// }}} // kvmap API 实现区域结束

// remixdb {{{ // RemixDB 公开 API 区域开始 (对 XDB 的简单封装)
// 默认配置：生成 ckeys 和 tags：速度快，但消耗略多的内存/磁盘空间
// 如需更多选项，请使用 xdb_open
  struct xdb *
remixdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const bool tags)
{
  // 调用 xdb_open，使用一些默认参数 (wal_size=mt_size*2, ckeys=true, nr_workers=4, co_per_worker=1, worker_cores="auto")
  return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, true, tags, 4, 1, "auto");
}

// 紧凑模式：提供略低的写放大 (WA) 和更低的磁盘使用率；
// 但是，如果工作负载的写局部性较差，压缩可能会变慢。
// 哈希标签也被禁用，因此点查询会慢得多。
// 仅当磁盘空间非常有限时才应使用此模式。
  struct xdb *
remixdb_open_compact(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb)
{
  // 调用 xdb_open，使用紧凑模式参数 (ckeys=false, tags=false, co_per_worker=4)
  return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, false, false, 4, 4, "auto");
}

// 获取数据库引用
  struct xdb_ref *
remixdb_ref(struct xdb * const xdb)
{
  return xdb_ref(xdb);
}

// 释放数据库引用
  void
remixdb_unref(struct xdb_ref * const ref)
{
  (void)xdb_unref(ref); // 调用底层的 xdb_unref
}

// 关闭数据库
  void
remixdb_close(struct xdb * const xdb)
{
  xdb_close(xdb);
}

// 插入或更新键值对
  bool
remixdb_put(struct xdb_ref * const ref, // 指向 XDB 数据库引用的指针
    const void * const kbuf, // 指向键数据缓冲区的指针
    const u32 klen, // 键的长度
    const void * const vbuf, // 指向值数据缓冲区的指针
    const u32 vlen) // 值的长度
{
  // TODO: 巨大的 KV 应该存储在单独的文件中，并在 xdb 中插入间接引用
  if ((klen + vlen) > 65500) // 限制键值总长度
    return false;

  struct kv * const newkv = kv_create(kbuf, klen, vbuf, vlen); // 创建 KV 对象
  if (!newkv)
    return false;

  struct kref kref;
  kref_ref_kv(&kref, newkv); // 从 KV 对象创建键引用
  return xdb_update(ref, &kref, newkv); // 调用底层更新函数
}

// 删除键
  bool
remixdb_del(struct xdb_ref * const ref, const void * const kbuf, const u32 klen)
{
  struct kref kref;
  kref_ref_hash32(&kref, kbuf, klen); // 从键缓冲区创建键引用 (包含哈希计算)

  struct kv * const ts_kv = xdb_new_ts(&kref); // 创建删除标记
  if (!ts_kv)
    return false;

  return xdb_update(ref, &kref, ts_kv); // 调用底层更新函数 (写入删除标记)
}

// 测试键是否存在 (在 Wormhole 中，即内存表)
  bool
remixdb_probe(struct xdb_ref * const ref, const void * const kbuf, const u32 klen)
{
  struct kref kref;
  kref_ref_hash32(&kref, kbuf, klen);
  return xdb_probe(ref, &kref); // 调用底层探测函数
}

// Get 操作的辅助信息结构体 (RemixDB API 版本)
struct remixdb_get_info { void * vbuf_out; u32 * vlen_out; };

// 用于 RemixDB Get 操作的 inpr 回调
  static void
remixdb_inp_get(struct kv * kv, void * priv)
{
  // 查看此键时进行复制，以避免 get 返回后出现一致性问题
  if (kv) {
    struct remixdb_get_info * const info = (typeof(info))priv;
    *info->vlen_out = kv->vlen; // 复制原始值长度 (可能包含标记)
    if (kv->vlen != SST_VLEN_TS) // 如果不是删除标记
      memcpy(info->vbuf_out, kv_vptr_c(kv), kv->vlen & SST_VLEN_MASK); // 复制实际值数据
  }
}

// 获取键对应的值
  bool
remixdb_get(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
    void * const vbuf_out, u32 * const vlen_out)
{
  struct kref kref;
  kref_ref_hash32(&kref, kbuf, klen); // 创建键引用

  xdb_ref_update_version(ref); // 更新版本
  xdb_ref_enter(ref); // 进入临界区

  // WMT (可写内存表)
  struct remixdb_get_info info = {vbuf_out, vlen_out};
  if (wmt_api->inpr(ref->wmt_ref, &kref, remixdb_inp_get, &info)) { // 如果 WMT 处理了请求
    xdb_ref_leave(ref); // 离开临界区
    return (*vlen_out) != SST_VLEN_TS; // 返回是否找到有效值 (非删除标记)
  }
  xdb_ref_leave(ref); // 离开临界区

  // IMT (不可变内存表)
  if (ref->imt_ref) {
    if (imt_api->inpr(ref->imt_ref, &kref, remixdb_inp_get, &info))
      return (*vlen_out) != SST_VLEN_TS;
  }
  // 如果内存表中未找到，则在 SSTables 中查找
  return msstv_get_value_ts(ref->vref, &kref, vbuf_out, vlen_out);
}

// 同步数据到磁盘
  void
remixdb_sync(struct xdb_ref * const ref)
{
  return xdb_sync(ref); // 调用底层同步函数
}

// 创建迭代器
  struct xdb_iter *
remixdb_iter_create(struct xdb_ref * const ref)
{
  return xdb_iter_create(ref);
}

// 定位迭代器
void
remixdb_iter_seek(struct xdb_iter * const iter,    // 指向 XDB 迭代器的指针
                  const void * const kbuf,         // 指向要查找的键数据缓冲区的常量指针
                  const u32 klen)                  // 键数据的长度（字节数）
{
  struct kref kref;
  kref_ref_hash32(&kref, kbuf, klen);          // 从键缓冲区创建键引用结构体
                                                   // - 计算键的 CRC32 哈希值用于快速查找
                                                   // - 创建内部使用的键引用格式

  xdb_iter_seek(iter, &kref);                 // 调用底层 XDB 迭代器定位函数
                                                   // - 实际执行定位操作
                                                   // - 将迭代器移动到 >= 指定键的第一个位置
}
// 检查迭代器是否有效
  bool
remixdb_iter_valid(struct xdb_iter * const iter)
{
  return xdb_iter_valid(iter);
}

// 获取迭代器当前指向的键值对
  bool
remixdb_iter_peek(struct xdb_iter * const iter,
    void * const kbuf_out, u32 * const klen_out,
    void * const vbuf_out, u32 * const vlen_out)
{
  struct kvref kvref;
  if (!miter_kvref(iter->miter, &kvref)) // 获取 KV 引用失败
    return false;

  // 此处不应看到删除标记
  debug_assert(kvref.hdr.vlen != SST_VLEN_TS);
  if (kbuf_out) { // 如果提供了键输出缓冲区
    const u32 klen_data = kvref.hdr.klen;
    memcpy(kbuf_out, kvref.kptr, klen_data);
    *klen_out = klen_data;
  }

  if (vbuf_out) { // 如果提供了值输出缓冲区
    const u32 vlen_data = kvref.hdr.vlen & SST_VLEN_MASK; // 获取实际值长度
    memcpy(vbuf_out, kvref.vptr, vlen_data);
    *vlen_out = vlen_data;
  }

  return true;
}

// 迭代器向前移动一个键
  void
remixdb_iter_skip1(struct xdb_iter * const iter)
{
  xdb_iter_skip1(iter);
}

// 迭代器向前移动 n 个键
  void
remixdb_iter_skip(struct xdb_iter * const iter, const u32 nr)
{
  xdb_iter_skip(iter, nr);
}

// 停放迭代器
  void
remixdb_iter_park(struct xdb_iter * const iter)
{
  xdb_iter_park(iter);
}

// 销毁迭代器
  void
remixdb_iter_destroy(struct xdb_iter * const iter)
{
  xdb_iter_destroy(iter);
}
// }}} remixdb // RemixDB 公开 API 区域结束

// fdm: marker
