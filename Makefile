# Makefile - RemixDB 项目构建配置文件
# 构建规则说明（目标文件总是以 .out 结尾）
# SRC-X.out += abc        # 额外源文件: abc.c
# MOD-X.out += abc        # 额外模块: abc.c abc.h
# ASM-X.out += abc        # 额外汇编文件: abc.S
# DEP-X.out += abc        # 额外依赖: abc
# FLG-X.out += -finline   # 额外编译标志
# LIB-X.out += abc        # 额外库链接选项 -labc

# X.out : xyz.h xyz.c # 用于指定需要编译/链接的额外依赖

# 可执行目标（X => X.out）
TARGETS += xdbdemo xdbtest xdbexit
# 单独的源文件（X => X.c）
SOURCES +=
SOURCES += $(EXTRASRC)
# 汇编文件（X => X.S）
ASSMBLY +=
ASSMBLY += $(EXTRAASM)
# 模块文件（X => X.c X.h）- 核心组件
MODULES += lib kv wh blkio sst xdb
MODULES += $(EXTRAMOD)
# 头文件（X => X.h）
HEADERS += ctypes
HEADERS += $(EXTRAHDR)

# EXTERNSRC/EXTERNDEP 不属于此代码仓库
# extern-src 会被链接
EXTERNSRC +=
# extern-dep 不会被链接
EXTERNDEP +=

# 编译标志
FLG +=
# 链接库（m表示数学库libm）
LIB += m

#### 默认构建目标
.PHONY : all
all : bin libremixdb.so sotest.out

# 构建共享库 libremixdb.so
libremixdb.so : Makefile Makefile.common lib.h kv.h wh.h blkio.h sst.h xdb.h lib.c kv.c wh.c blkio.c sst.c xdb.c
    # 设置共享库编译标志（包含位置无关代码 -fPIC）
	$(eval ALLFLG := $(CSTD) $(EXTRA) $(FLG) -shared -fPIC)
    # 设置链接库
	$(eval ALLLIB := $(addprefix -l,$(LIB) $(LIB-$@)))
    # 编译所有核心模块为共享库
	$(CCC) $(ALLFLG) -o $@ lib.c kv.c wh.c blkio.c sst.c xdb.c $(ALLLIB)
    # 去除调试符号，减小文件大小
	strip --strip-all --discard-all @remixdb.strip $@

# 构建共享库测试程序
sotest.out : sotest.c Makefile Makefile.common libremixdb.so remixdb.h
    # 设置编译标志
	$(eval ALLFLG := $(CSTD) $(EXTRA) $(FLG))
    # 编译测试程序，链接到当前目录的 libremixdb.so
	$(CCC) $(ALLFLG) -o $@ $< -L . -lremixdb
    # 显示运行提示（蓝色文字）
	@echo "$(shell $(TPUT) setaf 4)Now run $ LD_LIBRARY_PATH=. ./sotest.out$(shell $(TPUT) sgr0)"

# 系统安装目标
.PHONY : install
install : libremixdb.so remixdb.h
    # 安装共享库到系统库目录，权限755（可执行）
	install -D --mode=0755 libremixdb.so $(PREFIX)/lib/libremixdb.so
    # 安装头文件到系统头文件目录，权限644（只读）
	install -D --mode=0644 remixdb.h $(PREFIX)/usr/include/remixdb.h

# 包含通用构建规则（必须在这里包含）
include Makefile.common