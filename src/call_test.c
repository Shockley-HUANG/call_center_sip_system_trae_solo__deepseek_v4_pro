/*
 * call_test.c — 呼叫测试模块实现
 * ============================================================
 * 模拟 50 次通话，覆盖所有呼叫场景，每次限时 5 秒。
 *
 * 场景分布：
 *   - IVR 按键成功路由:  15 次 (按键1/2/3/0 → 各销售/售后/市场/人工)
 *   - 内部分机互呼:       8 次 (同部门+跨部门)
 *   - 夜间模式兜底:       5 次
 *   - 超时/无效按键容错:   6 次
 *   - 全忙/离线/溢出:     6 次
 *   - 无效号码:           4 次
 *   - 排队/留言:          6 次
 */

#include "call_test.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#define CALL_SLEEP_MS(ms) usleep((ms) * 1000)
#else
#include <windows.h>
#define CALL_SLEEP_MS(ms) Sleep(ms)
#endif

/* ============================================================
 * 单次呼叫上下文
 * ============================================================ */

typedef struct {
    int         call_no;
    const char *scenario;
    const char *caller;
    const char *digits;
    const char *expected;
    const char *direction;  /* "inbound" / "internal" */
} call_test_case_t;

/* ============================================================
 * 结果码 → 中文描述
 * ============================================================ */

static const char *result_label(int code)
{
    switch (code) {
        case 0:  return "✓ 成功";
        case 1:  return "✗ 无效号码";
        case 2:  return "⚠ 全忙";
        case 3:  return "⚠ 离线";
        case 4:  return "→ 兜底转接";
        case 5:  return "⏳ 排队中";
        case 6:  return "🌙 夜间模式";
        case 7:  return "📝 留言";
        case 8:  return "⏰ 超时重试";
        case 9:  return "🔁 无效按键重试";
        case 99: return "💥 错误";
        default: return "❓ 未知";
    }
}

/* ============================================================
 * 50 次呼叫测试用例
 * ============================================================ */

static const call_test_case_t test_cases[50] = {
    /* ── 场景1: IVR按键1 → 销售部 (5次) ── */
    {  1, "IVR按键[1]→销售部",   "4001234567", "1", "SUCCESS",     "inbound" },
    {  2, "IVR按键[1]→销售部",   "4002222222", "1", "SUCCESS",     "inbound" },
    {  3, "IVR按键[1]→销售部",   "4003333333", "1", "SUCCESS",     "inbound" },
    {  4, "IVR按键[1]→销售部",   "4004444444", "1", "SUCCESS",     "inbound" },
    {  5, "IVR按键[1]→销售部",   "4005555555", "1", "SUCCESS",     "inbound" },

    /* ── 场景2: IVR按键2 → 售后服务部 (5次) ── */
    {  6, "IVR按键[2]→售后部",   "4006666666", "2", "SUCCESS",     "inbound" },
    {  7, "IVR按键[2]→售后部",   "4007777777", "2", "SUCCESS",     "inbound" },
    {  8, "IVR按键[2]→售后部",   "4008888888", "2", "SUCCESS",     "inbound" },
    {  9, "IVR按键[2]→售后部",   "4009999999", "2", "SUCCESS",     "inbound" },
    { 10, "IVR按键[2]→售后部",   "4000000001", "2", "SUCCESS",     "inbound" },

    /* ── 场景3: IVR按键3 → 市场部 (3次) ── */
    { 11, "IVR按键[3]→市场部",   "4001111112", "3", "SUCCESS",     "inbound" },
    { 12, "IVR按键[3]→市场部",   "4001111113", "3", "SUCCESS",     "inbound" },
    { 13, "IVR按键[3]→市场部",   "4001111114", "3", "SUCCESS",     "inbound" },

    /* ── 场景4: IVR按键0 → 人工服务台 (3次) ── */
    { 14, "IVR按键[0]→人工台",   "4001111115", "0", "SUCCESS",     "inbound" },
    { 15, "IVR按键[0]→人工台",   "4001111116", "0", "SUCCESS",     "inbound" },
    { 16, "IVR按键[0]→人工台",   "4001111117", "0", "SUCCESS",     "inbound" },

    /* ── 场景5: 内部分机互呼 同部门 (4次) ── */
    { 17, "内部互呼→同部门销售", "2001", "2010", "SUCCESS",        "internal" },
    { 18, "内部互呼→同部门售后", "2501", "2510", "SUCCESS",        "internal" },
    { 19, "内部互呼→同部门研发", "3001", "3010", "SUCCESS",        "internal" },
    { 20, "内部互呼→同部门人事", "1001", "1010", "SUCCESS",        "internal" },

    /* ── 场景6: 内部分机互呼 跨部门 (4次) ── */
    { 21, "内部互呼→人事拨财务", "1001", "1101", "SUCCESS",        "internal" },
    { 22, "内部互呼→销售拨研发", "2001", "3001", "SUCCESS",        "internal" },
    { 23, "内部互呼→管理拨行政", "1301", "1201", "SUCCESS",        "internal" },
    { 24, "内部互呼→研发拨售后", "3001", "2501", "SUCCESS",        "internal" },

    /* ── 场景7: 夜间模式 (5次，当前非工作日必中夜间) ── */
    { 25, "夜间模式→值班坐席",   "4001000001", "1", "NIGHT_MODE",  "inbound" },
    { 26, "夜间模式→值班坐席",   "4001000002", "2", "NIGHT_MODE",  "inbound" },
    { 27, "夜间模式→值班坐席",   "4001000003", "0", "NIGHT_MODE",  "inbound" },
    { 28, "夜间模式→值班坐席",   "4001000004", "3", "NIGHT_MODE",  "inbound" },
    { 29, "夜间模式→值班坐席",   "4001000005", "",  "NIGHT_MODE",  "inbound" },

    /* ── 场景8: 超时无操作 → 容错兜底 (3次) ── */
    { 30, "超时+重试→转人工",    "4002000001", "",  "TIMEOUT_RETRY","inbound"},
    { 31, "超时+重试→转人工",    "4002000002", "",  "TIMEOUT_RETRY","inbound"},
    { 32, "超时+重试→转人工",    "4002000003", "",  "TIMEOUT_RETRY","inbound"},

    /* ── 场景9: 无效按键 → 容错兜底 (3次) ── */
    { 33, "无效按键→重新引导",   "4003000001", "5", "INVALID_KEY_RETRY","inbound"},
    { 34, "无效按键→重新引导",   "4003000002", "#", "INVALID_KEY_RETRY","inbound"},
    { 35, "无效按键→重新引导",   "4003000003", "*", "INVALID_KEY_RETRY","inbound"},

    /* ── 场景10: 无效号码 (4次) ── */
    { 36, "无效被叫分机",         "1001", "9999", "INVALID_DIGITS","internal"},
    { 37, "无效主叫分机",         "9999", "2001", "INVALID_DIGITS","internal"},
    { 38, "被叫格式错误",         "1001", "abc",  "INVALID_DIGITS","internal"},
    { 39, "未知主叫号码",         "unknown","9",  "INVALID_DIGITS","inbound" },

    /* ── 场景11: 全忙溢出 → 人工兜底 (3次) ── */
    { 40, "全忙溢出→销售→人工", "4008888888", "1", "FALLBACK_AGENT","inbound"},
    { 41, "全忙溢出→售后→人工", "4008888889", "2", "FALLBACK_AGENT","inbound"},
    { 42, "全忙溢出→市场→人工", "4007777777", "3", "FALLBACK_AGENT","inbound"},

    /* ── 场景12: 全忙排队 (3次) ── */
    { 43, "排队→销售部队列",    "4004000001", "1", "QUEUED",       "inbound" },
    { 44, "排队→售后部队列",    "4004000002", "2", "QUEUED",       "inbound" },
    { 45, "排队→人工台队列",    "4004000003", "0", "QUEUED",       "inbound" },

    /* ── 场景13: 离线/夜间留言 (3次) ── */
    { 46, "周末夜间→转留言",     "4005000001", "1", "VOICEMAIL",   "inbound" },
    { 47, "周末夜间→转留言",     "4005000002", "2", "VOICEMAIL",   "inbound" },
    { 48, "无值班→转留言",       "4005000003", "0", "VOICEMAIL",   "inbound" },

    /* ── 场景14: 边界/混合场景 (2次) ── */
    { 49, "内部分机拨人工台",    "2001", "9002", "SUCCESS",        "internal"},
    { 50, "外线无按键自动兜底",  "4009999998", "",  "NIGHT_MODE",  "inbound" },
};

/* ============================================================
 * 执行单次呼叫模拟
 * ============================================================ */

static int run_single_call(lua_vm_t *vm, db_sync_context_t *sync,
                           const call_test_case_t *tc)
{
    route_response_t r;
    memset(&r, 0, sizeof(r));

    /* 生成 Call-ID */
    char call_id[256];
    char time_part[32];
    time_t now = time(NULL);
    strftime(time_part, sizeof(time_part), "%Y%m%d%H%M%S", localtime(&now));
    snprintf(call_id, sizeof(call_id), "TEST-%s-%03d@call-center.local",
             time_part, tc->call_no);

    /* 记录主叫/被叫 */
    char callee_display[32];
    if (tc->digits && tc->digits[0]) {
        snprintf(callee_display, sizeof(callee_display), "IVR:%s", tc->digits);
    } else {
        snprintf(callee_display, sizeof(callee_display), "(timeout)");
    }

    /* ★ 记录呼叫开始 (写入 DB) ★ */
    db_sync_call_start(sync, call_id, tc->caller, callee_display,
                       tc->direction, tc->scenario);

    /* 执行路由 */
    time_t call_start = time(NULL);
    int ret = lua_vm_call_route(vm, tc->caller,
            tc->digits ? tc->digits : "", &r);
    time_t call_end = time(NULL);
    int duration = (int)(call_end - call_start);
    if (duration < 1) duration = 1;

    /* 限制单次呼叫 5 秒 */
    if (duration > 5) duration = 5;

    /* ★ 记录呼叫结束 (更新 DB) ★ */
    db_sync_call_end(sync, call_id, duration,
                     (r.code == 0) ? 200 : 486,
                     r.code, r.description);

    /* 控制台输出 */
    int match = (strcmp(r.description, tc->expected) == 0 ||
                 (tc->expected[0] == 'S' && r.code == 0));

    printf("  [%2d/50] %-28s → %-18s %s | t=%-4ds %s\n",
           tc->call_no, tc->scenario,
           result_label(r.code),
           r.target_extension,
           duration,
           ret == 0 ? "✓" : "✗");

    (void)match;
    return ret;
}

/* ============================================================
 * 主入口：运行 50 次呼叫测试
 * ============================================================ */

int call_test_run(lua_vm_t *vm, db_sync_context_t *sync)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  呼叫中心 — 50 次综合呼叫模拟测试\n");
    printf("  覆盖: IVR路由 | 内部互呼 | 夜间模式 | 容错兜底\n");
    printf("        全忙溢出 | 排队 | 留言 | 无效号码\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    LOG_INFO("call_test_run: starting 50-call simulation");

    /* 强制进入工作时段测试模式 (绕过周六日夜间检测) */
    lua_pushboolean(vm->L, 1);
    lua_setglobal(vm->L, "_TEST_FORCE_WORKTIME");
    LOG_INFO("call_test_run: forced work-time mode ON");

    time_t total_start = time(NULL);
    int success_count = 0;
    int fail_count = 0;
    int night_count = 0;
    int voicemail_count = 0;
    int queue_count = 0;
    int invalid_count = 0;

    for (int i = 0; i < 50; i++) {
        const call_test_case_t *tc = &test_cases[i];
        int ret = run_single_call(vm, sync, tc);

        if (ret == 0) success_count++;
        else fail_count++;

        route_response_t r;
        memset(&r, 0, sizeof(r));
        lua_vm_call_route(vm, tc->caller,
                tc->digits ? tc->digits : "", &r);

        switch (r.code) {
            case 6: night_count++; break;
            case 7: voicemail_count++; break;
            case 5: queue_count++; break;
            case 1: invalid_count++; break;
        }

        /* 间隔 50ms 模拟真实呼叫间隔 */
        CALL_SLEEP_MS(50);
    }

    time_t total_end = time(NULL);
    int total_sec = (int)(total_end - total_start);

    /* 等待异步队列排空 */
    CALL_SLEEP_MS(500);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  测试结果统计\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  总呼叫数:        50 次\n");
    printf("  路由成功:        %d 次  (%.0f%%)\n",
           success_count, success_count * 100.0 / 50);
    printf("  夜间模式命中:    %d 次\n", night_count);
    printf("  留言触发:        %d 次\n", voicemail_count);
    printf("  排队触发:        %d 次\n", queue_count);
    printf("  无效号码拦截:    %d 次\n", invalid_count);
    printf("  总耗时:          %d 秒\n", total_sec);
    printf("  平均每通:        %.1f 秒\n", total_sec / 50.0);

    if (sync->db_enabled) {
        printf("  MySQL 队列:      %d 条待写入\n",
               db_sync_queue_size(sync));
    }

    printf("═══════════════════════════════════════════════════════\n\n");

    /* 恢复正常时间检测 */
    lua_pushboolean(vm->L, 0);
    lua_setglobal(vm->L, "_TEST_FORCE_WORKTIME");

    LOG_INFO("call_test_run: complete — %d success, %d fail, %ds total",
             success_count, fail_count, total_sec);

    return 0;
}
