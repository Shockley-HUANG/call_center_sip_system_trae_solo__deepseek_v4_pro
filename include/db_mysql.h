/*
 * db_mysql.h — MySQL 数据库操作接口
 * ============================================================
 * V2.0: 连接池架构替代单连接+全局锁
 * 提供连接池管理、连接级锁、业务 CRUD 接口。
 * 基于 libmysqlclient C API 封装。
 */

#ifndef DB_MYSQL_H
#define DB_MYSQL_H

#include "db_config.h"
#include "common_types.h"

#include <mysql/mysql.h>

/* ============================================================
 * MySQL 连接池：单槽位 (MYSQL 类型在此处可用)
 * ============================================================ */
typedef struct {
    MYSQL  *conn;
    int     in_use;
    int     connected;
} mysql_slot_t;

/* ============================================================
 * MySQL 连接池结构体
 * ============================================================ */
typedef struct {
    mysql_slot_t   *slots;
    int             pool_size;
    mysql_config_t  config;
    pthread_mutex_t pool_lock;
    pthread_cond_t  pool_cond;
    int             reconnecting;        /* 重连风暴保护: 1=正在重连 */
    time_t          last_reconnect_attempt;
    int             reconnect_backoff_secs;
    int             active_count;        /* 已连接槽位数 */
} mysql_pool_t;

/* ============================================================
 * 连接池生命周期
 * ============================================================ */

mysql_pool_t *db_mysql_pool_create(const mysql_config_t *config);

void          db_mysql_pool_destroy(mysql_pool_t *pool);

/* 预连接所有槽位 */
int           db_mysql_pool_connect_all(mysql_pool_t *pool);

/* 检查连接池健康度 */
int           db_mysql_pool_health(mysql_pool_t *pool);

/* ============================================================
 * 连接借还 (核心并发接口)
 * ============================================================ */

/* 从池中借出一个空闲连接，timeout_ms 超时返回 NULL */
MYSQL *db_mysql_pool_borrow(mysql_pool_t *pool, int timeout_ms);

/* 归还连接到池中 */
void   db_mysql_pool_return(mysql_pool_t *pool, MYSQL *conn);

/* ============================================================
 * 便捷函数：借 → 执行 → 还 (替代旧 API)
 * ============================================================ */

/* 执行非查询 SQL，返回影响行数 */
int db_mysql_execute(mysql_pool_t *pool, const char *sql);

/* 执行查询 SQL，返回 MYSQL_RES* (调用者必须 mysql_free_result) */
MYSQL_RES *db_mysql_query(mysql_pool_t *pool, const char *sql);

/* ============================================================
 * 业务 CRUD 接口 (基于连接池)
 * ============================================================ */

int db_mysql_insert_department(mysql_pool_t *pool,
        const char *dept_key, const char *dept_name,
        const char *short_number, const char *range_start,
        const char *range_end, int external_access);

int db_mysql_load_departments(mysql_pool_t *pool);

int db_mysql_insert_extension(mysql_pool_t *pool,
        const char *extension, const char *employee_name, int department_id);

int db_mysql_find_extension(mysql_pool_t *pool,
        const char *extension, char *dept_name_out, int dept_name_size);

int db_mysql_upsert_agent(mysql_pool_t *pool,
        const char *extension, int department_id,
        int is_online, const char *status, int concurrent_calls);

int db_mysql_update_agent_status(mysql_pool_t *pool,
        const char *extension, const char *status, int concurrent_calls);

/* 从 MySQL 查询单个坐席状态 (Cache Aside 回源用) */
int db_mysql_query_agent_single(mysql_pool_t *pool,
        const char *extension, char *status_out, int status_size,
        int *concurrent_calls_out);

int db_mysql_find_idle_agents(mysql_pool_t *pool,
        const char *dept_key, char *result_json, int result_size, int limit_count);

int db_mysql_insert_call_record(mysql_pool_t *pool,
        const char *call_id, const char *caller, const char *callee,
        const char *direction, const char *department,
        const char *start_time, int result_code, const char *result_desc);

int db_mysql_update_call_end(mysql_pool_t *pool,
        const char *call_id, const char *end_time,
        int duration, int sip_status, int result_code, const char *result_desc);

int db_mysql_insert_call_log(mysql_pool_t *pool,
        const char *call_id, const char *log_level,
        const char *event_type, const char *message,
        const char *caller, const char *callee, const char *department);

int db_mysql_insert_voicemail(mysql_pool_t *pool,
        const char *caller_number, const char *department,
        const char *message_text, const char *file_path, int duration);

/* Schema 初始化 */
int db_mysql_init_schema(mysql_pool_t *pool, const char *schema_path);

/* 缓存预热用全量查询 */
int db_mysql_load_all_departments_for_cache(mysql_pool_t *pool);
int db_mysql_load_all_agents_for_cache(mysql_pool_t *pool);

/* 批量插入辅助 (一次性构造并执行大 SQL) */
int db_mysql_execute_long(mysql_pool_t *pool, const char *sql, int sql_len);

#endif /* DB_MYSQL_H */
