#!/bin/bash

# 这个脚本对 xdb 数据库目录执行简单的 COW（写时复制）拷贝
# 原目录和目标目录必须在同一个支持硬链接的文件系统中
# 表文件将使用硬链接进行复制，不会真正拷贝数据
# WAL 文件会被真正拷贝，因为它们是可变的

# 检查命令行参数
if [[ $# -lt 2 ]]; then
  echo "usage: <orig-path> <dest-path>"
  echo "用法: <原目录路径> <目标目录路径>"
  exit 0
fi

orig=${1} dest=${2}  # 获取原目录和目标目录路径

# 检查原目录是否存在且包含有效的 HEAD 符号链接
if [[ ! -d ${orig} || ! -h ${orig}/HEAD ]]; then
  echo "${orig}/HEAD is not a symbolic link"
  echo "${orig}/HEAD 不是一个符号链接"
  exit 0
fi

# 检查目标目录是否已存在
if [[ -d ${dest} ]]; then
  echo "${dest} already exists; must use a non-existing path"
  echo "${dest} 已经存在；必须使用一个不存在的路径"
  exit 0
fi

# 创建目标目录
mkdir -p ${dest}
if [[ ! -d ${dest} ]]; then
  echo "creating ${dest} failed"
  echo "创建 ${dest} 失败"
  exit 0
fi

# 为不可变文件创建硬链接
# *.sstx：SST 表数据文件
# *.ssty：SST 表索引文件  
# *.ver：版本文件
cp -l ${orig}/*.sstx ${orig}/*.ssty ${orig}/*.ver ${dest}/

# 复制软链接 HEAD 和 HEAD1（指向某个 *.ver 文件）
# HEAD：当前数据库版本指针
# HEAD1：备份版本指针
cp -a ${orig}/HEAD ${dest}/HEAD
cp -a ${orig}/HEAD1 ${dest}/HEAD1

# 真正拷贝 WAL（Write-Ahead Log）文件
# wal1, wal2：预写日志文件，记录未提交的写操作
cp ${orig}/wal1 ${orig}/wal2 ${dest}/

echo "COW copy completed successfully"
echo "COW 拷贝完成"
echo "Source: ${orig}"
echo "源目录: ${orig}"
echo "Destination: ${dest}"
echo "目标目录: ${dest}"