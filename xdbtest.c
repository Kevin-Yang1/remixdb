/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

#include "ctypes.h"
#include "lib.h"
#include "kv.h"
#include "sst.h"
#include "xdb.h"

// 全局变量定义
struct xdb * xdb;              // 数据库实例
static u64 nkeys = 0;          // 键的总数量
static u64 nupdate = 0;        // 每轮更新操作的数量
static u64 min_stale = 0;      // 最小过期键数量（用于检测数据一致性）
static u8 * magics = NULL;     // 魔数数组，记录每个键对应的期望值
u32 nths_update = 0;           // 更新线程数量
u32 nths_getscan = 0;          // 读取/扫描线程数量

// 原子计数器，用于线程间统计
au64 all_seq;    // 序列号计数器
au64 all_stale;  // 过期键计数器
au64 all_found;  // 找到的键计数器

/**
 * 更新工作线程函数
 * 执行随机的写入（PUT）和删除（DEL）操作
 */
  static void *
update_worker(void * const ptr)
{
  (void)ptr;
  srandom_u64(time_nsec());                          // 使用当前时间初始化随机数种子
  const u64 seq = atomic_fetch_add(&all_seq, 1);    // 获取线程序号
  const u64 range = nkeys / nths_update;            // 每个线程负责的键范围大小
  const u64 mask = range - 1;                       // 范围掩码
  const u64 base = seq * range;                     // 当前线程的起始键偏移
  struct xdb_ref * const ref = remixdb_ref(xdb);    // 获取数据库引用
  u8 ktmp[16];                                       // 键缓冲区
  u8 * const vtmp = calloc(1, 1lu << 16);          // 值缓冲区（64KB）
  memset(vtmp, (int)random_u64(), 1lu << 16);       // 用随机数填充值缓冲区

  //printf("random update [%lu, %lu]\n", base, base+mask);
  // 执行设置/删除操作
  for (u64 i = 0; i < nupdate; i++) {
    const u64 r = random_u64();                      // 生成随机数
    const u64 k = base + ((r >> 8) & mask);         // 计算要操作的键ID
    const u8 v = r & 0xff;                          // 提取值的第一个字节作为魔数
    vtmp[0] = v;                                     // 设置值的第一个字节
    magics[k] = v;                                   // 记录期望的魔数
    strhex_64(ktmp, k);                             // 将键ID转换为16进制字符串

    if (v == 0) { // 删除操作
      remixdb_del(ref, ktmp, 16);
    } else { // 更新操作
      // 大部分时候使用小值（100-200字节），偶尔使用大值（4200+字节）
      const u32 vlen = ((i & 0x3fffu) != 0x1357u) ? (((u32)r & 0xf0) + 100) : ((((u32)r & 0xf0) << 6) + 4200);
      remixdb_put(ref, ktmp, 16, vtmp, vlen);
    }
  }

  remixdb_unref(ref);  // 释放数据库引用
  free(vtmp);          // 释放值缓冲区
  return NULL;
}

/**
 * 读取/扫描工作线程函数
 * 执行GET操作和范围扫描，检查数据一致性
 */
  static void *
getscan_worker(void * const ptr)
{
  (void)ptr;
  const u64 seq = atomic_fetch_add(&all_seq, 1);    // 获取线程序号
  const u64 unit = nkeys / nths_getscan + 1;        // 每个线程负责的键数量
  const u64 min = unit * seq;                       // 当前线程的起始键ID
  const u64 max0 = min + unit;                      // 计算结束键ID
  const u64 max = nkeys < max0 ? nkeys : max0;      // 确保不超过总键数

  struct xdb_ref * const ref = remixdb_ref(xdb);    // 获取数据库引用

  u8 ktmp[16];                                       // 键缓冲区
  u8 * const out = calloc(1, 1lu << 16);           // 输出缓冲区（64KB）
  u32 vlen_out = 0;                                 // 输出值长度

  // 顺序GET操作，检查数据一致性
  u64 stale = 0;  // 过期/不一致的键计数
  for (u64 i = min; i < max; i++) {
    strhex_64(ktmp, i);                             // 生成键
    const bool r = remixdb_get(ref, ktmp, 16, out, &vlen_out);  // 执行GET操作
    // 检查读取的值是否与期望的魔数匹配
    if ((r ? out[0] : 0) != magics[i])
      stale++;
  }

  // 范围扫描操作
  u32 klen_out;
  u8 kend[16];
  strhex_64(ktmp, min);                             // 起始键
  strhex_64(kend, max);                             // 结束键
  struct xdb_iter * const iter = remixdb_iter_create(ref);  // 创建迭代器
  remixdb_iter_seek(iter, ktmp, 16);                // 定位到起始位置
  memset(ktmp, 0, 16);                              // 清空键缓冲区

  // 扫描指定范围内的所有键
  u64 found = 0;  // 找到的键计数
  while (remixdb_iter_valid(iter)) {
    remixdb_iter_peek(iter, ktmp, &klen_out, NULL, NULL);  // 获取当前键
    debug_assert(klen_out == 16);                   // 确保键长度正确
    if (memcmp(ktmp, kend, 16) < 0) {              // 检查是否在范围内
      found++;
      remixdb_iter_skip1(iter);                     // 移动到下一个键
    } else {
      break;  // 超出范围，退出循环
    }
  }

  //printf("get [%lu, %lu] stale %lu found %lu\n", min, max-1, stale, found);
  atomic_fetch_add(&all_stale, stale);              // 累加过期键数
  atomic_fetch_add(&all_found, found);              // 累加找到的键数

  remixdb_iter_destroy(iter);  // 销毁迭代器
  remixdb_unref(ref);          // 释放数据库引用
  free(out);                   // 释放输出缓冲区
  return NULL;
}

/**
 * 主函数 - RemixDB 压力测试程序
 */
  int
main(int argc, char** argv)
{
  // 检查命令行参数
  if (argc < 6) {
    printf("Usage: <dirname> <cache-mb> <mt-mb> <data-power> <update-power> [<epochs>]\n");
    printf("用法: <数据库目录> <缓存大小MB> <内存表大小MB> <数据规模指数> <更新规模指数> [<测试轮数>]\n");
    printf("    WAL size = <mt-mb>*2\n");
    printf("    WAL大小 = <内存表大小MB>*2\n");
    return 0;
  }

  // 检查CPU核心数量（至少需要5个核心）
  const u32 ncores = process_affinity_count();
  if (ncores < 5) {
    fprintf(stderr, "Need at least five cores on the cpu affinity list\n");
    fprintf(stderr, "在CPU亲和性列表中至少需要5个核心\n");
    exit(0);
  }

  // 解析命令行参数
  const u64 cachesz = a2u64(argv[2]);    // 缓存大小（MB）
  const u64 mtsz = a2u64(argv[3]);       // 内存表大小（MB）
  const u64 dpower = a2u64(argv[4]);     // 数据规模指数（键数量 = 2^dpower）
  const u64 upower = a2u64(argv[5]);     // 更新规模指数（每轮更新数 = 2^upower）

  // 打开数据库
  xdb = remixdb_open(argv[1], cachesz, mtsz, true);
  if (!xdb) {
    fprintf(stderr, "xdb_open failed\n");
    fprintf(stderr, "数据库打开失败\n");
    return 0;
  }

  // 初始化测试参数
  nkeys = 1lu << dpower;      // 键的总数量
  if (nkeys < 1024)
    nkeys = 1024;
  nupdate = 1lu << upower;    // 每轮更新操作数量

  min_stale = nkeys;          // 初始化最小过期键数
  magics = calloc(nkeys, 1);  // 分配魔数数组
  nths_getscan = ncores - 4;  // 读取线程数（预留4个核心给压缩线程）
  nths_update = ncores - 4;   // 写入线程数

  // 确保更新线程数是2的幂次（为了简化分区逻辑）
  while (__builtin_popcount(nths_update) > 1)
    nths_update--;
  printf("write threads %u check threads %u\n", nths_update, nths_getscan);
  printf("写入线程 %u 个，检查线程 %u 个\n", nths_update, nths_getscan);

  debug_assert(magics);
  const u32 ne = (argc < 7) ? 1000000 : a2u32(argv[6]);  // 测试轮数

  // 主测试循环
  for (u32 e = 0; e < ne; e++) {
    // 执行写入操作阶段
    all_seq = 0;
    const u64 dt = thread_fork_join(nths_update, update_worker, false, NULL);
    
    // 每4轮重新打开数据库（压力测试）
    if ((e & 0x3u) == 0x3u) { // close/open every 4 epochs
      remixdb_close(xdb);
      // 交替开启/关闭压缩键功能，增加压力测试强度
      xdb = (e & 4) ? remixdb_open(argv[1], cachesz, mtsz, (e & 8) != 0) : remixdb_open_compact(argv[1], cachesz, mtsz);
      if (xdb) {
        printf("reopen remixdb ok\n");
        printf("重新打开数据库成功\n");
      } else {
        printf("reopen failed\n");
        printf("重新打开数据库失败\n");
        exit(0);
      }
    }
    
    // 执行读取/扫描操作阶段
    all_stale = 0;
    all_found = 0;
    all_seq = 0;
    (void)thread_fork_join(nths_getscan, getscan_worker, false, NULL);

    // 输出当前轮次的统计信息
    char ts[64];
    time_stamp(ts, sizeof(ts));                      // 获取时间戳
    const u64 nr = nupdate * nths_update;           // 总操作数
    printf("[%4u] %s put/del nr %lu mops %.3lf keyrange %lu keycount %lu stale %lu\n",
        e, ts, nr, (double)nr / (double)dt * 1e3, nkeys, all_found, all_stale);
    printf("[%4u] %s 写入/删除操作数 %lu MOPS %.3lf 键范围 %lu 键计数 %lu 过期键 %lu\n",
        e, ts, nr, (double)nr / (double)dt * 1e3, nkeys, all_found, all_stale);

    // 数据一致性检查：过期键数不应该增加（这表明数据丢失）
    if (all_stale > min_stale)
      debug_die();
    min_stale = all_stale;
  }
  
  // 清理资源
  free(magics);
  remixdb_close(xdb);
  return 0;
}