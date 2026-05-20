/*
 * main.c — 呼叫中心 SIP 服务主入口
 * ============================================================
 * V3.0: epoll 高并发服务端骨架 + 全场景商用路由
 *
 * 启动模式：
 *   ./sip_server          启动 epoll 服务端（默认）
 *   ./sip_server --demo   运行路由 Demo 测试
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "common_types.h"
#include "logger.h"
#include "lua_utils.h"
#include "event_loop.h"
#include "epoll_socket.h"

#define LUA_SCRIPT_PATH  "lua/route.lua"
#define PROJECT_NAME      "call_center_sip_server"
#define PROJECT_VERSION   "3.0.0"

static volatile int keep_running = 1;
static event_loop_t *g_event_loop = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
    LOG_INFO("Received shutdown signal, exiting gracefully...");

    if (g_event_loop) {
        el_stop(g_event_loop);
    }
}

static void setup_signal_handlers(void)
{
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static void print_banner(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Call Center SIP Server  v%s                    ║\n", PROJECT_VERSION);
    printf("║   企业400呼叫中心 - epoll高并发 + 全场景路由        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

static const char *code_name(int code)
{
    switch (code) {
        case 0:  return "SUCCESS";
        case 1:  return "INVALID_DIGITS";
        case 2:  return "DEPT_FULL";
        case 3:  return "AGENT_OFFLINE";
        case 4:  return "FALLBACK_AGENT";
        case 5:  return "QUEUED";
        case 6:  return "NIGHT_MODE";
        case 7:  return "VOICEMAIL";
        case 8:  return "TIMEOUT_RETRY";
        case 9:  return "INVALID_KEY_RETRY";
        case 99: return "ERROR";
        default: return "UNKNOWN";
    }
}

/* ============================================================
 * Demo 1: IVR 按键路由 + 无效按键容错 + 超时兜底
 * ============================================================ */
static void demo_ivr_and_fallback(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 1: IVR 按键路由 + 容错兜底 (接口1/5/6)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    route_response_t r;

    struct { const char *desc; const char *phone; const char *key; } tests[] = {
        {"外部客户按键[1] → 销售部",       "4001234567", "1"},
        {"外部客户按键[2] → 售后服务部",   "4001234567", "2"},
        {"外部客户按键[3] → 市场部",       "4001234567", "3"},
        {"外部客户按键[0] → 人工服务台",   "4001234567", "0"},
        {"超时无操作 → 超时重试引导",      "4001234567", ""},
        {"二次超时无操作 → 转人工坐席",    "4001234567", ""},
        {"无效按键[5] 第1次 → 重新引导",   "4009999999", "5"},
        {"无效按键[#] 第2次 → 重新引导",   "4009999999", "#"},
        {"无效按键[*] 第3次 → 转人工坐席", "4009999999", "*"},
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < total; i++) {
        printf("  [%d/%d] %s\n", i + 1, total, tests[i].desc);
        printf("         phone=%-12s key=%s\n", tests[i].phone,
               tests[i].key[0] ? tests[i].key : "(timeout)");

        memset(&r, 0, sizeof(r));
        lua_vm_route_dispatch(vm, "get_ivr_route", &r, "ss",
                              tests[i].phone, tests[i].key);
        printf("         → %-18s target=%-8s %s\n",
               code_name(r.code), r.target_extension, r.description);
    }

    printf("\n  ✓ Demo 1 完成\n");
}

static void demo_agent_status_and_overflow(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 2: 坐席状态检测 + 全忙溢出 (接口2/3)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    route_response_t r;

    printf("  [坐席状态检测]\n");
    const char *depts[] = {"sales", "service", "market", "support", "hr", "rnd"};
    for (int i = 0; i < 6; i++) {
        memset(&r, 0, sizeof(r));
        lua_vm_route_dispatch(vm, "check_agent_status", &r, "s", depts[i]);
        printf("    %-10s → %-18s %s\n", depts[i], code_name(r.code), r.description);
    }

    printf("\n  [全忙溢出链路测试]\n");

    memset(&r, 0, sizeof(r));
    lua_vm_route_dispatch(vm, "overflow_route", &r, "ss", "sales", "4008888888");
    printf("    销售部全忙 → %-18s target=%-8s %s\n",
           code_name(r.code), r.target_extension, r.description);

    memset(&r, 0, sizeof(r));
    lua_vm_route_dispatch(vm, "overflow_route", &r, "ss", "market", "4007777777");
    printf("    市场部全忙 → %-18s target=%-8s %s\n",
           code_name(r.code), r.target_extension, r.description);

    printf("\n  ✓ Demo 2 完成\n");
}

static void demo_time_judge(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 3: 时段判断 + 夜间兜底路由 (接口4)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    route_response_t r;
    memset(&r, 0, sizeof(r));
    lua_vm_route_dispatch(vm, "time_judge_route", &r, "");
    printf("    当前时段: %-18s\n", code_name(r.code));
    printf("    描述: %s\n", r.description);

    memset(&r, 0, sizeof(r));
    lua_vm_call_function(vm, "get_work_time_status", NULL);
    if (lua_istable(vm->L, -1)) {
        lua_getfield(vm->L, -1, "is_work_time");
        int is_work = lua_toboolean(vm->L, -1);
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "time_desc");
        const char *desc = lua_tostring(vm->L, -1);
        lua_pop(vm->L, 1);

        printf("    工作日模式: %s\n", is_work ? "是" : "否");
        printf("    时段描述: %s\n", desc ? desc : "N/A");
    }
    lua_pop(vm->L, 1);

    printf("\n  ✓ Demo 3 完成\n");
}

static void demo_internal_call(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 4: 内部分机互呼路由 (接口7)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    route_response_t r;

    struct { const char *desc; const char *caller; const char *callee; } tests[] = {
        {"同部门互呼 → 销售部内部",         "2001", "2010"},
        {"跨部门互呼 → 人事→财务",          "1001", "1101"},
        {"跨部门互呼 → 销售→研发",          "2001", "3001"},
        {"跨部门互呼 → 管理层→行政",        "1301", "1201"},
        {"无效主叫分机",                     "9999", "2001"},
        {"无效被叫分机",                     "1001", "9999"},
        {"被叫格式错误",                     "1001", "abc"},
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < total; i++) {
        printf("  [%d/%d] %s\n", i + 1, total, tests[i].desc);
        printf("         %s → %s\n", tests[i].caller, tests[i].callee);

        memset(&r, 0, sizeof(r));
        lua_vm_route_dispatch(vm, "dept_internal_call_route", &r, "ss",
                              tests[i].caller, tests[i].callee);
        printf("         → %-18s target=%-8s %s\n",
               code_name(r.code), r.target_extension, r.description);
    }

    printf("\n  ✓ Demo 4 完成\n");
}

static void demo_route_call(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 5: route_call 主入口综合路由\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    route_response_t r;

    struct { const char *desc; const char *caller; const char *digits; } tests[] = {
        {"外部400→按键1(销售)",    "4001234567", "1"},
        {"外部400→按键2(售后)",    "4001234567", "2"},
        {"外部400→按键0(人工)",    "4001234567", "0"},
        {"外部400→超时无操作",     "4001234567", ""},
        {"外部未知→无效按键",      "unknown",    "9"},
        {"内部员工→拨1001(人事)",  "2001",       "1001"},
        {"内部员工→拨9002(客服)",  "1001",       "9002"},
        {"内部员工→拨2501(售后)",  "1001",       "2501"},
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int success = 0;
    for (int i = 0; i < total; i++) {
        printf("  [%d/%d] %s\n", i + 1, total, tests[i].desc);
        printf("         caller=%-12s digits=%-6s\n", tests[i].caller,
               tests[i].digits[0] ? tests[i].digits : "(timeout)");

        memset(&r, 0, sizeof(r));
        int ret = lua_vm_call_route(vm, tests[i].caller, tests[i].digits, &r);
        printf("         → %-18s target=%-8s %s\n",
               code_name(r.code), r.target_extension, r.description);
        if (ret == 0) success++;
    }

    printf("\n  综合通过: %d/%d\n", success, total);
    printf("  ✓ Demo 5 完成\n");
}

static void demo_aux_functions(lua_vm_t *vm)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Demo 6: 辅助接口测试\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    printf("  [IVR 菜单]\n");
    lua_vm_call_function(vm, "get_ivr_menu", NULL);
    if (lua_istable(vm->L, -1)) {
        int len = (int)lua_rawlen(vm->L, -1);
        for (int i = 1; i <= len; i++) {
            lua_rawgeti(vm->L, -1, i);
            lua_getfield(vm->L, -1, "key");
            const char *key = lua_tostring(vm->L, -1);
            lua_pop(vm->L, 1);
            lua_getfield(vm->L, -1, "name");
            const char *name = lua_tostring(vm->L, -1);
            lua_pop(vm->L, 1);
            printf("    按键[%s] → %s\n", key ? key : "?", name ? name : "?");
            lua_pop(vm->L, 1);
        }
    }
    lua_pop(vm->L, 1);

    printf("\n  [部门信息查询]\n");
    const char *query_depts[] = {"sales", "support", "hr", "rnd"};
    for (int i = 0; i < 4; i++) {
        lua_vm_call_function(vm, "get_department_info", "s", query_depts[i]);
        if (lua_istable(vm->L, -1)) {
            lua_getfield(vm->L, -1, "name");
            const char *name = lua_tostring(vm->L, -1);
            lua_pop(vm->L, 1);
            lua_getfield(vm->L, -1, "size");
            int size = (int)lua_tointeger(vm->L, -1);
            lua_pop(vm->L, 1);
            lua_getfield(vm->L, -1, "idle_agents");
            int idle = (int)lua_tointeger(vm->L, -1);
            lua_pop(vm->L, 1);
            printf("    %-10s: %s | %d人 | 空闲%d\n",
                   query_depts[i], name ? name : "?",
                   size, idle);
        }
        lua_pop(vm->L, 1);
    }

    printf("\n  [路由配置]\n");
    lua_vm_call_function(vm, "get_route_config", NULL);
    if (lua_istable(vm->L, -1)) {
        lua_getfield(vm->L, -1, "ivr_timeout");
        printf("    IVR超时: %ds\n", (int)lua_tointeger(vm->L, -1));
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "max_invalid_keys");
        printf("    无效按键上限: %d次\n", (int)lua_tointeger(vm->L, -1));
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "max_queue_size");
        printf("    排队上限: %d人\n", (int)lua_tointeger(vm->L, -1));
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "queue_timeout");
        printf("    排队超时: %ds\n", (int)lua_tointeger(vm->L, -1));
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "fallback_agent");
        printf("    兜底号码: %s\n", lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);

        lua_getfield(vm->L, -1, "night_duty_dept");
        printf("    夜间值班部门: %s\n", lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);
    }
    lua_pop(vm->L, 1);

    printf("\n  [IVR 欢迎语]\n");
    lua_vm_call_function(vm, "get_ivr_prompt", NULL);
    const char *prompt = lua_tostring(vm->L, -1);
    printf("    \"%s\"\n", prompt ? prompt : "N/A");
    lua_pop(vm->L, 1);

    printf("\n  ✓ Demo 6 完成\n");
}

static void run_demos(lua_vm_t *vm)
{
    demo_ivr_and_fallback(vm);
    demo_agent_status_and_overflow(vm);
    demo_time_judge(vm);
    demo_internal_call(vm);
    demo_route_call(vm);
    demo_aux_functions(vm);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  全部 Demo 测试完成!\n");
    printf("  覆盖 7 个核心接口:\n");
    printf("    1. get_ivr_route          2. check_agent_status\n");
    printf("    3. overflow_route         4. time_judge_route\n");
    printf("    5. invalid_key_fallback   6. timeout_fallback\n");
    printf("    7. dept_internal_call_route\n");
    printf("═══════════════════════════════════════════════════════\n\n");
}

/* ============================================================
 * 服务端模式：启动 epoll 服务
 * ============================================================ */
static int start_server_mode(lua_vm_t *vm)
{
    server_config_t config;
    memset(&config, 0, sizeof(config));

    config.sip_port          = DEFAULT_SIP_PORT;
    config.rtp_port_min      = DEFAULT_RTP_PORT_MIN;
    config.rtp_port_max      = DEFAULT_RTP_PORT_MAX;
    config.tcp_backlog       = SERVER_TCP_BACKLOG;
    config.max_connections   = MAX_CONCURRENT_CALLS;
    config.idle_timeout_sec  = CONNECTION_IDLE_TIMEOUT_SEC;
    config.epoll_timeout_ms  = EPOLL_WAIT_TIMEOUT_MS;
    config.epoll_max_events  = MAX_EPOLL_EVENTS;

    g_event_loop = el_create(&config);
    if (!g_event_loop) {
        LOG_FATAL("Failed to create event loop");
        return -1;
    }

    g_event_loop->user_data = vm;

    LOG_INFO("========================================");
    LOG_INFO("  SIP Server started on port %u", DEFAULT_SIP_PORT);
    LOG_INFO("  Max connections: %d", MAX_CONCURRENT_CALLS);
    LOG_INFO("  Idle timeout: %ds", CONNECTION_IDLE_TIMEOUT_SEC);
    LOG_INFO("  Press Ctrl+C to shutdown...");
    LOG_INFO("========================================");

    int ret = el_run(g_event_loop);

    return ret;
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(int argc, char *argv[])
{
    int demo_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            demo_mode = 1;
        }
    }

    print_banner();
    setup_signal_handlers();

    if (logger_init("./log", "sip_server.log", LOG_LEVEL_DEBUG) != 0) {
        fprintf(stderr, "Logger initialization failed\n");
        return EXIT_FAILURE;
    }

    LOG_INFO("========================================");
    LOG_INFO("  %s v%s starting...", PROJECT_NAME, PROJECT_VERSION);
    LOG_INFO("  Mode: %s", demo_mode ? "Demo Test" : "Server");
    LOG_INFO("========================================");

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

    if (lua_vm_load_script(vm, LUA_SCRIPT_PATH) != 0) {
        LOG_FATAL("Failed to load route script: %s", LUA_SCRIPT_PATH);
        lua_vm_destroy(vm);
        return EXIT_FAILURE;
    }

    LOG_INFO("Lua route script V2.0 loaded successfully");

    if (demo_mode) {
        run_demos(vm);
        LOG_INFO("All demos completed, server ready for SIP connections");
        LOG_INFO("Press Ctrl+C to shutdown...");

        while (keep_running) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
        }
    } else {
        if (start_server_mode(vm) != 0) {
            LOG_ERROR("Server mode exited with error");
        }
    }

    LOG_INFO("Shutting down...");

    if (g_event_loop) {
        el_destroy(g_event_loop);
        g_event_loop = NULL;
    }

    lua_vm_destroy(vm);
    logger_close();

    printf("\n[INFO] Server stopped gracefully.\n");
    return EXIT_SUCCESS;
}
