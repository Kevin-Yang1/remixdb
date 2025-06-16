# REMIX 和 RemixDB

REMIX 数据结构在论文 ["REMIX: Efficient Range Query for LSM-trees", FAST'21](https://www.usenix.org/conference/fast21/presentation/zhong) 中被提出。

本代码库维护了 REMIX 索引数据结构的参考实现，以及一个线程安全的嵌入式键值存储实现，即 RemixDB。
它可以在最新的 Linux/FreeBSD/MacOS 上编译，支持 x86\_64 和 AArch64 CPU。

此代码库正在积极维护中，包含了超越原始 RemixDB 实现的优化。

# 新闻

* 实验性的 Python API 现已在 `xdb.py` 中提供。请参阅 `xdb.py` 末尾的示例。
* RemixDB 现在提供 `xdb_merge` 用于原子读-修改-写操作。
* 已添加两项优化来提升压缩和点查询性能（见下文）。

# 优化

## 优化 1: 最小化 REMIX (重新)构建成本

此实现采用了一项优化来最小化 REMIX 构建成本。
在随机写入实验中，与 REMIX 论文中描述的实现相比，此优化将吞吐量提升了 2 倍（0.96MOPS vs. 0.50MOPS）。
配置: klen=16; vlen=120; 20.2 亿个 KV; 256GB 有效 KV 数据; 单线程随机顺序加载; 无压缩。

创建新表文件时，RemixDB 可以创建表中所有键的副本。
具体来说，它使用前缀压缩按排序顺序编码所有键（不包括值），这创建了一个*压缩键块*（*CKB*）。
CKB 存储在表文件的末尾。
此功能可以自由开启和关闭。当包含和不包含 CKB 的表一起使用时，不存在兼容性问题。

创建新 REMIX 时，构建过程将检查每个输入表是否包含 CKB。
如果为真，该过程将使用这些 CKB 构建新的 REMIX。它还利用现有的 REMIX 来避免不必要的键比较。
通过这种方式，新的 REMIX 将通过读取旧的 REMIX 和 CKB 来创建，而无需访问表文件的键值数据块。

在运行系统中，旧的 REMIX 结构通常驻留在缓存中。
CKB 仅用于 REMIX 构建，批量读入内存，构建完成后即被丢弃。

除非系统管理具有小值的巨大键，否则 CKB 通常比原始键值数据块小得多。
假设平均 CKB 大小是平均键值数据块大小的 10%，
此优化用 10% 的额外写入 I/O 和存储空间使用量换取 REMIX 构建期间 90% 的读取 I/O 减少。

`remixdb_open` 打开/创建一个开启优化的 remixdb。每个新创建的 sstable 都将具有 CKB。
除非绝对有必要节省一点磁盘空间，否则您应该使用 `remixdb_open`。
`remixdb_open_compact` 打开一个关闭优化的 remixdb。每个新创建的 sstable 将不包含 CKB。
由其中一个函数创建的存储可以安全地由另一个函数打开。

## 优化 2: 使用哈希标签改进点查询

原始 RemixDB 中的点查询在段中执行二分搜索，这需要大约五次键比较，并可能导致多次 I/O。
当前实现提供了一个新选项，名为 `tags`（`remixdb_open` 的最后一个参数）。

启用此选项后，每个新的 REMIX 将存储一个 8 位哈希标签数组。每个标签对应于 REMIX 管理的一个键。
点查询（GET/PROBE）将首先像往常一样定位目标段。
然后它将检查标签以找到用于完整键匹配的候选键，而无需在段中使用二分搜索。
使用 8 位标签和段中最多 32 个键，如果找到键，点查询大约需要 1.06 次键比较，
如果键不存在，大约需要 0.12 次键比较。

TODO: 标签也可用于使用现有键加速迭代器搜索。

# 当前实现的限制

* *KV 大小*: 最大键+值大小限制为 65500 字节。
  这大致对应于 64KB 块大小限制。
  TODO: 将巨大的 KV 对存储在单独的文件中，并在 RemixDB 中存储 KV 对的文件地址。

# 配置和调优

## CPU 亲和性

RemixDB 采用后台线程执行异步压缩。
在可能的情况下（在 Linux 或 FreeBSD 上），这些线程被固定在特定的核心上以提高效率。
为了避免与前台线程的干扰，有必要分离不同线程使用的核心。
默认情况下，RemixDB 将 4 个压缩线程固定在当前进程亲和性列表的最后四个核心上。
例如，在具有两个 10 核处理器的机器上，核心 0,2,4,...,16,18 属于 numa 节点 0，
其余核心属于 numa 节点 1。
默认行为是使用核心 16 到 19，这是次优设置。
为了避免性能损失，应该使用 `numactl` 来指定 cpu 亲和性。

```
$ numactl -C 0,2,4,6,8 ./xdbdemo.out    # 压缩线程在 2,4,6,8

$ numactl -C 0,2,4,6,8,10,12,14 ./xdbtest.out 256 256 18 18 100    # 用户线程在 0,2,4,6; 压缩线程在 8,10,12,14
```

工作线程亲和性也可以使用 `xdb_open` 显式指定。

## 最大打开文件数

当前实现在运行时保持每个表文件打开。
这需要在 `/etc/security/limits.conf` 中设置较大的 `nofile`。
例如，将 `* - nofile 100000` 添加到 `limits.conf`，重启/重新登录，并使用 `ulimit -n` 进行双重检查。

## 最大表文件大小

`MSSTZ_NBLKS`（sst.c）控制 SST 文件中 4KB 块的最大数量。默认数量是 20400。
最大值是 65520（256MB 数据块，加上元数据）。

## 大页面

配置大页面可以有效改善 RemixDB 的性能。
通常几百个 2MB 大页面就足以用于 MemTable 中的内存分配。
块缓存自动检测并在可用时使用 1GB 大页面（否则，回退到 2MB 页面，然后是 4KB 页面）。
如果您将缓存大小设置为 4GB，应配置 4x 1GB 大页面。

# 开始使用

RemixDB 默认使用 `liburing`（`io_uring`），因此需要 Linux 内核 >= 5.1。
它也可以在所有支持的平台上使用 POSIX AIO，但性能可能会受到负面影响。

`clang` 是默认编译器。它通常比 GCC 产生更快的代码。要使用 GCC：

$ make CCC=gcc
强烈推荐 `jemalloc`。如果 jemalloc 可用且您更喜欢使用它，请在 `make` 时使用 `M=j`：

$ make M=j
类似地，可以使用 `M=t` 链接 `tcmalloc`。

`xdbdemo.c` 包含使用 `remixdb_*` 函数的示例代码。
这些函数提供了一个干净的编程接口，无需使用特殊的数据类型或结构。

## xdbdemo

编译并运行演示代码：

$ make M=j xdbdemo.out
$ ./xdbdemo.out
## xdbtest

`xdbtest` 是一个使用 `remixdb_*` 函数的压力测试程序。
它尝试使用亲和性列表上的所有可用核心，这可能导致平庸的性能。
您应该使用 numactl 来指定测试线程可用的核心。
假设您总共有八个核心（0...7），最佳实践是让测试器在前四个核心上运行，并将最后四个核心分配给压缩线程。以下示例使用此配置。

使用 4GB 块缓存、4GB MemTable 和具有 3200 万个 KV（2^25）的数据集运行，每轮执行 100 万次更新（2^20）：

$ make M=j xdbtest.out
$ numactl -N 0 ./xdbtest.out /tmp/xdbtest 4096 4096 25 20 100
使用更小的内存占用运行（256MB 块缓存、256MB Memtable 和 100 万个 KV）：

$ numactl -N 0 ./xdbtest.out /tmp/xdbtest 256 256 20 20 100
此设置消耗高达 850MB 内存（RSS）和 /tmp/xdbtest 中的 1.8GB 空间。

xdbtest.out 的首次运行应该总是显示 stale=0。
如果您在不删除 `/tmp/xdbtest` 的情况下再次运行它，
它会在开始时显示非零的陈旧数字，但会快速下降并最终达到零。

## xdbexit

`xdbexit` 是一个测试崩溃恢复的简单程序。
它插入一些新键并调用 `remixdb_sync()` 使所有缓冲数据在 WAL 中持久化。
然后它立即调用 `exit()` 而不进行任何清理。
重复运行它。在每次运行中，它应该显示找到了所有先前插入的 KV。

使用小占用运行：

$ for i in $(seq 1 30); do ./xdbexit.out ./dbdir 256 256; done
在常规大小设置中运行：

$ for i in $(seq 1 30); do ./xdbexit.out ./dbdir 4096 4096; done
## libremixdb.so

要将 remixdb 用作共享库，运行 `make libremixdb.so` 和 `make install`。
包含一个 PKGBUILD（用于 Archlinux 的 pacman）作为示例打包脚本。
