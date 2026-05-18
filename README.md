# 千人企业呼叫中心模拟系统

基于 **C/C++ + Lua + SIP + RTP + epoll** 技术栈的企业级呼叫中心模拟系统，支持 1000 人规模企业内部通信、外部 400 总机 IVR 导航、24 小时人工客服。

## 开发工具

| 工具 | 版本 / 型号 | 用途 |
|------|-------------|------|
| Trae Solo | IDE | AI 智能编程助手，全程辅助开发 |
| DeepSeek V4 PRO | LLM | 大语言模型，驱动代码生成与架构设计 |
| GitHub CLI | v2.92.0 | 版本管控与远程仓库自动化管理 |
| Git | v2.54.0 | 分布式版本控制系统 |

## 项目结构

```
call_center_sip_system/
├── src/                    # C 源代码
│   ├── main.c              # 主程序入口 + Demo 测试
│   ├── logger.c            # 日志工具实现
│   └── lua_utils.c         # Lua 虚拟机封装（初始化/调用/栈操作）
├── include/                # C 头文件
│   ├── common_types.h      # 通用类型定义（会话、坐席、路由响应）
│   ├── logger.h            # 日志宏接口
│   └── lua_utils.h         # Lua 工具函数声明
├── lua/                    # Lua 业务脚本
│   └── route.lua           # 呼叫路由脚本（核心业务逻辑）
├── conf/                   # 配置文件
│   └── sip_server.conf     # 服务主配置
├── build/                  # 编译产物（自动生成）
├── log/                    # 运行日志（自动生成）
├── docs/                   # 项目设计文档（永久保留）
│   └── 千人企业呼叫中心模拟系统完整设计方案.md
├── .trae/                  # 项目规则与 AI 配置
│   └── rules/
│       └── project_rules.md
├── ISSUES_LOG.md           # 项目问题记录日志
└── Makefile                # 编译构建系统
```

## 快速开始

### 1. 环境要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| GCC | >= 7.0 | C11 标准支持 |
| GNU Make | >= 4.0 | 构建工具 |
| Lua | 5.2 / 5.3 / 5.4 | 开发库 (liblua-dev) |
| pkg-config | 任意 | 自动检测依赖 |
| Linux | Ubuntu 20.04+ / CentOS 7+ | 运行环境 |

### 2. 安装依赖

**Ubuntu / Debian：**
```bash
make install-deps-ubuntu
# 或手动:
sudo apt-get update
sudo apt-get install -y liblua5.4-dev lua5.4 build-essential pkg-config
```

**CentOS / RHEL：**
```bash
make install-deps-centos
# 或手动:
sudo yum install -y epel-release
sudo yum install -y lua-devel lua gcc make pkgconfig
```

### 3. 编译

```bash
cd call_center_sip_system
make
```

编译成功后生成 `build/sip_server` 可执行文件。

### 4. 运行 Demo

```bash
make run
```

程序将自动加载 `lua/route.lua` 路由脚本，运行 13 个呼叫路由测试用例 + 6 个内部互拨测试，输出路由结果。

### 5. 其他命令

```bash
make clean           # 清理编译产物
make distclean       # 清理编译产物 + 日志
make debug           # 调试模式编译（-O0 -ggdb3）
make gdb             # 编译并启动 GDB 调试
make valgrind        # 编译并检测内存泄漏
make check-lua       # 检查 Lua 开发环境
make help            # 显示所有命令
```

## 架构说明

### 分层架构

```
┌─────────────────────────────────────────┐
│          Lua 业务路由层                  │
│  IVR 导航 | 号段匹配 | 人工兜底 | 热更新   │
├─────────────────────────────────────────┤
│          C/C++ 通信引擎层                │
│  SIP 信令 | RTP 媒体 | epoll 调度 | Socket  │
├─────────────────────────────────────────┤
│          数据存储层                      │
│  MySQL (员工/部门) | Redis (状态/队列)     │
└─────────────────────────────────────────┘
```

### C ↔ Lua 交互流程

```
C 程序 (main.c)
  │
  ├─ lua_vm_init()         → 创建 Lua 虚拟机, 加载标准库
  ├─ lua_vm_load_script()  → 加载 lua/route.lua
  │
  └─ lua_vm_call_route()   → 调用 route_call(caller_id, digits)
       │                         │
       │                         ├─ is_external_caller()  判断内/外
       │                         ├─ route_external_call()  IVR 路由
       │                         ├─ route_internal_call()  分机直拨
       │                         └─ return { code, target, dept, desc }
       │
       └─ 解析 Lua table → route_response_t (C 结构体)
```

## 路由逻辑说明

### 400 总机 IVR 导航

客户拨打 `400-123-4567` → 语音播报 → 按键分流：

| 按键 | 路由目标 | 号段 |
|------|----------|------|
| 1 | 销售部 | 2000 号段 |
| 2 | 售后服务部 | 2500 号段 |
| 3 | 市场部 | 2900 号段 |
| 4 | 24h 人工服务台 | 9000 号段 |
| 超时/无效 | 人工兜底 | 9000 |

### 内外权限隔离

- **外部来电**：仅可访问销售、售后、市场、人工台（`external = true` 的部门）
- **内部员工**：可拨打所有内部分机，全号段互通
- 人事、财务、行政、管理层、研发部 不对外暴露

### 异常兜底

所有以下场景统一转入 24h 人工服务台 (9000)：
- IVR 超时 10 秒未操作
- 按键未匹配任何规则
- 部门坐席全忙溢出
- 拨打不存在的分机号

## 后续扩展

本项目预留了完整的 SIP/RTP 扩展接口，参见 `common_types.h` 中的：

- `call_session_t` — 通话会话结构体（含 SIP/RTP Socket FD、端口、状态）
- `agent_info_t` — 坐席信息结构体（在线状态、并发通话数）
- `route_response_t` — 路由响应结构体

后续开发按设计文档第八节任务清单逐步实现：
1. Lua 号段路由脚本（✅ 已完成）
2. C 调用 Lua 路由函数（✅ 已完成）
3. epoll 高并发 SIP 服务端
4. 24h 人工兜底策略脚本（✅ 已完成）
5. 会话状态管理、通话日志

## 常见问题

### Q1: 编译报错 `fatal error: lua.h: No such file or directory`

**原因**：未安装 Lua 开发库。

**解决**：
```bash
# Ubuntu/Debian
sudo apt-get install liblua5.4-dev
# CentOS/RHEL
sudo yum install lua-devel
```

如果已安装但仍报错，手动指定路径：
```bash
# 查看 lua.h 位置
find /usr -name "lua.h" 2>/dev/null
# 例如在 /usr/include/lua5.4/lua.h
# 编译时指定:
make CFLAGS="-I/usr/include/lua5.4" LDFLAGS="-llua5.4"
```

### Q2: 链接报错 `undefined reference to luaL_newstate`

**原因**：链接时找不到 Lua 库，或库版本不匹配。

**解决**：
```bash
# 确认已安装
ldconfig -p | grep lua
# 手动指定
make LDFLAGS="-llua5.4"
# 或创建符号链接
sudo ln -s /usr/lib/x86_64-linux-gnu/liblua5.4.so /usr/lib/liblua.so
```

### Q3: `pkg-config` 找不到 Lua

**原因**：部分系统 Lua 的 `.pc` 文件路径不标准。

**解决**：
```bash
# 检查 .pc 文件
find /usr -name "lua*.pc" 2>/dev/null
# 设置环境变量
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
# 或直接跳过 pkg-config，手动编译
make CFLAGS="-I/usr/include/lua5.4" LDFLAGS="-llua5.4 -lm -ldl"
```

### Q4: 运行时找不到 route.lua

**原因**：必须在项目根目录执行程序。

**解决**：
```bash
# 始终在项目根目录运行
cd /path/to/call_center_sip_system
./build/sip_server
# 或使用 make run（自动切换到项目目录）
make run
```

### Q5: 权限不足无法运行

**解决**：
```bash
chmod +x build/sip_server
```

### Q6: 端口被占用

如果后续启动了 SIP 服务（5060 端口），可能端口冲突：
```bash
# 查看端口占用
sudo netstat -tlnp | grep 5060
# 或修改配置文件中的端口
vim conf/sip_server.conf
```

## 开发进展

| 时间 | 版本 | 内容 |
|------|------|------|
| 2026-05-18 22:15 | v0.1.0 | 修复 README.md UTF-8 BOM 编码问题，解决 GitHub 页面中文乱码 |
| 2026-05-18 21:00 | v0.1.0 | 新增项目规则文件 (.trae/rules/project_rules.md) 与问题记录日志 (ISSUES_LOG.md)，建立项目知识库备份 |
| 2026-05-18 20:45 | v0.1.0 | README 格式规范化，补充开发工具信息（Trae Solo IDE、DeepSeek V4 PRO LLM） |
| 2026-05-18 20:30 | v0.1.0 | 初始化呼叫中心 C + Lua 项目基础框架，完成 Demo 编译运行；版本管控体系建立，GitHub 公开仓库创建，远程仓库重命名 |
