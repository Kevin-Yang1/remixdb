/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

#include "lib.h"
#include "kv.h"
#include "sst.h"
#include "xdb.h"

  int
main(int argc, char** argv)
{
  // 检查命令行参数
  if (argc < 4) {
    printf("Usage: <dirname> <mt-mb> <cache-mb>\n");
    printf("用法: <数据库目录> <内存表大小MB> <缓存大小MB>\n");
    return 0;
  }

  // 打开数据库：目录名, 内存表大小(MB), 缓存大小(MB), 使用标签
  struct xdb * const xdb = remixdb_open(argv[1], a2u64(argv[2]), a2u64(argv[3]), true);
  if (!xdb) {
    fprintf(stderr, "xdb_open failed\n");
    return 0;
  }
  struct xdb_ref * const ref = remixdb_ref(xdb);

  // 创建迭代器进行数据完整性检查
  struct xdb_iter * const iter = remixdb_iter_create(ref);
  u64 kid = 0; // 键ID计数器
  
  // 从头开始遍历，跳过前1000个键，以1000为步长检查数据
  remixdb_iter_seek(iter, "", 0);
  remixdb_iter_skip(iter, 1000);
  
  u8 key[20];     // 当前键缓冲区
  u8 keycmp[20];  // 期望键缓冲区（用于比较）
  u32 klen = 0;   // 键长度
  
  // 第一轮检查：以1000为步长验证键的连续性
  while (remixdb_iter_valid(iter)) {
    kid += 1000;
    remixdb_iter_peek(iter, key, &klen, NULL, NULL);
    strdec_64(keycmp, kid); // 生成期望的键（十进制字符串）
    
    // 比较实际键和期望键
    if (memcmp(key, keycmp, 20)) {
      printf("key mismatch at %lu; delete %s and restart the loop\n", kid, argv[1]);
      printf("键不匹配，位置 %lu；请删除 %s 并重新开始循环\n", kid, argv[1]);
      exit(0);
    }
    remixdb_iter_skip(iter, 1000); // 跳过1000个键
  }

  // 第二轮检查：从上次位置开始，逐个检查剩余的键
  u64 count = kid;
  remixdb_iter_seek(iter, "", 0);
  remixdb_iter_skip(iter, kid);
  
  while (remixdb_iter_valid(iter)) {
    remixdb_iter_peek(iter, key, &klen, NULL, NULL);
    remixdb_iter_skip1(iter); // 跳到下一个键
    strdec_64(keycmp, count);
    count++;
    
    // 验证键的连续性
    if (memcmp(key, keycmp, 20)) {
      printf("key mismatch at %lu; delete %s and restart loop again\n", count, argv[1]);
      printf("键不匹配，位置 %lu；请删除 %s 并重新开始循环\n", count, argv[1]);
      exit(0);
    }
  }
  printf("found %lu keys, last %.20s OK\n", count, key);
  printf("找到 %lu 个键，最后一个键 %.20s 正常\n", count, key);
  
  // 销毁迭代器
  remixdb_iter_destroy(iter);

  // 准备插入新数据
  u8 value[1024];
  memset(value, 0x11, 1024); // 用0x11填充值缓冲区
  
#define NEW ((100000)) // 定义要插入的新键数量
  
  // 插入新的键值对
  for (u64 i = 0; i < NEW; i++) {
    strdec_64(key, count + i); // 生成连续的键
    remixdb_put(ref, key, 20, value, 1024);
  }
  
  printf("insert [%lu, %lu]; now exit()\n", count, count + NEW - 1);
  printf("插入范围 [%lu, %lu]；现在退出程序\n", count, count + NEW - 1);
  
  // 同步数据到磁盘
  remixdb_sync(ref);
  
  // 强制退出程序（模拟程序崩溃）
  // 这是为了测试数据库的崩溃恢复能力
  exit(0);
  return 0;
}