/*
 * db_mysql.c — MySQL 连接池 + 业务 CRUD 实现 V2.0
 * ============================================================
 * V2.0 核心改进：
 *   - 连接池架构：多槽位并发，每槽位独立 MYSQL 句柄
 *   - 借还模型：borrow() → 使用 → return()，消除全局锁
 *   - 重连风暴保护：单协作者标记 + 指数退避
 *   - 池级健康检测：borrow 时自动检测断连并触发协作者重连
 */

#include "db_mysql.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================
 * 连接池生命周期
 * ============================================================ */

mysql_pool_t *db_mysql_pool_create(const mysql_config_t *config)
{
    mysql_pool_t *pool = (mysql_pool_t *)calloc(1, sizeof(mysql_pool_t));
    if (!pool) {
        LOG_ERROR("db_mysql_pool_create: calloc failed");
        return NULL;
    }

    pool->config = *config;
    pool->pool_size = config->pool_size > 0 ? config->pool_size : MYSQL_POOL_DEFAULT_SIZE;
    if (pool->pool_size > MYSQL_POOL_MAX_SIZE) {
        pool->pool_size = MYSQL_POOL_MAX_SIZE;
    }

    pool->slots = (mysql_slot_t *)calloc((size_t)pool->pool_size, sizeof(mysql_slot_t));
    if (!pool->slots) {
        LOG_ERROR("db_mysql_pool_create: slots calloc failed");
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->pool_lock, NULL);
    pthread_cond_init(&pool->pool_cond, NULL);
    pool->reconnecting = 0;
    pool->reconnect_backoff_secs = DB_RECONNECT_BASE_SEC;

    LOG_INFO("db_mysql_pool_create: pool=%p size=%d for %s@%s:%u/%s",
             (void *)pool, pool->pool_size,
             config->user, config->host, config->port, config->database);
    return pool;
}

void db_mysql_pool_destroy(mysql_pool_t *pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->pool_lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->slots[i].conn) {
            mysql_close(pool->slots[i].conn);
            pool->slots[i].conn = NULL;
        }
    }
    pthread_mutex_unlock(&pool->pool_lock);

    free(pool->slots);
    pthread_mutex_destroy(&pool->pool_lock);
    pthread_cond_destroy(&pool->pool_cond);
    free(pool);
    LOG_INFO("db_mysql_pool_destroy: pool destroyed");
}

/* ============================================================
 * 单槽位连接建立 (内部函数)
 * ============================================================ */

static int slot_connect(mysql_pool_t *pool, int idx)
{
    mysql_slot_t *slot = &pool->slots[idx];

    if (slot->conn) {
        mysql_close(slot->conn);
    }

    slot->conn = mysql_init(NULL);
    if (!slot->conn) {
        LOG_ERROR("slot_connect[%d]: mysql_init failed", idx);
        return -1;
    }

    int timeout = pool->config.connect_timeout > 0 ? pool->config.connect_timeout : 5;
    int rtimeout = pool->config.read_timeout > 0 ? pool->config.read_timeout : 10;

    mysql_options(slot->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    mysql_options(slot->conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(slot->conn, MYSQL_OPT_READ_TIMEOUT, &rtimeout);

    MYSQL *ret = mysql_real_connect(
            slot->conn,
            pool->config.host,
            pool->config.user,
            pool->config.password,
            pool->config.database,
            pool->config.port,
            NULL, 0);

    if (!ret) {
        LOG_ERROR("slot_connect[%d]: failed - %s", idx, mysql_error(slot->conn));
        mysql_close(slot->conn);
        slot->conn = NULL;
        slot->connected = 0;
        return -1;
    }

    slot->connected = 1;
    slot->in_use = 0;
    return 0;
}

int db_mysql_pool_connect_all(mysql_pool_t *pool)
{
    if (!pool) return -1;

    int ok = 0;
    for (int i = 0; i < pool->pool_size; i++) {
        if (slot_connect(pool, i) == 0) {
            ok++;
        }
    }

    pool->active_count = ok;
    LOG_INFO("db_mysql_pool_connect_all: %d/%d slots connected", ok, pool->pool_size);
    return (ok > 0) ? 0 : -1;
}

int db_mysql_pool_health(mysql_pool_t *pool)
{
    if (!pool) return -1;
    return pool->active_count;
}

/* ============================================================
 * 重连风暴保护：单协作者 + 指数退避
 * ============================================================ */

static int pool_try_reconnect(mysql_pool_t *pool)
{
    /* 只有一个线程可以执行重连 */
    if (pool->reconnecting) {
        return -1;
    }

    time_t now = time(NULL);
    if (now - pool->last_reconnect_attempt < pool->reconnect_backoff_secs) {
        return -1;
    }

    pool->reconnecting = 1;
    pool->last_reconnect_attempt = now;

    LOG_INFO("pool_try_reconnect: attempting reconnection (backoff=%ds)...",
             pool->reconnect_backoff_secs);

    int ok = 0;
    for (int i = 0; i < pool->pool_size; i++) {
        if (!pool->slots[i].connected) {
            if (slot_connect(pool, i) == 0) {
                ok++;
            }
        } else {
            ok++;
        }
    }

    pool->active_count = ok;

    if (ok == pool->pool_size) {
        pool->reconnect_backoff_secs = DB_RECONNECT_BASE_SEC;
        LOG_INFO("pool_try_reconnect: all %d slots reconnected", ok);
    } else {
        pool->reconnect_backoff_secs *= 2;
        if (pool->reconnect_backoff_secs > DB_RECONNECT_MAX_SEC) {
            pool->reconnect_backoff_secs = DB_RECONNECT_MAX_SEC;
        }
        LOG_WARN("pool_try_reconnect: %d/%d slots ok, next backoff=%ds",
                 ok, pool->pool_size, pool->reconnect_backoff_secs);
    }

    pool->reconnecting = 0;
    pthread_cond_broadcast(&pool->pool_cond);
    return (ok > 0) ? 0 : -1;
}

/* ============================================================
 * 连接借还 (核心并发接口)
 * ============================================================ */

MYSQL *db_mysql_pool_borrow(mysql_pool_t *pool, int timeout_ms)
{
    if (!pool) return NULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&pool->pool_lock);

    while (1) {
        /* 寻找空闲且已连接的槽位 */
        for (int i = 0; i < pool->pool_size; i++) {
            if (!pool->slots[i].in_use && pool->slots[i].connected) {
                pool->slots[i].in_use = 1;
                pthread_mutex_unlock(&pool->pool_lock);
                return pool->slots[i].conn;
            }
        }

        /* 没有可用槽位 → 尝试触发重连 */
        if (pool->active_count < pool->pool_size && !pool->reconnecting) {
            pthread_mutex_unlock(&pool->pool_lock);
            pool_try_reconnect(pool);
            pthread_mutex_lock(&pool->pool_lock);
            continue;
        }

        /* 等待归还或超时 */
        int wait_ret = pthread_cond_timedwait(
                &pool->pool_cond, &pool->pool_lock, &ts);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->pool_lock);
            LOG_WARN("db_mysql_pool_borrow: timeout after %dms", timeout_ms);
            return NULL;
        }
    }
}

void db_mysql_pool_return(mysql_pool_t *pool, MYSQL *conn)
{
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->pool_lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->slots[i].conn == conn) {
            pool->slots[i].in_use = 0;
            break;
        }
    }
    pthread_cond_signal(&pool->pool_cond);
    pthread_mutex_unlock(&pool->pool_lock);
}

/* ============================================================
 * 便捷函数：借 → 执行 → 还
 * ============================================================ */

int db_mysql_execute(mysql_pool_t *pool, const char *sql)
{
    MYSQL *conn = db_mysql_pool_borrow(pool, MYSQL_POOL_BORROW_TIMEOUT_MS);
    if (!conn) return -1;

    int ret = -1;
    if (mysql_real_query(conn, sql, (unsigned long)strlen(sql)) == 0) {
        ret = (int)mysql_affected_rows(conn);
    } else {
        LOG_ERROR("db_mysql_execute: query failed - %s [SQL: %.100s...]",
                  mysql_error(conn), sql);
    }

    db_mysql_pool_return(pool, conn);
    return ret;
}

MYSQL_RES *db_mysql_query(mysql_pool_t *pool, const char *sql)
{
    MYSQL *conn = db_mysql_pool_borrow(pool, MYSQL_POOL_BORROW_TIMEOUT_MS);
    if (!conn) return NULL;

    MYSQL_RES *result = NULL;
    if (mysql_real_query(conn, sql, (unsigned long)strlen(sql)) == 0) {
        result = mysql_store_result(conn);
        if (!result) {
            LOG_ERROR("db_mysql_query: store_result failed - %s",
                      mysql_error(conn));
        }
    } else {
        LOG_ERROR("db_mysql_query: query failed - %s [SQL: %.100s...]",
                  mysql_error(conn), sql);
    }

    db_mysql_pool_return(pool, conn);
    return result;
}

/* ============================================================
 * 业务 CRUD — 部门
 * ============================================================ */

int db_mysql_insert_department(mysql_pool_t *pool,
        const char *dept_key, const char *dept_name,
        const char *short_number, const char *range_start,
        const char *range_end, int external_access)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO departments (dept_key, dept_name, short_number, "
             "range_start, range_end, external_access) VALUES "
             "('%s','%s','%s','%s','%s',%d) "
             "ON DUPLICATE KEY UPDATE dept_name=VALUES(dept_name), "
             "range_start=VALUES(range_start), range_end=VALUES(range_end), "
             "external_access=VALUES(external_access)",
             dept_key, dept_name, short_number, range_start, range_end,
             external_access);
    return db_mysql_execute(pool, sql);
}

int db_mysql_load_departments(mysql_pool_t *pool)
{
    const char *sql = "SELECT COUNT(*) FROM departments";
    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) return -1;
    int count = 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) count = atoi(row[0]);
    mysql_free_result(res);
    return count;
}

/* ============================================================
 * 业务 CRUD — 分机
 * ============================================================ */

int db_mysql_insert_extension(mysql_pool_t *pool,
        const char *extension, const char *employee_name, int department_id)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO extensions (extension, employee_name, department_id) "
             "VALUES ('%s','%s',%d) "
             "ON DUPLICATE KEY UPDATE employee_name=VALUES(employee_name)",
             extension, employee_name, department_id);
    return db_mysql_execute(pool, sql);
}

int db_mysql_find_extension(mysql_pool_t *pool,
        const char *extension, char *dept_name_out, int dept_name_size)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "SELECT d.dept_name FROM extensions e "
             "JOIN departments d ON e.department_id = d.id "
             "WHERE e.extension = '%s' AND e.is_active = 1",
             extension);

    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) return -1;

    int found = 0;
    if (mysql_num_rows(res) > 0) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row[0] && dept_name_out) {
            strncpy(dept_name_out, row[0], (size_t)(dept_name_size - 1));
            dept_name_out[dept_name_size - 1] = '\0';
        }
        found = 1;
    }
    mysql_free_result(res);
    return found;
}

/* ============================================================
 * 业务 CRUD — 坐席
 * ============================================================ */

int db_mysql_upsert_agent(mysql_pool_t *pool,
        const char *extension, int department_id,
        int is_online, const char *status, int concurrent_calls)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO agents (extension, department_id, is_online, "
             "status, concurrent_calls) "
             "VALUES ('%s', %d, %d, '%s', %d) "
             "ON DUPLICATE KEY UPDATE is_online=%d, status='%s', "
             "concurrent_calls=%d, last_active=NOW()",
             extension, department_id, is_online, status, concurrent_calls,
             is_online, status, concurrent_calls);
    return db_mysql_execute(pool, sql);
}

int db_mysql_update_agent_status(mysql_pool_t *pool,
        const char *extension, const char *status, int concurrent_calls)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "UPDATE agents SET status='%s', concurrent_calls=%d, "
             "last_active=NOW() WHERE extension='%s'",
             status, concurrent_calls, extension);
    return db_mysql_execute(pool, sql);
}

/* Cache Aside 回源：从 MySQL 查询单个坐席 */
int db_mysql_query_agent_single(mysql_pool_t *pool,
        const char *extension, char *status_out, int status_size,
        int *concurrent_calls_out)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "SELECT status, concurrent_calls FROM agents "
             "WHERE extension = '%s'",
             extension);

    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) {
        if (status_out && status_size > 0) {
            strncpy(status_out, "offline", (size_t)(status_size - 1));
            status_out[status_size - 1] = '\0';
        }
        if (concurrent_calls_out) *concurrent_calls_out = 0;
        return -1;
    }

    int found = 0;
    if (mysql_num_rows(res) > 0) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (status_out && row[0]) {
            strncpy(status_out, row[0], (size_t)(status_size - 1));
            status_out[status_size - 1] = '\0';
        }
        if (concurrent_calls_out) {
            *concurrent_calls_out = row[1] ? atoi(row[1]) : 0;
        }
        found = 1;
    } else {
        if (status_out && status_size > 0) {
            strncpy(status_out, "offline", (size_t)(status_size - 1));
            status_out[status_size - 1] = '\0';
        }
        if (concurrent_calls_out) *concurrent_calls_out = 0;
    }

    mysql_free_result(res);
    return found ? 0 : -1;
}

int db_mysql_find_idle_agents(mysql_pool_t *pool,
        const char *dept_key, char *result_json, int result_size,
        int limit_count)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "SELECT a.extension FROM agents a "
             "JOIN departments d ON a.department_id = d.id "
             "WHERE d.dept_key = '%s' AND a.is_online = 1 "
             "AND a.status = 'idle' AND a.concurrent_calls = 0 "
             "ORDER BY a.last_active ASC LIMIT %d",
             dept_key, limit_count);

    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) return -1;

    int count = (int)mysql_num_rows(res);
    int offset = 0;
    offset += snprintf(result_json + offset, (size_t)(result_size - offset), "[");
    for (int i = 0; i < count; i++) {
        MYSQL_ROW row = mysql_fetch_row(res);
        offset += snprintf(result_json + offset, (size_t)(result_size - offset),
                           "%s\"%s\"", (i > 0 ? "," : ""), row[0] ? row[0] : "");
    }
    snprintf(result_json + offset, (size_t)(result_size - offset), "]");

    mysql_free_result(res);
    return count;
}

/* ============================================================
 * 业务 CRUD — 通话记录
 * ============================================================ */

int db_mysql_insert_call_record(mysql_pool_t *pool,
        const char *call_id, const char *caller, const char *callee,
        const char *direction, const char *department,
        const char *start_time, int result_code, const char *result_desc)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO call_records (call_id, caller_number, callee_number, "
             "direction, department, start_time, result_code, result_desc) "
             "VALUES ('%s','%s','%s','%s','%s','%s',%d,'%s')",
             call_id, caller, callee, direction,
             department ? department : "",
             start_time, result_code, result_desc ? result_desc : "");
    return db_mysql_execute(pool, sql);
}

int db_mysql_update_call_end(mysql_pool_t *pool,
        const char *call_id, const char *end_time,
        int duration, int sip_status, int result_code, const char *result_desc)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "UPDATE call_records SET end_time='%s', duration=%d, "
             "sip_status=%d, result_code=%d, result_desc='%s' "
             "WHERE call_id='%s'",
             end_time, duration, sip_status, result_code,
             result_desc ? result_desc : "", call_id);
    return db_mysql_execute(pool, sql);
}

/* ============================================================
 * 业务 CRUD — 呼叫日志
 * ============================================================ */

int db_mysql_insert_call_log(mysql_pool_t *pool,
        const char *call_id, const char *log_level,
        const char *event_type, const char *message,
        const char *caller, const char *callee, const char *department)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO call_logs (call_id, log_level, event_type, message, "
             "caller_number, callee_number, department) "
             "VALUES ('%s','%s','%s','%s','%s','%s','%s')",
             call_id ? call_id : "",
             log_level, event_type,
             message ? message : "",
             caller ? caller : "",
             callee ? callee : "",
             department ? department : "");
    return db_mysql_execute(pool, sql);
}

/* ============================================================
 * 业务 CRUD — 留言
 * ============================================================ */

int db_mysql_insert_voicemail(mysql_pool_t *pool,
        const char *caller_number, const char *department,
        const char *message_text, const char *file_path, int duration)
{
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO voicemails (caller_number, department, message_text, "
             "file_path, duration) "
             "VALUES ('%s','%s','%s','%s',%d)",
             caller_number,
             department ? department : "",
             message_text ? message_text : "",
             file_path ? file_path : "",
             duration);
    return db_mysql_execute(pool, sql);
}

/* ============================================================
 * Schema 初始化
 * ============================================================ */

int db_mysql_init_schema(mysql_pool_t *pool, const char *schema_path)
{
    FILE *fp = fopen(schema_path, "r");
    if (!fp) {
        LOG_WARN("db_mysql_init_schema: schema file not found: %s "
                 "(skipping auto-init)", schema_path);
        return -2;
    }

    LOG_INFO("db_mysql_init_schema: executing %s ...", schema_path);

    char line[DB_MAX_SQL_LEN];
    char statement[DB_MAX_SQL_LEN * 4];
    int stmt_len = 0;
    int stmt_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (trimmed[0] == '-' && trimmed[1] == '-') continue;
        if (trimmed[0] == '\n' || trimmed[0] == '\r' || trimmed[0] == '\0') continue;

        int len = (int)strlen(line);
        if (stmt_len + len < (int)(sizeof(statement) - 2)) {
            memcpy(statement + stmt_len, line, (size_t)len);
            stmt_len += len;
        }

        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            if (len >= 2 && line[len - 2] == '\r') line[len - 2] = '\0';
        }

        if (line[strlen(line) - 1] == ';') {
            statement[stmt_len] = '\0';
            char *s = statement;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

            if (strlen(s) > 0) {
                if (db_mysql_execute(pool, s) >= 0) {
                    stmt_count++;
                } else {
                    LOG_WARN("db_mysql_init_schema: statement failed: %.100s...", s);
                }
            }
            stmt_len = 0;
        }
    }

    fclose(fp);
    LOG_INFO("db_mysql_init_schema: executed %d statements", stmt_count);
    return stmt_count;
}

/* ============================================================
 * 缓存预热全量查询
 * ============================================================ */

int db_mysql_load_all_departments_for_cache(mysql_pool_t *pool)
{
    const char *sql = "SELECT dept_key, dept_name, short_number, "
                      "range_start, range_end, external_access, "
                      "min_agents_per_shift FROM departments";
    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) return -1;
    int count = (int)mysql_num_rows(res);
    mysql_free_result(res);
    return count;
}

int db_mysql_load_all_agents_for_cache(mysql_pool_t *pool)
{
    const char *sql = "SELECT COUNT(*) FROM agents";
    MYSQL_RES *res = db_mysql_query(pool, sql);
    if (!res) return -1;
    int count = 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) count = atoi(row[0]);
    mysql_free_result(res);
    return count;
}

/* ============================================================
 * 批量插入：支持超长 SQL (>4096 字节)
 * ============================================================ */

int db_mysql_execute_long(mysql_pool_t *pool, const char *sql, int sql_len)
{
    MYSQL *conn = db_mysql_pool_borrow(pool, MYSQL_POOL_BORROW_TIMEOUT_MS);
    if (!conn) return -1;

    int ret = -1;
    if (mysql_real_query(conn, sql, (unsigned long)sql_len) == 0) {
        ret = (int)mysql_affected_rows(conn);
    } else {
        LOG_ERROR("db_mysql_execute_long: query failed - %s [SQL_len=%d]",
                  mysql_error(conn), sql_len);
    }

    db_mysql_pool_return(pool, conn);
    return ret;
}
