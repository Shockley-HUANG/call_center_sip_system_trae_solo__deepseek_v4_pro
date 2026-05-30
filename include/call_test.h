/*
 * call_test.h — 呼叫测试模块
 * ============================================================
 * 模拟 50 次通话测试，覆盖完整呼叫场景，
 * 每通电话限时 5 秒，记录到 MySQL + Redis。
 */

#ifndef CALL_TEST_H
#define CALL_TEST_H

#include "common_types.h"
#include "lua_utils.h"
#include "db_sync.h"

/* 运行 50 次呼叫模拟测试 */
int call_test_run(lua_vm_t *vm, db_sync_context_t *sync);

#endif /* CALL_TEST_H */
