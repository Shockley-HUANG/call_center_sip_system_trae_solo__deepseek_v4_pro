# ============================================================
#  Call Center SIP Server - Makefile
#  千人企业呼叫中心模拟系统
#  技术栈: C + Lua + epoll + Socket
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

# --- 最终编译标志 ---
CFLAGS  += -I$(INC_DIR) $(LUA_CFLAGS) -D_GNU_SOURCE
LDFLAGS += $(LUA_LIBS) -lm -ldl -lpthread

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
all: check-lua dirs $(TARGET)
	@echo "$(GREEN)[✓] 编译成功: $(TARGET)$(RESET)"

# ============================================================
#  创建必要目录
# ============================================================
.PHONY: dirs
dirs:
	@mkdir -p $(BUILD_DIR) $(LOG_DIR)

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
.PHONY: run
run: all
	@echo "$(GREEN)[RUN] 启动 SIP Server...$(RESET)"
	@echo ""
	@LANG=zh_CN.UTF-8 $(TARGET)

.PHONY: demo
demo: all
	@echo "$(GREEN)[DEMO] 运行 Demo 测试 (3秒自动退出)...$(RESET)"
	@echo ""
	@echo "a" | LANG=zh_CN.UTF-8 timeout 5 $(TARGET) 2>&1; exit 0

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
#  安装 Lua 依赖 (Ubuntu/Debian)
# ============================================================
.PHONY: install-deps-ubuntu
install-deps-ubuntu:
	@echo "$(YELLOW)[APT] 安装 Lua 开发库...$(RESET)"
	sudo apt-get update
	sudo apt-get install -y liblua5.4-dev lua5.4 build-essential pkg-config valgrind

.PHONY: install-deps-centos
install-deps-centos:
	@echo "$(YELLOW)[YUM] 安装 Lua 开发库...$(RESET)"
	sudo yum install -y epel-release
	sudo yum install -y lua-devel lua gcc make pkgconfig valgrind

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
	@echo "$(GREEN)make clean$(RESET)           清理编译产物"
	@echo "$(GREEN)make distclean$(RESET)       清理编译产物和日志"
	@echo "$(GREEN)make debug$(RESET)           调试模式编译"
	@echo "$(GREEN)make gdb$(RESET)             编译 + GDB调试"
	@echo "$(GREEN)make valgrind$(RESET)        编译 + 内存泄漏检测"
	@echo "$(GREEN)make check-lua$(RESET)       检查 Lua 环境"
	@echo "$(GREEN)make install-deps-ubuntu$(RESET)  安装依赖 (Ubuntu/Debian)"
	@echo "$(GREEN)make install-deps-centos$(RESET)  安装依赖 (CentOS/RHEL)"
	@echo "$(GREEN)make help$(RESET)            显示此帮助"
	@echo ""