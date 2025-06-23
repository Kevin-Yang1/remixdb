# RemixDB 学习指南

## 项目概述

RemixDB 是一个基于 LSM-tree（Log-Structured Merge Tree）的高性能嵌入式键值数据库，实现了 REMIX 索引结构来优化范围查询性能。

## 核心组件架构

```
RemixDB 整体架构：
┌─────────────────────────────────────────────────────────────┐
│                        应用层 API                            │
│  remixdb_open, remixdb_put, remixdb_get, remixdb_iter_*     │
├─────────────────────────────────────────────────────────────┤
│                      XDB 核心引擎                           │
│  • 事务处理 (xdb.c)                                         │
│  • WAL (Write-Ahead Logging)                               │
│  • 内存表管理 (WMT/IMT)                                     │
│  • 压缩调度                                                 │
├─────────────────────────────────────────────────────────────┤
│                     存储层                                  │
│  • WormHole 内存表 (wh.c) - 高性能并发哈希表                  │
│  • SSTable 管理 (sst.c) - 磁盘存储格式                       │
│  • REMIX 索引 - 优化范围查询                                 │
│  • Block I/O (blkio.c) - 异步 I/O 管理                      │
├─────────────────────────────────────────────────────────────┤
│                    基础设施                                 │
│  • 键值抽象 (kv.c) - 统一的 KV 接口                          │
│  • 工具库 (lib.c) - 内存管理、并发控制、QSBR                  │
└─────────────────────────────────────────────────────────────┘
```

## 学习路径

### 第一阶段：理解基础概念

1. **LSM-tree 原理**

   - 理解写优化存储结构
   - WAL + MemTable + SSTable 的工作流程
   - 压缩（Compaction）过程
2. **运行示例程序**

   ```bash
   # 编译并运行演示程序
   make CCC=gcc O=0g xdbdemo.out
   ./xdbdemo.out
   ```

### 第二阶段：核心数据结构

1. **键值抽象层 (kv.h/kv.c)**

   - `struct kv`: 统一的键值对表示
   - `struct kref`: 键引用（避免复制）
   - 哈希计算和内存管理
2. **WormHole 内存表 (wh.h/wh.c)**

   - 高性能并发有序映射
   - MetaTrieHT 架构
   - QSBR 内存回收机制

### 第三阶段：存储引擎

1. **SSTable 存储 (sst.h/sst.c)**

   - 不可变表文件格式
   - REMIX 索引结构
   - 块级压缩和缓存
2. **异步 I/O (blkio.h/blkio.c)**

   - io_uring 集成
   - 异步读写队列
   - 缓存管理

### 第四阶段：事务引擎

1. **XDB 核心 (xdb.h/xdb.c)**
   - WAL 实现
   - 版本控制
   - 压缩调度
   - 多版本并发控制 (MVCC)

### 第五阶段：高级特性

1. **REMIX 索引优化**

   - 前缀压缩
   - 哈希标签加速
   - 范围查询优化
2. **并发控制**

   - QSBR (Quiescent State Based Reclamation)
   - 读写锁和自旋锁
   - 无锁数据结构

## 代码阅读顺序

### 1. 从示例开始 (推荐起点)

```c
// xdbdemo.c - 了解 API 使用方式
```

### 2. 基础抽象

```c
// kv.h/kv.c - 键值对抽象
// lib.h/lib.c - 基础工具库
```

### 3. 内存表实现

```c
// wh.h/wh.c - WormHole 并发哈希表
```

### 4. 存储层

```c
// sst.h/sst.c - SSTable 实现
// blkio.h/blkio.c - 异步 I/O
```

### 5. 引擎核心

```c
// xdb.h/xdb.c - 主引擎逻辑
```

## 实验建议

### 1. 基础实验

```bash
# 编译调试版本
make CCC=gcc O=0g all

# 运行测试
./xdbtest.out 100 100 10 10 1000

# 查看数据库文件
ls -la xdbdemo/
```

### 2. 性能测试

```bash
# 编译优化版本
make CCC=gcc O=rg xdbtest.out

# 大规模测试
./xdbtest.out 1000 1000 16 16 100000
```

### 3. 调试分析

```bash
# 使用 AddressSanitizer
make CCC=gcc O=0s xdbdemo.out
./xdbdemo.out

# 使用 GDB 调试
gdb ./xdbdemo.out
```

## 关键概念理解

### LSM-tree 写入流程

1. 写入 WAL（保证持久性）
2. 写入内存表 WMT
3. WMT 满时转为不可变表 IMT
4. 后台压缩 IMT 到 SSTable

### REMIX 查询优化

1. 使用前缀树索引快速定位
2. 段内使用哈希标签加速
3. 减少不必要的键比较

### 并发控制

1. WMT 支持多线程并发读写
2. QSBR 实现无锁内存回收
3. 版本控制保证读一致性

## 学习重点

1. **理解 LSM-tree 的权衡**：写优化 vs 读优化
2. **掌握并发控制**：如何在高并发下保证数据一致性
3. **学习 REMIX 优化**：如何改进传统 LSM-tree 的范围查询性能
4. **异步 I/O 设计**：如何利用现代 Linux 的 io_uring
5. **内存管理**：QSBR、slab分配器等高性能内存管理技术

开始学习时，建议先运行示例程序，然后从 kv.h 和 xdbdemo.c 开始阅读代码。
