/*
 * Copyright (c) 2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

#include <stdio.h>

#include "lib.h"
#include "kv.h"
#include "xdb.h"

  int
main(int argc, char ** argv)
{
  (void)argc;
  (void)argv;
  
  // 使用小配置进行演示
  // 在中等规模的设置中，建议两个参数都使用 4096
  // 参数说明：数据库目录、块缓存大小(MB)、内存表大小(MB)、是否使用标签
  struct xdb * const xdb = remixdb_open("./xdbdemo", 256, 256, true); // blockcache=256MB, MemTable=256MB, use_tags=true

  // 需要一个 ref（引用）来执行以下数据库操作
  // 一个线程应该维护一个 ref 并持续使用它
  // 不同线程应该使用不同的 refs
  struct xdb_ref * const ref = remixdb_ref(xdb);

  bool r; // 操作结果标志

  // 插入键值对操作
  r = remixdb_put(ref, "remix", 5, "easy", 4);
  printf("remixdb_put remix easy %c\n", r?'T':'F');

  // 插入另一个键值对
  r = remixdb_put(ref, "time_travel", 11, "impossible", 10);
  printf("remixdb_put time_travel impossible %c\n", r?'T':'F');

  // 删除键值对操作
  r = remixdb_del(ref, "time_travel", 11);
  printf("remixdb_del time_travel %c\n", r?'T':'F');

  // 探测键是否存在（不返回值）
  r = remixdb_probe(ref, "time_travel", 11);
  printf("remixdb_probe time_travel %c\n", r?'T':'F');

  // 定义输出缓冲区
  u32 klen_out = 0;    // 输出键长度
  char kbuf_out[8] = {}; // 输出键缓冲区
  u32 vlen_out = 0;    // 输出值长度
  char vbuf_out[8] = {}; // 输出值缓冲区
  
  // 获取键值对操作
  r = remixdb_get(ref, "remix", 5, vbuf_out, &vlen_out);
  printf("remixdb_get remix %c %u %.*s\n", r?'T':'F', vlen_out, vlen_out, vbuf_out);

  // 为范围操作准备一些键值对
  remixdb_put(ref, "00", 2, "0_value", 7);
  remixdb_put(ref, "11", 2, "1_value", 7);
  remixdb_put(ref, "22", 2, "2_value", 7);

  // 将所有数据持久化到日志中
  // 执行同步操作开销较大
  remixdb_sync(ref);

  // 范围操作演示
  struct xdb_iter * const iter = remixdb_iter_create(ref);

  printf("remixdb_iter_seek \"\" (零长度字符串)\n");
  // 定位到第一个键（传入 NULL 和长度 0）
  remixdb_iter_seek(iter, NULL, 0); // seek to the first key
  // 实际上你可以向存储中插入零长度的键 (0 <= klen, klen+vlen <= 65500)

  // 遍历所有键值对
  while (remixdb_iter_valid(iter)) { // 检查迭代器是否指向有效的键值对
    r = remixdb_iter_peek(iter, kbuf_out, &klen_out, vbuf_out, &vlen_out);
    if (r) {
      printf("remixdb_iter_peek klen=%u key=%.*s vlen=%u value=%.*s\n",
          klen_out, klen_out, kbuf_out, vlen_out, vlen_out, vbuf_out);
    } else {
      printf("ERROR!\n");
    }
    remixdb_iter_skip1(iter); // 跳到下一个键值对
  }

  // 这是可选的操作！
  // 迭代器可能持有一些（读者）锁
  // 其他（写者）线程可能被活跃的迭代器阻塞
  // 当你需要空闲时调用 iter_park 来释放这些资源
  // 如果你正在积极使用迭代器，则不需要调用 iter_park
  remixdb_iter_park(iter);
  usleep(10); // 休眠 10 微秒

  // 调用 iter_park 后，你必须执行 seek() 来继续其他操作
  printf("remixdb_iter_seek \"0\" (键长度=1)\n");
  remixdb_iter_seek(iter, "0", 1);
  
  // 这次我们不想复制值，只获取键
  r = remixdb_iter_peek(iter, kbuf_out, &klen_out, NULL, NULL);
  if (r){
    printf("remixdb_iter_peek klen=%u key=%.*s\n", klen_out, klen_out, kbuf_out);
  } else {
    printf("ERROR: iter_peek failed\n");
  }

  // 销毁迭代器
  remixdb_iter_destroy(iter);
  
  // 当我们调用 unref() 时，必须没有活跃的迭代器
  remixdb_unref(ref);

  // close 操作不是线程安全的
  // 当你调用 close() 时，其他线程必须已经释放了它们的引用
  remixdb_close(xdb);
  return 0;
}