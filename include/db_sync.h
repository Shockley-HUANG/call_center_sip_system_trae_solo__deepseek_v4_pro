/*
 * db_sync.h — 数据同步层接口 V2.0
 * ============================================================
 * V2.0 核心改进：
 *   1. 真异步机制：生产者-消费者队列 + 独立 Worker 线程
 *   2. 连接池：每个 Worker 持有独立 MySQL 连接，消除全局锁竞争
 *   3. Cache Aside：Redis Miss → 自动回源 MySQL → 回写 Redis
 *   4. 运行时恢复：Redis 重连后自动全量预热
 *   5. 重连风暴保护：单协作者 + 指数退避
 *
 * 架构：
 *   [SIP 主线程] ──enqueue──→ [Task Queue] ──dequeue──→ [Worker 线程]
 *        │                         │                         │
 *    Redis HSET               (立即返回)              MySQL Pool Borrow
 *    (实时,微秒级)                                    mysql_real_query()
 *                                                     Pool Return
 */

#ifndef DB_SYNC_H
#define DB_SYNC_H

#include "db_config.h"
#include "db_mysql.h"
#include "db_redis.h"
#include "common_types.h"

/* ============================================================
 * 数据同步上下文 (主结构体)
 * ============================================================ */
typedef struct {
    mysql_pool_t    *mysql_pool;
    redis_context_t *redis;
    db_task_queue_t  queue;              /* 异步任务队列 */
    pthread_t        worker_thread;      /* 后台 Worker 线程 */
    int              worker_running;     /* 1=运行中 0=已停止 */
    int              db_enabled;         /* MySQL 池是否可用 */
    int              cache_enabled;      /* Redis 是否可用 */
    time_t           last_health_check;
    int              health_check_interval_sec;
} db_sync_context_t;

/* ============================================================
 * 生命周期管理
 * ============================================================ */

db_sync_context_t *db_sync_create(mysql_pool_t *pool, redis_context_t *redis);

void db_sync_destroy(db_sync_context_t *sync);

/* 初始化：连接池、建表、缓存预热、启动 Worker 线程 */
int db_sync_init(db_sync_context_t *sync, const char *schema_path);

/* 停止 Worker 线程 (阻塞等待线程退出) */
void db_sync_stop_worker(db_sync_context_t *sync);

/* 健康检查：MySQL + Redis 状态，自动触发 Redis 恢复预热 */
int db_sync_health_check(db_sync_context_t *sync);

/* ============================================================
 * 任务入队接口 (主线程调用，非阻塞)
 *
 * 这些函数将任务打包为 db_task_t 推入队列后立即返回，
 * Worker 线程异步消费队列中的任务并执行 MySQL 写入。
 * 队列满时丢弃最旧的任务（防止内存爆炸）。
 * ============================================================ */

/* 坐席状态变更 → Redis 实时更新 + 异步入队 MySQL 持久化 */
int db_sync_agent_status_change(db_sync_context_t *sync,
        const char *extension, const char *new_status, int concurrent_calls);

/* 坐席上下线 */
int db_sync_agent_online(db_sync_context_t *sync,
        const char *extension, int is_online);

/* 通话发起 → 异步入队 MySQL 写入 */
int db_sync_call_start(db_sync_context_t *sync,
        const char *call_id, const char *caller, const char *callee,
        const char *direction, const char *department);

/* 通话结束 → 异步入队 MySQL 更新 */
int db_sync_call_end(db_sync_context_t *sync,
        const char *call_id, int duration_sec,
        int sip_status, int result_code, const char *result_desc);

/* 部门空闲更新 */
int db_sync_dept_idle_update(db_sync_context_t *sync,
        const char *dept_key, int idle_count);

/* ============================================================
 * Cache Aside 查询接口 (Redis优先 → Miss时回源MySQL → 回写Redis)
 * ============================================================ */

/* 查询坐席状态 (Cache Aside) */
int db_sync_agent_status_query(db_sync_context_t *sync,
        const char *extension, char *status_out, int size,
        int *concurrent_calls_out);

/* 查询部门空闲坐席数 (Cache Aside) */
int db_sync_dept_idle_query(db_sync_context_t *sync,
        const char *dept_key);

/* ============================================================
 * 缓存预热 (启动时 + Redis 恢复时)
 * ============================================================ */

int db_sync_cache_warmup_depts(db_sync_context_t *sync);
int db_sync_cache_warmup_agents(db_sync_context_t *sync);
int db_sync_cache_warmup_all(db_sync_context_t *sync);

/* ============================================================
 * 种子数据生成 (首次部署时调用)
 * ============================================================ */

/* 按部门规模自动生成员工分机 + 坐席记录，写入 extensions + agents 表 */
int db_sync_seed_demo_data(db_sync_context_t *sync);

/* ============================================================
 * 队列监控 (运维接口)
 * ============================================================ */

int db_sync_queue_size(db_sync_context_t *sync);

#endif /* DB_SYNC_H */
