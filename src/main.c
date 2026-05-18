/*
 * main.c — 呼叫中心 SIP 服务主入口
 * ============================================================
 * 作为整个呼叫中心系统的启动入口，负责：
 *   1. 解析命令行参数，打印启动横幅
 *   2. 初始化日志系统（双通道：终端 + 文件）
 *   3. 注册信号处理（Ctrl+C 优雅退出）
 *   4. 创建并初始化 Lua 虚拟机，加载路由脚本
 *   5. 运行 19 个路由 Demo 测试用例（验证 C↔Lua 交互正确性）
 *   6. 进入事件循环，等待后续 SIP 连接
 *   7. 收到退出信号后，优雅关闭所有资源
 *
 * 编译依赖：
 *   common_types.h — 系统类型定义
 *   logger.h       — 日志系统
 *   lua_utils.h    — Lua 虚拟机封装
 *
 * 对应设计方案：
 *   第八章「可落地开发任务清单」第 1 项：Lua 号段路由脚本 ✓
 *   第八章「可落地开发任务清单」第 2 项：C 调用 Lua 路由函数 ✓
 *
 * 后续将在事件循环中接入 epoll + SIP Socket 处理真实通话。
 */

#include <stdio.h>      /* printf, fprintf, stderr */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* memset */
#include <signal.h>     /* signal, SIGINT, SIGTERM, SIGPIPE, SIG_IGN */
#include "common_types.h"
#include "logger.h"
#include "lua_utils.h"

/* ============================================================
 * 一、项目常量定义
 * ============================================================ */

/* Lua 路由脚本的相对路径（从项目根目录起算） */
#define LUA_SCRIPT_PATH  "lua/route.lua"

/* 项目名称（用于日志和横幅） */
#define PROJECT_NAME      "call_center_sip_server"

/* 项目版本号 */
#define PROJECT_VERSION   "1.0.0"

/* ============================================================
 * 二、信号处理
 * ------------------------------------------------------------
 * 信号处理是服务进程的基础能力：
 *   SIGINT  — Ctrl+C 手动终止
 *   SIGTERM — kill 命令终止
 *   SIGPIPE — 忽略（防止 Socket 断开时程序意外退出）
 *
 * volatile 关键字：告诉编译器 keep_running 可能被异步修改，
 * 确保 while(keep_running) 每次循环都从内存重新读取。
 * ============================================================ */

static volatile int keep_running = 1;

/*
 * 信号处理器
 * 将 keep_running 置为 0，主循环检测到后将执行优雅关闭流程。
 * (void)sig 抑制编译器"未使用参数"警告。
 */
static void signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
    LOG_INFO("Received shutdown signal, exiting gracefully...");
}

/*
 * 注册信号处理函数
 * SIGPIPE 使用 SIG_IGN（忽略），因为 Socket 断开是常见场景，
 * 程序应自行处理 write() 返回的 EPIPE 错误，而不是被信号杀死。
 */
static void setup_signal_handlers(void)
{
    signal(SIGINT,  signal_handler);   /* Ctrl+C  */
    signal(SIGTERM, signal_handler);   /* kill    */
    signal(SIGPIPE, SIG_IGN);          /* 忽略管道断开信号 */
}

/* ============================================================
 * 三、启动横幅
 * ============================================================ */

/*
 * 打印 ASCII 艺术风格的启动横幅
 * 包含项目名称和版本号，便于运维人员确认运行的版本。
 */
static void print_banner(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Call Center SIP Server  v%s                    ║\n", PROJECT_VERSION);
    printf("║   千人企业呼叫中心模拟系统 - C+Lua 框架            ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ============================================================
 * 四、呼叫路由 Demo 测试
 * ------------------------------------------------------------
 * 在 C 程序中通过 lua_vm_call_route() 调用 Lua 的 route_call()，
 * 验证 C↔Lua 交互链路正常工作。
 *
 * 测试覆盖场景（对照设计方案）：
 *   - 3.2节 400总机IVR导航（按键 1/2/3/4）
 *   - 3.2节 IVR超时自动转人工
 *   - 4.3节 部门坐席全忙溢出到人工台
 *   - 5.1节 内部分机直拨
 *   - 第七章 兜底异常流程（无效按键/无匹配路由）
 * ============================================================ */

/*
 * 测试一：外部400总机进线路由测试（13个用例）
 *
 * 测试流程：
 *   对每个测试用例，调用 lua_vm_call_route(vm, caller, digits, &resp)
 *   打印 caller、digits、路由结果码、目标分机、部门、描述信息。
 *   统计通过数（code == ROUTE_RESULT_SUCCESS，即 0）。
 */
static void run_route_demo(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════\n");
    printf("  呼叫路由 Demo 测试\n");
    printf("═══════════════════════════════════════════════════\n\n");

    /* 测试用例结构体：描述 + 主叫号码 + 按键/分机号 */
    typedef struct {
        const char *desc;      /* 用例描述 */
        const char *caller;    /* 主叫号码 */
        const char *digits;    /* IVR 按键或分机号 */
    } test_case_t;

    /* 13 个测试用例，覆盖设计方案中的所有路由场景 */
    test_case_t tests[] = {
        /* --- IVR 按键路由（设计方案 3.2 节）--- */
        {"外部客户按1 → 销售部",          "4001234567", "1"},
        {"外部客户按2 → 售后服务部",      "4001234567", "2"},
        {"外部客户按3 → 市场部",          "4001234567", "3"},
        {"外部客户按4 → 人工服务台",      "4001234567", "4"},

        /* --- IVR 异常场景（设计方案 3.2 + 7.3 节）--- */
        {"外部客户超时无操作 → 人工兜底", "4001234567", ""},       /* 空输入模拟超时 */
        {"外部客户按无效键 → 人工兜底",   "4001234567", "9"},      /* 按键不在 1~4 范围 */

        /* --- 内部分机互拨（设计方案 5.1 节）--- */
        {"内部员工拨1001 → 人事部",       "1002",       "1001"},   /* 人事部内部互拨 */
        {"内部员工拨2001 → 销售部",       "1001",       "2001"},   /* 跨部门互拨 */
        {"内部员工拨2501 → 售后服务部",   "1001",       "2501"},
        {"内部员工拨3001 → 研发部",       "1001",       "3001"},
        {"内部员工拨9001 → 客服中心",     "1001",       "9001"},

        /* --- 溢出/兜底场景（设计方案 4.3 + 7.3 节）--- */
        {"部门坐席全忙 → 溢出到人工台",   "4001234567", "1:full"}, /* ":full" 后缀触发全忙模拟 */
        {"未匹配路由 → 人工兜底",         "unknown",    "9999"},   /* 无效号码 + 未知主叫 */
    };

    int total = sizeof(tests) / sizeof(tests[0]);  /* 测试用例总数 */
    int success = 0;  /* 路由成功计数器 */

    /* 逐个执行测试 */
    for (int i = 0; i < total; i++) {
        route_response_t resp;
        memset(&resp, 0, sizeof(resp));  /* 清零响应结构体 */

        /* ★ 核心调用：C → Lua route_call() */
        int ret = lua_vm_call_route(vm, tests[i].caller, tests[i].digits, &resp);

        /* 打印测试结果 */
        printf("  [%2d/%2d] %s\n", i + 1, total, tests[i].desc);
        printf("           Caller: %-12s  Digits: %-6s\n",
               tests[i].caller,
               tests[i].digits[0] ? tests[i].digits : "(timeout)");  /* 空字符串显示为 timeout */
        printf("           Result: code=%-2d  target=%-6s  dept=%-10s\n",
               resp.code, resp.target_extension, resp.department);
        printf("           Desc:   %s\n", resp.description);

        /* 判断路由是否成功 */
        if (ret == ROUTE_RESULT_SUCCESS) {
            printf("           Status: \033[32m✓ SUCCESS\033[0m\n\n");
            success++;
        } else {
            /* 非 SUCCESS 不一定有问题（如超时→人工兜底是预期行为） */
            printf("           Status: \033[33m→ FALLBACK\033[0m\n\n");
        }
    }

    /* 汇总统计 */
    printf("═══════════════════════════════════════════════════\n");
    printf("  Demo 完成: %d/%d 测试通过\n", success, total);
    printf("═══════════════════════════════════════════════════\n\n");
}

/*
 * 测试二：内部员工互拨测试（6个用例）
 *
 * 额外验证内部分机之间的互通性，覆盖所有部门间的通信。
 */
static void run_internal_dial_demo(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════\n");
    printf("  内部分机互拨 Demo 测试\n");
    printf("═══════════════════════════════════════════════════\n\n");

    typedef struct {
        const char *desc;
        const char *caller;
        const char *callee;
    } test_case_t;

    test_case_t tests[] = {
        /* 同部门内部互拨 */
        {"销售部内部互拨",       "2001", "2010"},
        {"售后服务部内部互拨",   "2501", "2600"},
        {"研发部内部互拨",       "3001", "3150"},

        /* 跨部门互拨 */
        {"人事部 → 财务部",      "1001", "1101"},
        {"管理层 → 行政部",      "1301", "1201"},
        {"销售部 → 人事部(内部)", "2001", "1001"},  /* 内部员工可拨打所有分机 */
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int success = 0;

    for (int i = 0; i < total; i++) {
        route_response_t resp;
        memset(&resp, 0, sizeof(resp));

        int ret = lua_vm_call_route(vm, tests[i].caller, tests[i].callee, &resp);

        printf("  [%d/%d] %s\n", i + 1, total, tests[i].desc);
        printf("         Caller: %-6s → Callee: %-6s\n",
               tests[i].caller, tests[i].callee);
        printf("         Result: code=%-2d  target=%-6s  dept=%-10s\n",
               resp.code, resp.target_extension, resp.department);
        printf("         Desc:   %s\n", resp.description);

        if (ret == ROUTE_RESULT_SUCCESS) {
            printf("         Status: \033[32m✓ CONNECTED\033[0m\n\n");
            success++;
        } else {
            printf("         Status: \033[31m✗ BLOCKED\033[0m\n\n");
        }
    }

    printf("═══════════════════════════════════════════════════\n");
    printf("  Demo 完成: %d/%d 测试通过\n", success, total);
    printf("═══════════════════════════════════════════════════\n\n");
}

/* ============================================================
 * 五、主函数
 * ------------------------------------------------------------
 * 启动流程（严格按序执行）：
 *   1. 打印启动横幅
 *   2. 初始化日志系统
 *   3. 注册信号处理器
 *   4. 创建 + 初始化 Lua 虚拟机
 *   5. 加载路由脚本
 *   6. 运行 Demo 测试（验证 C↔Lua 链路）
 *   7. 进入主事件循环（等待 Ctrl+C）
 *   8. 收到信号后：销毁 Lua 虚拟机 → 关闭日志 → 退出
 *
 * 每个步骤失败时都有完整的错误处理和资源清理。
 * ============================================================ */

int main(int argc, char *argv[])
{
    /* 抑制"未使用参数"警告（当前版本不解析命令行参数） */
    (void)argc;
    (void)argv;

    /* 步骤1: 打印启动横幅 */
    print_banner();

    /* 步骤2: 初始化日志系统
     * 目录: ./log, 文件: sip_server.log, 级别: DEBUG（开发阶段输出所有日志） */
    if (logger_init("./log", "sip_server.log", LOG_LEVEL_DEBUG) != 0) {
        fprintf(stderr, "Logger initialization failed\n");
        return EXIT_FAILURE;
    }

    LOG_INFO("========================================");
    LOG_INFO("  %s v%s starting...", PROJECT_NAME, PROJECT_VERSION);
    LOG_INFO("========================================");

    /* 步骤3: 注册信号处理器
     * 必须在 while 循环之前注册，否则 Ctrl+C 可能无法优雅退出 */
    setup_signal_handlers();

    /* 步骤4: 创建并初始化 Lua 虚拟机 */
    lua_vm_t *vm = lua_vm_create();
    if (!vm) {
        LOG_FATAL("Failed to create Lua VM");
        return EXIT_FAILURE;
    }

    if (lua_vm_init(vm) != 0) {
        LOG_FATAL("Failed to initialize Lua VM");
        lua_vm_destroy(vm);
        return EXIT_FAILURE;
    }

    /* 步骤5: 加载路由脚本
     * 脚本路径在项目顶部定义：LUA_SCRIPT_PATH = "lua/route.lua"
     * 加载失败则 LOG_FATAL（含 exit） */
    if (lua_vm_load_script(vm, LUA_SCRIPT_PATH) != 0) {
        LOG_FATAL("Failed to load route script: %s", LUA_SCRIPT_PATH);
        lua_vm_destroy(vm);
        return EXIT_FAILURE;
    }

    LOG_INFO("Lua route script loaded, ready to route calls");

    /* 步骤6: 运行 Demo 测试
     * 验证 C↔Lua 路由链路完整可用 */
    run_route_demo(vm);          /* 13 个 IVR + 内部路由测试 */
    run_internal_dial_demo(vm);   /* 6 个内部互拨测试 */

    /* 步骤7: 进入主事件循环
     * 当前为占位实现：每秒检查一次 keep_running 标志
     * 后续版本将替换为 epoll_wait() 事件驱动循环 */
    LOG_INFO("Demo completed, server is ready for SIP connections");
    LOG_INFO("Press Ctrl+C to shutdown...");

    while (keep_running) {
#ifdef _WIN32
        Sleep(1000);     /* Windows: 毫秒 */
#else
        sleep(1);         /* Linux: 秒 */
#endif
    }

    /* 步骤8: 优雅关闭
     * 销毁顺序：Lua 虚拟机 → 日志系统（与初始化顺序相反） */
    LOG_INFO("Shutting down...");
    lua_vm_destroy(vm);
    logger_close();

    printf("\n[INFO] Server stopped gracefully.\n");
    return EXIT_SUCCESS;
}