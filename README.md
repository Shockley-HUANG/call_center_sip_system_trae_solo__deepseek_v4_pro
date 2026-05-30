# 千人企业呼叫中心模拟系统

基于   C/C++ + Lua + SIP + RTP + epoll + MySQL + Redis   技术栈的企业级呼叫中心模拟系统，支持 1000 人规模企业内部通信、外部 400 总机 IVR 导航、24 小时人工客服。



当前版本

：V4.2 — MySQL连接池 + Redis缓存 + 异步Worker + 50次呼叫测试

## 开发工具

| 工具              | 版本 / 型号 | 用途                |
| --------------- | ------- | ----------------- |
| Trae Solo       | IDE     | AI 智能编程助手，全程辅助开发  |
| DeepSeek V4 PRO | LLM     | 大语言模型，驱动代码生成与架构设计 |
| GitHub CLI      | v2.92.0 | 版本管控与远程仓库自动化管理    |
| Git             | v2.54.0 | 分布式版本控制系统         |

## 项目结构

```
call_center_sip_system/
├── src/                    # C 源代码
│   ├── main.c              # 主程序入口（--demo / --call-test / Server 三模式）
│   ├── logger.c            # 日志工具实现
│   ├── lua_utils.c         # Lua 虚拟机封装（含 lua_vm_route_dispatch）
│   ├── epoll_socket.c      # epoll + Socket 底层封装（创建/注册/销毁）
│   ├── event_loop.c        # 事件循环与连接管理框架
│   ├── sip_handler.c       # SIP 协议栈（解析/验证/分发/响应）
│   ├── db_mysql.c           # MySQL 连接池 + 业务 CRUD（V2.0）
│   ├── db_redis.c           # Redis 缓存操作（KV/Hash/ZSet）
│   ├── db_sync.c            # 数据同步层（异步Worker+Cache Aside+运行时恢复）
│   └── call_test.c          # 50次呼叫模拟测试模块
├── include/                # C 头文件
│   ├── common_types.h      # 通用类型定义（11个路由结果码 + epoll常量）
│   ├── logger.h            # 日志宏接口
│   ├── lua_utils.h         # Lua 工具函数声明
│   ├── epoll_socket.h      # epoll/Socket 底层操作接口
│   ├── event_loop.h        # 事件循环回调 + 连接管理接口
│   ├── sip_handler.h       # SIP 报文处理接口（方法枚举/解析/分发）
│   ├── db_config.h          # 数据库配置+连接池槽位+异步任务队列类型
│   ├── db_mysql.h           # MySQL 连接池接口
│   ├── db_redis.h           # Redis 缓存接口
│   ├── db_sync.h            # 数据同步层接口
│   └── call_test.h          # 呼叫测试接口
├── lua/                    # Lua 业务脚本
│   └── route.lua           # 全场景商用路由脚本 V2.0（7大标准接口+_TEST_FORCE_WORKTIME）
├── conf/                   # 配置文件
│   └── sip_server.conf     # 服务主配置（含MySQL/Redis预留段）
├── sql/                    # 数据库脚本
│   └── schema.sql          # MySQL建表DDL（6张表+9部门种子数据）
├── tools/                  # 辅助脚本
│   ├── check_db.sh          # 数据库内容检查
│   ├── setup_mysql.sh       # MySQL环境初始化
│   └── full_test.sh         # 全流程测试脚本
├── build/                  # 编译产物（自动生成）
├── log/                    # 运行日志（自动生成）
├── docs/                   # 项目设计文档（永久保留）
│   └── 千人企业呼叫中心模拟系统完整设计方案.md  (V0.2)
├── .trae/                  # 项目规则与 AI 配置
│   └── rules/
│       ├── project_rules.md        # 项目编码/构建规则
│       └── ai_dev_constraints.md   # AI 开发约束规则
├── tasks.md                # 项目迭代任务清单
├── ISSUES_LOG.md           # 项目问题记录日志
└── Makefile                # 编译构建系统
```

## 快速开始

### 1. 环境要求

| 依赖         | 版本                   | 说明               |
| ---------- | -------------------- | ---------------- |
| GCC        | >= 7.0               | C11 标准支持         |
| GNU Make   | >= 4.0               | 构建工具             |
| Lua        | 5.2 / 5.3 / 5.4      | 开发库 (liblua-dev) |
| pkg-config | 任意                   | 自动检测依赖           |
| MySQL      | >= 8.0                | 持久化存储（libmysqlclient-dev） |
| Redis      | >= 7.0                | 高速缓存（libhiredis-dev） |
| Linux      | Ubuntu 20.04+ / WSL2 | 运行环境（需要 epoll 支持）|

### 2. 安装依赖



Ubuntu / Debian / WSL：



```bash
make install-deps-ubuntu
# 或手动:
sudo apt-get update
sudo apt-get install -y liblua5.4-dev lua5.4 build-essential pkg-config
sudo apt-get install -y libmysqlclient-dev libhiredis-dev mysql-server
```



CentOS / RHEL：



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

### 4. 运行

```bash
# 运行 Demo 测试（编译 + 6组全场景测试，5秒自动退出）
make demo

# 运行 50 次呼叫模拟测试（编译 + 全场景覆盖，自动写入MySQL）
make call-test

# 启动 epoll 服务端（编译 + SIP 5060 端口监听）
make run
```

三种启动模式：

```bash
./build/sip_server              # 默认：启动 epoll 服务端，监听 SIP 5060
./build/sip_server --demo       # Demo：运行 6 组路由测试
./build/sip_server --call-test  # Call Test：50次呼叫模拟 + 写入MySQL
```



注意

：在 WSL 环境中手动运行时，必须加 `LANG=zh_CN.UTF-8` 避免中文乱码：

```bash
LANG=zh_CN.UTF-8 ./build/sip_server
```

### 5. 其他命令

```bash
make clean           # 清理编译产物
make distclean       # 清理编译产物 + 日志
make init-db          # 初始化 MySQL 数据库（需要 sudo mysql）
make check-mysql      # 检查 MySQL 开发环境
make check-redis      # 检查 Redis 开发环境
make debug           # 调试模式编译（-O0 -ggdb3）
make gdb             # 编译并启动 GDB 调试
make valgrind        # 编译并检测内存泄漏
make help            # 显示所有命令
```

## 架构说明

### 分层架构

```
┌─────────────────────────────────────────┐
│          Lua 业务路由层                  │
│  IVR 导航 | 号段匹配 | 人工兜底 | 热更新   │
│  坐席状态管理 | 排队队列 | 时段判断 | 溢出  │
├─────────────────────────────────────────┤
│          C/C++ 通信引擎层                │
│  epoll 事件循环 | 连接管理 | 空闲检测     │
│  SIP 信令 | RTP 媒体 | Socket I/O       │
├─────────────────────────────────────────┤
│          数据存储层                      │
│  MySQL (员工/部门) | Redis (状态/队列)     │
└─────────────────────────────────────────┘
```

### epoll 高并发架构（V3.0 新增）

```
epoll_wait(100ms超时)
    │
    ├─ 网络事件分发 ──────────────────────
    │   ├─ EPOLLIN  → on_read()   读取数据
    │   ├─ EPOLLOUT → on_write()  可写通知
    │   ├─ EPOLLERR/EPOLLHUP → on_error()  异常处理
    │   └─ EPOLLRDHUP → on_close()  对端关闭
    │
    ├─ TCP 监听 → accept() → 注册客户端连接
    ├─ UDP SIP 5060 → 接收信令报文
    │
    └─ 定时任务（每 10s）
        ├─ 空闲连接检测（300s 超时释放）
        └─ on_idle() 业务周期回调
```

**核心能力**：
- epoll_create1 + epoll_ctl + epoll_wait 标准三步曲
- 连接槽位数组管理（MAX_CONCURRENT_CALLS=2048）
- 非阻塞 IO + EPOLLET（边缘触发预留）
- SO_REUSEADDR 端口复用
- 连接异常自动识别释放（错误/挂断/超时）
- 预留 SIP/RTP 业务回调接口

### C ↔ Lua 交互流程

```
C 程序 (main.c)
  │
  ├─ lua_vm_init()              → 创建 Lua 虚拟机, 加载标准库
  ├─ lua_vm_load_script()       → 加载 lua/route.lua
  │
  ├─ lua_vm_call_route()        → 调用 route_call(caller_id, digits)
  │       ├─ 时段判断 → 工作/夜间
  │       ├─ get_ivr_route()    → IVR 按键分发
  │       │    ├─ check_agent_status() → 坐席状态
  │       │    ├─ overflow_route()     → 全忙溢出
  │       │    ├─ invalid_key_fallback() → 无效按键
  │       │    └─ timeout_fallback()    → 超时兜底
  │       └─ dept_internal_call_route() → 内部分机
  │
  └─ lua_vm_route_dispatch()    → 调用任意7大标准接口
```

## 路由逻辑说明

### 400 总机 IVR 导航

客户拨打 `400-123-4567` → 时段判断 → 语音播报 → 按键分流：

| 按键     | 路由目标           | 号段        | 坐席数 |
| ------ | -------------- | --------- | :-: |
| 1      | 销售部            | 2001–2400 | 400 |
| 2      | 售后服务部          | 2501–2800 | 300 |
| 3      | 市场部            | 2901–2980 |  80 |
| 0      | 24h 人工服务台      | 9001–9050 |  50 |
| 超时5秒   | 重试引导 → 二次超时转人工 | —         |  —  |
| 无效按键3次 | 自动转人工坐席        | —         |  —  |

### 7大核心路由接口

| # | 接口                                         | 功能           |
| - | ------------------------------------------ | ------------ |
| 1 | `get_ivr_route(phone, key)`                | IVR 按键路由分发   |
| 2 | `check_agent_status(dept_id)`              | 部门坐席状态检测     |
| 3 | `overflow_route(dept_id, caller)`          | 坐席全忙溢出（4级策略） |
| 4 | `time_judge_route()`                       | 时段判断 + 夜间兜底  |
| 5 | `invalid_key_fallback(caller, key)`        | 无效按键容错兜底     |
| 6 | `timeout_fallback(caller)`                 | 超时无操作兜底      |
| 7 | `dept_internal_call_route(caller, callee)` | 内部分机互呼路由     |

### 内外权限隔离

-   外部来电  ：仅可访问销售、售后、市场、人工台（`external = true` 的部门）
-   内部员工  ：可拨打所有内部分机，全号段互通
- 人事、财务、行政、管理层、研发部 不对外暴露

### 全忙溢出策略（4级）

1. 溢出到公共人工组（support, 9000号段）
2. 按优先级溢出到其他对外部门
3. 所有对外部门全忙 → 进入排队队列
4. 排队满（>10人）→ 留言提示

### 24h 夜间兜底

-   工作时段  （周一至周五 08:00–18:00）：标准路由策略
-   非工作时段  ：转接值班人工坐席 → 无值班时播报留言提示

### 配置热更新

所有路由参数（超时时间、错误阈值、工作时段、溢出优先级）通过 `update_route_config()` 运行时热更新，无需重启 C 服务。

## 数据库初始化流程

### 自动建表机制

服务启动时自动执行 `sql/schema.sql`，包含 6 张表：

| # | 表名 | 用途 | 幂等性 |
|---|------|------|:---:|
| 1 | `departments` | 部门配置（9条种子） | `CREATE IF NOT EXISTS` |
| 2 | `extensions` | 员工分机 | `CREATE IF NOT EXISTS` |
| 3 | `agents` | 坐席状态 | `CREATE IF NOT EXISTS` |
| 4 | `call_records` | 通话记录 | `CREATE IF NOT EXISTS` |
| 5 | `call_logs` | 呼叫日志 | `CREATE IF NOT EXISTS` |
| 6 | `voicemails` | 用户留言 | `CREATE IF NOT EXISTS` |

### 启动流程

```
main.c: init_database()
  └─ db_sync_init()
       ├─ db_mysql_pool_connect_all()    # 8槽位连接池
       ├─ db_mysql_init_schema()          # 逐条执行 schema.sql
       └─ db_sync_seed_demo_data()        # 检查 agents>100 → 跳过; 否则生成1210员工+830坐席
```

### 重复运行行为

| 场景 | 结果 |
|------|------|
| 首次运行 | 自动建表 → 写入9部门 → 生成1210员工+830坐席 |
| 再次运行（已有DB） | `CREATE TABLE IF NOT EXISTS` 跳过建表；种子数据检测到 agents>100 自动跳过 |
| `make call-test` 重复执行 | 在既有 call_records 上**追加**50条新记录（不覆盖旧数据） |

### 数据存储位置

| 存储 | 位置 | Git |
|------|------|:--:|
| 表结构 + 部门种子 | `sql/schema.sql` | ✅ |
| 种子数据生成代码 | `src/db_sync.c` `db_sync_seed_demo_data()` | ✅ |
| MySQL 运行时数据 | `/var/lib/mysql/call_center/` (.ibd) | ❌ |

> 换机部署：只需 `make && make call-test`（前提 MySQL 已运行且有 `sip_user` 账户），程序自动建表+生成数据。

## 后续扩展

本项目预留了完整的 SIP/RTP 扩展接口，参见 `common_types.h` 中的：

- `call_session_t` — 通话会话结构体（含 SIP/RTP Socket FD、端口、状态）
- `agent_info_t` — 坐席信息结构体（在线状态、并发通话数）
- `route_response_t` — 路由响应结构体（11个结果码）

epoll 事件循环框架已为上层业务预留回调接口：
- `el_callbacks_t.on_accept` — 新连接接入
- `el_callbacks_t.on_read` — 数据可读（SIP报文接收）
- `el_callbacks_t.on_write` — 可写通知
- `el_callbacks_t.on_error` — 连接异常
- `el_callbacks_t.on_close` — 连接关闭
- `el_callbacks_t.on_idle` — 定时周期任务

后续开发按 tasks.md 任务清单逐步实现。

## 常见问题

### Q1: 编译报错 `fatal error: lua.h: No such file or directory`



原因

：未安装 Lua 开发库。



解决

：

```bash
# Ubuntu/Debian/WSL
sudo apt-get install liblua5.4-dev
# CentOS/RHEL
sudo yum install lua-devel
```

如果已安装但仍报错，手动指定路径：

```bash
find /usr -name "lua.h" 2>/dev/null
make CFLAGS="-I/usr/include/lua5.4" LDFLAGS="-llua5.4"
```

### Q2: 链接报错 `undefined reference to luaL_newstate`



原因

：链接时找不到 Lua 库，或库版本不匹配。



解决

：

```bash
ldconfig -p | grep lua
make LDFLAGS="-llua5.4"
```

### Q3: `pkg-config` 找不到 Lua



原因

：部分系统 Lua 的 `.pc` 文件路径不标准。



解决

：

```bash
find /usr -name "lua*.pc" 2>/dev/null
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
make CFLAGS="-I/usr/include/lua5.4" LDFLAGS="-llua5.4 -lm -ldl"
```

### Q4: 运行时找不到 route.lua



原因

：必须在项目根目录执行程序。



解决

：

```bash
cd /path/to/call_center_sip_system
./build/sip_server
# 或使用 make run / make demo
```

### Q5: 权限不足无法运行



解决

：

```bash
chmod +x build/sip_server
```

### Q6: 端口被占用（5060）

SIP 服务默认监听 5060 端口，可能端口冲突：

```bash
sudo netstat -tlnp | grep 5060
vim conf/sip_server.conf
```

### Q7: WSL 终端中文输出乱码



原因

：PowerShell → WSL 管道编码不匹配（GBK vs UTF-8）。



解决

：

```bash
# ✅ 正确方式
make run      # 已内置 LANG=zh_CN.UTF-8
make demo     # 同上

# ✅ 手动运行
LANG=zh_CN.UTF-8 ./build/sip_server

# ❌ 错误（会乱码）
wsl ./build/sip_server
```

详见 ISSUES\_LOG.md 问题 #3。

### Q8: Windows 编译报错 `unknown type name 'struct sockaddr_in'`



原因

：epoll/socket 为 Linux 专属 API，Windows 不支持。



解决

：在 WSL2 中编译运行本项目。

## 开发进展

| 时间               | 版本     | 内容                                                                                 |
| ---------------- | ------ | ---------------------------------------------------------------------------------- |
| 2026-05-30 14:30 | v4.2.0 | 迭代5完成：MySQL连接池(db_mysql)+Redis缓存(db_redis)+异步Worker线程(db_sync)+Cache Aside+重连风暴保护；schema.sql 6张表设计；1210名员工+830坐席种子数据；50次呼叫模拟测试(call_test)；_TEST_FORCE_WORKTIME测试模式；WSL编译零错误零警告+MySQL数据写入验证 |
| 2026-05-21 20:45 | v4.0.0 | 迭代4完成：UDP SIP协议栈（sip_handler.c/h），13种SIP方法识别，INVITE→Lua路由→SIP响应完整链路，BOM自动剥离+编译零错误零警告，Demo6组回归通过 |
| 2026-05-20 19:30 | v3.0.0 | 迭代3完成：epoll高并发服务端骨架（epoll_socket+event_loop+连接管理+空闲检测），编译零错误零警告，Demo+服务端模式验证通过 |
| 2026-05-19 11:00 | v2.0.0 | 补录：ISSUES\_LOG 问题#3 终端中文乱码、README V2.0、project\_rules 终端编码规范、tasks.md 创建、全量 Git 推送 |
| 2026-05-19 10:30 | v2.0.0 | 迭代2完成：全场景商用路由（7大接口+坐席状态+排队+时段+容错+热配置），WSL编译验证通过                                    |
| 2026-05-18 22:30 | v0.1.0 | 修复 ISSUES\_LOG.md 问题 #2 缺失 — Git blob 未实际更新                                        |
| 2026-05-18 22:15 | v0.1.0 | 修复 README.md UTF-8 BOM 编码问题，解决 GitHub 页面中文乱码                                       |
| 2026-05-18 21:00 | v0.1.0 | 新增项目规则文件与问题记录日志，建立项目知识库备份                                                          |
| 2026-05-18 20:45 | v0.1.0 | README 格式规范化，补充开发工具信息                                                              |
| 2026-05-18 20:30 | v0.1.0 | 初始化呼叫中心 C + Lua 项目基础框架，完成 Demo 编译运行                                                |
