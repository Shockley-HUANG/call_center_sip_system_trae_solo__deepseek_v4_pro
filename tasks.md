# 项目迭代任务清单

## 已完成

| 迭代 | 任务 | 日期 |
|------|------|------|
| V0.1 | 项目框架搭建（Makefile/目录结构/配置文件） | — |
| V0.1 | 日志系统（logger.c/h） | — |
| V0.1 | Lua虚拟机封装层（lua_utils.c/h） | — |
| V0.1 | 基础路由脚本（route.lua V1.0） | — |
| V0.1 | 主入口Demo（main.c 19个测试用例） | — |
| V0.2 | 扩展路由结果码（common_types.h → 11个码） | 2026-05-19 |
| V0.2 | 新增 lua_vm_route_dispatch 通用路由调度 | 2026-05-19 |
| V0.2 | 全场景路由脚本（route.lua V2.0, 874行） | 2026-05-19 |
| V0.2 | 更新 main.c 全场景 Demo（6组测试） | 2026-05-19 |
| V0.2 | 设计文档更新至 V0.2 | 2026-05-19 |
| V0.2 | WSL 编译验证（GCC + Lua5.4, 0错误） | 2026-05-19 |
| V0.2 | WSL Demo运行验证（6组全部通过） | 2026-05-19 |
| V0.2 | 终端中文乱码问题修复（LANG=zh_CN.UTF-8） | 2026-05-19 |
| V0.2 | ISSUES_LOG 问题#3 记录 | 2026-05-19 |
| V0.2 | README V2.0 全面更新 | 2026-05-19 |
| V0.2 | project_rules 新增终端编码规范 | 2026-05-19 |
| V0.2 | tasks.md 任务清单创建 | 2026-05-19 |
| V0.3 | epoll 核心封装层（epoll_socket.h/c） | 2026-05-20 |
| V0.3 | 事件循环与连接管理框架（event_loop.h/c） | 2026-05-20 |
| V0.3 | common_types.h 新增 epoll/网络常量 | 2026-05-20 |
| V0.3 | main.c 集成服务端骨架（--demo / Server 双模式） | 2026-05-20 |
| V0.3 | SIP UDP 5060 端口监听 + 事件分发 | 2026-05-20 |
| V0.3 | 连接空闲超时检测 + 异常自动释放 | 2026-05-20 |
| V0.3 | WSL 编译验证（零错误零警告） | 2026-05-20 |
| V0.3 | Demo + Server 模式运行验证通过 | 2026-05-20 |
| V0.3 | 本次遇到 BOM 重复/残留问题修复（通用脚本固化） | 2026-05-20 |
| V0.3.1 | ET 边缘触发模式（EPOLLET + while(1)循环读） | 2026-05-20 |
| V0.3.1 | EPOLLOUT 按需注册/取消（send_buf + es_send_all） | 2026-05-20 |
| V0.3.1 | UDP recvfrom + 65536 大缓冲区（防丢包截断） | 2026-05-20 |
| V0.3.1 | 槽位 FD 复用安全（free_connection 清理 send_buf） | 2026-05-20 |
| V0.3.1 | connection_t 新增 send_buf/send_len/send_offset | 2026-05-20 |
| V0.3.1 | 新增 el_send_data() + es_send_all() 公共 API | 2026-05-20 |
| V0.3.1 | WSL 编译验证 + Demo + Server 模式回归通过 | 2026-05-20 |
| V0.3.1 | ISSUES_LOG 问题#6 代码审查四项修复记录 | 2026-05-20 |
| V4.0 | SIP协议栈（sip_handler.h/c）：13种SIP方法识别/解析/验证/分发/响应生成 | 2026-05-21 |
| V4.0 | SIP INVITE → Lua route_call → 路由结果 → SIP状态码映射完整链路 | 2026-05-21 |
| V4.0 | event_loop 公开 el_default_on_read 接口供SIP模块复用 | 2026-05-21 |
| V4.0 | Makefile 新增 strip-bom 编译前自动剥离BOM（解决IDE追加BOM致GCC报错） | 2026-05-21 |
| V4.0 | WSL 编译验证（零错误零警告）+ Demo 6组回归通过 | 2026-05-21 |
| V4.2 | 迭代任务5：MySQL/Redis数据库层设计 | 2026-05-30 |
| V4.2 | 数据库配置模块（db_config.h）：MySQL/Redis连接池配置+异步任务队列+重试常量 | 2026-05-30 |
| V4.2 | MySQL数据表设计（schema.sql）：6张表（departments/extensions/agents/call_records/call_logs/voicemails） | 2026-05-30 |
| V4.2 | MySQL连接池模块（db_mysql.h/c V2.0）：8槽位连接池+借还模型+重连风暴保护+批量CRUD | 2026-05-30 |
| V4.2 | Redis缓存模块（db_redis.h/c）：KV/Hash/ZSet操作+坐席状态/部门空闲/路由配置缓存 | 2026-05-30 |
| V4.2 | 数据同步层（db_sync.h/c V2.0）：异步Worker线程+生产者消费者队列+Cache Aside+运行时恢复 | 2026-05-30 |
| V4.2 | 代码审查改进：真异步队列+连接池替代全局锁+Cache Aside+Redis运行时恢复+重连风暴保护 | 2026-05-30 |
| V4.2 | 种子数据生成：1210名员工分机+830名坐席程序化生成 | 2026-05-30 |
| V4.2 | 呼叫测试模块（call_test.h/c）：50次综合呼叫模拟+14种场景覆盖 | 2026-05-30 |
| V4.2 | main.c 新增 --call-test CLI模式 | 2026-05-30 |
| V4.2 | route.lua 新增 _TEST_FORCE_WORKTIME 测试模式标志 | 2026-05-30 |
| V4.2 | Makefile 新增 mysql/hiredis pkg-config自动检测 + call-test目标 | 2026-05-30 |
| V4.2 | MySQL安装+数据库初始化+50次呼叫数据写入验证（departments:9/extensions:1210/agents:830/call_records:50） | 2026-05-30 |
| V4.2 | WSL 编译验证（零错误零警告）+ Demo + call-test 全通过 | 2026-05-30 |

## 未开始

| 迭代 | 任务 |
|------|------|
| V0.4 | RTP媒体流传输 |
| V0.4 | 会话状态管理（call_session_t生命周期） |
| V0.5 | 配置文件热加载 |
| V0.5 | 通话日志实时写入 call_logs 表 |
| V1.0 | 系统集成联调 |
| V1.0 | 压力测试（千级并发） |
