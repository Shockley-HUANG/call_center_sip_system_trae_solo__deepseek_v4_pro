# ============================================================
#  Call Center SIP Server - Makefile
#  千人企业呼叫中心模拟系统
#  技术栈: C + Lua + epoll + Socket + MySQL + Redis
#  Version: V4.2 — MySQL连接池 + Redis缓存 + 异步Worker
# ============================================================

# --- 编译器与标志 ---
CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -g -O2
LDFLAGS  =

# --- 目录结构 ---
SRC_DIR      = src
INC_DIR      = include
LUA_DIR      = lua
CONF_DIR     = conf
BUILD_DIR    = build
LOG_DIR      = log
DOCS_DIR     = docs
SQL_DIR      = sql

# --- 源文件 ---
SRCS    = $(wildcard $(SRC_DIR)/*.c)
OBJS    = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# --- Lua 配置（自动检测或手动指定）---
LUA_CFLAGS  =
LUA_LIBS    =
LUA_PC      = $(shell \
               if pkg-config --exists lua5.4 2>/dev/null; then echo lua5.4; \
               elif pkg-config --exists lua5.3 2>/dev/null; then echo lua5.3; \
               elif pkg-config --exists lua5.2 2>/dev/null; then echo lua5.2; \
               elif pkg-config --exists lua 2>/dev/null; then echo lua; \
               else echo ""; fi)

ifneq ($(LUA_PC),)
    LUA_CFLAGS  = $(shell pkg-config --cflags $(LUA_PC))
    LUA_LIBS    = $(shell pkg-config --libs $(LUA_PC))
else
    LUA_CFLAGS  = -I/usr/include/lua5.4 -I/usr/local/include/lua5.4 \
                  -I/usr/include/lua5.3 -I/usr/local/include/lua5.3 \
                  -I/usr/include/lua5.2 -I/usr/local/include/lua5.2 \
                  -I/usr/include/lua -I/usr/local/include
    LUA_LIBS    = -llua5.4 -llua5.3 -llua5.2 -llua
endif

# --- MySQL 配置（自动检测）---
MYSQL_CFLAGS  =
MYSQL_LIBS    =
MYSQL_PC      = $(shell \
               if pkg-config --exists mysqlclient 2>/dev/null; then echo mysqlclient; \
               elif pkg-config --exists mariadb 2>/dev/null; then echo mariadb; \
               else echo ""; fi)

ifneq ($(MYSQL_PC),)
    MYSQL_CFLAGS  = $(shell pkg-config --cflags $(MYSQL_PC))
    MYSQL_LIBS    = $(shell pkg-config --libs $(MYSQL_PC))
else
    MYSQL_CFLAGS  = -I/usr/include/mysql -I/usr/local/include/mysql
    MYSQL_LIBS    = -lmysqlclient
endif

# --- Redis (hiredis) 配置（自动检测）---
REDIS_CFLAGS  =
REDIS_LIBS    =
REDIS_PC      = $(shell \
               if pkg-config --exists hiredis 2>/dev/null; then echo hiredis; \
               else echo ""; fi)

ifneq ($(REDIS_PC),)
    REDIS_CFLAGS  = $(shell pkg-config --cflags $(REDIS_PC))
    REDIS_LIBS    = $(shell pkg-config --libs $(REDIS_PC))
else
    REDIS_CFLAGS  =
    REDIS_LIBS    = -lhiredis
endif

# --- 最终编译标志 ---
CFLAGS  += -I$(INC_DIR) $(LUA_CFLAGS) $(MYSQL_CFLAGS) $(REDIS_CFLAGS) -D_GNU_SOURCE
LDFLAGS += $(LUA_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS) -lm -ldl -lpthread

# --- 目标程序 ---
TARGET  = $(BUILD_DIR)/sip_server

# --- 颜色输出 ---
GREEN   = \033[32m
YELLOW  = \033[33m]
RED     = \033[31m
CYAN    = \033[36m
RESET   = \033[0m

# ============================================================
#  默认目标: 编译
# ============================================================
.PHONY: all
all: check-lua check-mysql check-redis dirs strip-bom $(TARGET)
	@echo "$(GREEN)[✓] 编译成功: $(TARGET)$(RESET)"

# ============================================================
#  创建必要目录
# ============================================================
.PHONY: dirs
dirs:
	@mkdir -p $(BUILD_DIR) $(LOG_DIR) $(SQL_DIR)

# ============================================================
#  剥离 C/H 源文件 BOM（GCC 不支持 BOM）
# ============================================================
define STRIP_BOM_SCRIPT
import sys, os, glob
for f in glob.glob("src/*.c") + glob.glob("include/*.h"):
    try:
        d = open(f, "rb").read()
        if d[:3] == b"\xef\xbb\xbf":
            open(f, "wb").write(d[3:])
    except Exception as e:
        print("BOM strip error: %s: %s" % (f, e), file=sys.stderr)
endef
export STRIP_BOM_SCRIPT

.PHONY: strip-bom
strip-bom:
	@python3 -c "$$STRIP_BOM_SCRIPT"

# ============================================================
#  编译目标程序
# ============================================================
$(TARGET): $(OBJS)
	@echo "$(CYAN)[LD] 链接...$(RESET)"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "$(CYAN)[CC] 编译 $< ...$(RESET)"
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================================
#  检查 Lua 开发环境
# ============================================================
.PHONY: check-lua
check-lua:
	@echo "$(CYAN)[CHECK] 检测 Lua 开发环境...$(RESET)"
	@if [ -n "$(LUA_PC)" ]; then \
		echo "  $(GREEN)[✓] pkg-config 检测到: $(LUA_PC)$(RESET)"; \
	elif pkg-config --exists lua 2>/dev/null; then \
		echo "  $(GREEN)[✓] pkg-config 检测到: lua$(RESET)"; \
	else \
		echo "  $(YELLOW)[!] pkg-config 未检测到 Lua, 尝试手动查找...$(RESET)"; \
	fi
	@echo "  CFLAGS: $(LUA_CFLAGS)"
	@echo "  LIBS:   $(LUA_LIBS)"

# ============================================================
#  检查 MySQL 开发环境
# ============================================================
.PHONY: check-mysql
check-mysql:
	@echo "$(CYAN)[CHECK] 检测 MySQL 开发环境...$(RESET)"
	@if [ -n "$(MYSQL_PC)" ]; then \
		echo "  $(GREEN)[✓] pkg-config 检测到: $(MYSQL_PC)$(RESET)"; \
	else \
		echo "  $(YELLOW)[!] pkg-config 未检测到 MySQL, 尝试默认路径...$(RESET)"; \
		echo "  $(YELLOW)[!] 可运行: sudo apt-get install libmysqlclient-dev$(RESET)"; \
	fi
	@echo "  CFLAGS: $(MYSQL_CFLAGS)"
	@echo "  LIBS:   $(MYSQL_LIBS)"

# ============================================================
#  检查 Redis (hiredis) 开发环境
# ============================================================
.PHONY: check-redis
check-redis:
	@echo "$(CYAN)[CHECK] 检测 Redis (hiredis) 开发环境...$(RESET)"
	@if [ -n "$(REDIS_PC)" ]; then \
		echo "  $(GREEN)[✓] pkg-config 检测到: $(REDIS_PC)$(RESET)"; \
	else \
		echo "  $(YELLOW)[!] pkg-config 未检测到 hiredis, 尝试默认路径...$(RESET)"; \
		echo "  $(YELLOW)[!] 可运行: sudo apt-get install libhiredis-dev$(RESET)"; \
	fi
	@echo "  CFLAGS: $(REDIS_CFLAGS)"
	@echo "  LIBS:   $(REDIS_LIBS)"

# ============================================================
#  清理
# ============================================================
.PHONY: clean
clean:
	@echo "$(YELLOW)[CLEAN] 清理编译产物...$(RESET)"
	rm -rf $(BUILD_DIR)/*.o $(TARGET)
	@echo "$(GREEN)[✓] 清理完成$(RESET)"

.PHONY: distclean
distclean: clean
	@echo "$(YELLOW)[CLEAN] 清理日志...$(RESET)"
	rm -rf $(LOG_DIR)/*.log
	@echo "$(GREEN)[✓] 深度清理完成$(RESET)"

# ============================================================
#  运行
# ============================================================
.PHONY: call-test
call-test: all
	@echo "$(GREEN)[TEST] 运行 50 次呼叫模拟测试 (含MySQL/Redis记录)...$(RESET)"
	@echo ""
	@echo "a" | LANG=zh_CN.UTF-8 timeout 8 $(TARGET) --call-test 2>&1; exit 0

.PHONY: run
run: all
	@echo "$(GREEN)[RUN] 启动 SIP Server...$(RESET)"
	@echo ""
	@LANG=zh_CN.UTF-8 $(TARGET)

.PHONY: demo
demo: all
	@echo "$(GREEN)[DEMO] 运行 Demo 测试 (3秒自动退出)...$(RESET)"
	@echo ""
	@echo "a" | LANG=zh_CN.UTF-8 timeout 5 $(TARGET) --demo 2>&1; exit 0

# ============================================================
#  调试模式
# ============================================================
.PHONY: debug
debug: CFLAGS += -DDEBUG -O0 -ggdb3
debug: all
	@echo "$(GREEN)[DEBUG] 调试模式编译完成$(RESET)"

.PHONY: gdb
gdb: debug
	@echo "$(GREEN)[GDB] 启动 GDB 调试...$(RESET)"
	gdb --args $(TARGET)

.PHONY: valgrind
valgrind: debug
	@echo "$(GREEN)[VALGRIND] 内存检测...$(RESET)"
	valgrind --leak-check=full --show-leak-kinds=all $(TARGET)

# ============================================================
#  安装依赖 (Ubuntu/Debian)
# ============================================================
.PHONY: install-deps-ubuntu
install-deps-ubuntu:
	@echo "$(YELLOW)[APT] 安装开发依赖...$(RESET)"
	sudo apt-get update
	sudo apt-get install -y liblua5.4-dev lua5.4 build-essential pkg-config valgrind
	sudo apt-get install -y libmysqlclient-dev libhiredis-dev
	@echo "$(GREEN)[✓] 所有依赖安装完成$(RESET)"

.PHONY: install-deps-centos
install-deps-centos:
	@echo "$(YELLOW)[YUM] 安装开发依赖...$(RESET)"
	sudo yum install -y epel-release
	sudo yum install -y lua-devel lua gcc make pkgconfig valgrind
	sudo yum install -y mysql-devel hiredis-devel
	@echo "$(GREEN)[✓] 所有依赖安装完成$(RESET)"

# ============================================================
#  初始化 MySQL 数据库 (需要 MySQL 服务运行中)
# ============================================================
.PHONY: init-db
init-db:
	@echo "$(CYAN)[DB] 初始化 MySQL 数据库...$(RESET)"
	@mysql -u root -p < $(SQL_DIR)/schema.sql && \
		echo "$(GREEN)[✓] 数据库初始化完成$(RESET)" || \
		echo "$(RED)[✗] 数据库初始化失败，请检查 MySQL 服务状态$(RESET)"

# ============================================================
#  帮助信息
# ============================================================
.PHONY: help
help:
	@echo "$(CYAN)============================================================$(RESET)"
	@echo "$(CYAN)  Call Center SIP Server - Makefile 使用指南$(RESET)"
	@echo "$(CYAN)============================================================$(RESET)"
	@echo ""
	@echo "$(GREEN)make$(RESET)                编译项目"
	@echo "$(GREEN)make run$(RESET)             编译 + 运行 (UTF-8)"
	@echo "$(GREEN)make demo$(RESET)            编译 + Demo测试 (自动退出)"
	@echo "$(GREEN)make call-test$(RESET)        编译 + 50次呼叫模拟 (自动退出)"
	@echo "$(GREEN)make init-db$(RESET)         初始化 MySQL 数据库 (需要root权限)"
	@echo "$(GREEN)make clean$(RESET)           清理编译产物"
	@echo "$(GREEN)make distclean$(RESET)       清理编译产物和日志"
	@echo "$(GREEN)make debug$(RESET)           调试模式编译"
	@echo "$(GREEN)make gdb$(RESET)             编译 + GDB调试"
	@echo "$(GREEN)make valgrind$(RESET)        编译 + 内存泄漏检测"
	@echo "$(GREEN)make check-lua$(RESET)       检查 Lua 环境"
	@echo "$(GREEN)make check-mysql$(RESET)     检查 MySQL 环境"
	@echo "$(GREEN)make check-redis$(RESET)     检查 Redis (hiredis) 环境"
	@echo "$(GREEN)make install-deps-ubuntu$(RESET)  安装依赖 (Ubuntu/Debian)"
	@echo "$(GREEN)make install-deps-centos$(RESET)  安装依赖 (CentOS/RHEL)"
	@echo "$(GREEN)make help$(RESET)            显示此帮助"
	@echo ""
