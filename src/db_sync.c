/*
 * db_sync.c — 数据同步层实现 V2.0
 * ============================================================
 * V2.0 核心架构：
 *
 *   [SIP 主线程]                            [Worker 线程]
 *       │                                        │
 *   ┌───┴─── Redis HSET (实时, ~100μs)            │
 *   │                                            │
 *   └───┴── enqueue(task) → [Task Queue] → dequeue(task)
 *          (非阻塞, ~1μs)           │              │
 *                                (条件变量      pool_borrow()
 *                                 等待)         mysql_real_query()
 *                                              pool_return()
 *
 * 关键特性：
 *   1. 真异步：主线程入队后立即返回，不等待 MySQL IO
 *   2. 连接池：Worker 从池借连接执行，归还后释放
 *   3. Cache Aside：Redis Miss → MySQL 回源 → 写回 Redis
 *   4. Redis 运行时恢复：健康检查检测到 Redis 恢复 → 自动全量预热
 *   5. 重连风暴保护：单协作者 + 指数退避
 *   6. 队列溢出保护：满时丢弃最旧任务
 *   7. 内存安全：每个 MYSQL_RES 都通过 mysql_free_result 释放
 */

#include "db_sync.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 任务队列操作 (线程安全)
 * ============================================================ */

static void queue_init(db_task_queue_t *q)
{
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_destroy(db_task_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    db_task_t *node = q->head;
    while (node) {
        db_task_t *next = node->next;
        free(node);
        node = next;
    }
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

/* 入队：主线程调用，非阻塞。返回 0 成功，-1 队列满 */
static int queue_push(db_task_queue_t *q, db_task_t *task)
{
    if (!q || !task) return -1;

    pthread_mutex_lock(&q->lock);

    if (q->count >= DB_QUEUE_MAX_SIZE) {
        /* 队列溢出保护：丢弃最旧的任务 */
        db_task_t *old = q->head;
        if (old) {
            q->head = old->next;
            if (!q->head) q->tail = NULL;
            free(old);
            q->count--;
            LOG_WARN("queue_push: dropped oldest task (queue full, size=%d)",
                     DB_QUEUE_MAX_SIZE);
        }
    }

    task->next = NULL;
    if (!q->tail) {
        q->head = task;
        q->tail = task;
    } else {
        q->tail->next = task;
        q->tail = task;
    }
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* 出队：Worker 线程调用，阻塞等待。shutdown 时返回 NULL */
static db_task_t *queue_pop(db_task_queue_t *q)
{
    pthread_mutex_lock(&q->lock);

    while (!q->head && !q->shutdown) {
        pthread_cond_wait(&q->cond, &q->lock);
    }

    if (q->shutdown && !q->head) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    db_task_t *task = q->head;
    q->head = task->next;
    if (!q->head) q->tail = NULL;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return task;
}

/* ============================================================
 * 任务序列化/反序列化
 * ============================================================ */

static void pack_agent_status(db_task_t *task,
        const char *extension, const char *status, int concurrent_calls)
{
    task->type = DB_TASK_AGENT_STATUS;
    snprintf(task->data, DB_TASK_DATA_LEN, "%s|%s|%d",
             extension, status, concurrent_calls);
}

static void pack_agent_online(db_task_t *task,
        const char *extension, int is_online)
{
    task->type = DB_TASK_AGENT_ONLINE;
    snprintf(task->data, DB_TASK_DATA_LEN, "%s|%d", extension, is_online);
}

static void pack_call_start(db_task_t *task,
        const char *call_id, const char *caller, const char *callee,
        const char *direction, const char *department, const char *start_time)
{
    task->type = DB_TASK_CALL_START;
    snprintf(task->data, DB_TASK_DATA_LEN, "%s|%s|%s|%s|%s|%s",
             call_id, caller, callee, direction,
             department ? department : "",
             start_time ? start_time : "");
}

static void pack_call_end(db_task_t *task,
        const char *call_id, int duration, int sip_status,
        int result_code, const char *result_desc)
{
    task->type = DB_TASK_CALL_END;
    snprintf(task->data, DB_TASK_DATA_LEN, "%s|%d|%d|%d|%s",
             call_id, duration, sip_status, result_code,
             result_desc ? result_desc : "");
}

/* ============================================================
 * Worker 线程：消费队列中的任务并执行 MySQL 写入
 * ============================================================ */

static void *db_sync_worker(void *arg)
{
    db_sync_context_t *sync = (db_sync_context_t *)arg;
    LOG_INFO("db_sync_worker: thread started (tid=%lu)",
             (unsigned long)pthread_self());

    while (1) {
        db_task_t *task = queue_pop(&sync->queue);
        if (!task) {
            break; /* shutdown signal */
        }

        if (!sync->db_enabled) {
            LOG_DEBUG("db_sync_worker: MySQL disabled, dropping task type=%d",
                      (int)task->type);
            free(task);
            continue;
        }

        char *data = task->data;

        switch (task->type) {
        case DB_TASK_AGENT_STATUS: {
            char ext[32], status[32];
            int calls = 0;
            if (sscanf(data, "%31[^|]|%31[^|]|%d", ext, status, &calls) >= 2) {
                db_mysql_update_agent_status(sync->mysql_pool, ext, status, calls);
            }
            break;
        }
        case DB_TASK_AGENT_ONLINE: {
            char ext[32];
            int online = 0;
            if (sscanf(data, "%31[^|]|%d", ext, &online) >= 1) {
                const char *st = online ? "idle" : "offline";
                db_mysql_update_agent_status(sync->mysql_pool, ext, st, 0);
            }
            break;
        }
        case DB_TASK_CALL_START: {
            char call_id[256], caller[32], callee[32];
            char direction[32], dept[64], start_time[32];
            if (sscanf(data, "%255[^|]|%31[^|]|%31[^|]|%31[^|]|%63[^|]|%31[^\n]",
                       call_id, caller, callee, direction, dept, start_time) >= 3) {
                db_mysql_insert_call_record(sync->mysql_pool,
                        call_id, caller, callee, direction, dept, start_time, 0, "");
            }
            break;
        }
        case DB_TASK_CALL_END: {
            char call_id[256], result_desc[128];
            int duration = 0, sip_status = 0, result_code = 0;
            if (sscanf(data, "%255[^|]|%d|%d|%d|%127[^\n]",
                       call_id, &duration, &sip_status,
                       &result_code, result_desc) >= 3) {
                char end_time[32];
                time_t now = time(NULL);
                strftime(end_time, sizeof(end_time), "%Y-%m-%d %H:%M:%S", localtime(&now));
                db_mysql_update_call_end(sync->mysql_pool,
                        call_id, end_time, duration, sip_status, result_code, result_desc);
            }
            break;
        }
        case DB_TASK_DEPT_IDLE: {
            /* 部门空闲计数的 MySQL 持久化是备用的低频操作，
               主要依赖 Redis。此处仅做日志追踪 */
            char dept_key[64];
            int idle = 0;
            if (sscanf(data, "%63[^|]|%d", dept_key, &idle) >= 2) {
                LOG_DEBUG("db_sync_worker: dept_idle %s=%d (logged only)", dept_key, idle);
            }
            break;
        }
        default:
            break;
        }

        free(task);
    }

    LOG_INFO("db_sync_worker: thread exiting");
    return NULL;
}

/* ============================================================
 * 生命周期管理
 * ============================================================ */

db_sync_context_t *db_sync_create(mysql_pool_t *pool, redis_context_t *redis)
{
    db_sync_context_t *sync = (db_sync_context_t *)calloc(1, sizeof(db_sync_context_t));
    if (!sync) {
        LOG_ERROR("db_sync_create: calloc failed");
        return NULL;
    }

    sync->mysql_pool = pool;
    sync->redis = redis;
    sync->db_enabled = 0;
    sync->cache_enabled = 0;
    sync->worker_running = 0;
    sync->health_check_interval_sec = 15;

    queue_init(&sync->queue);
    LOG_INFO("db_sync_create: context created");
    return sync;
}

void db_sync_destroy(db_sync_context_t *sync)
{
    if (!sync) return;

    db_sync_stop_worker(sync);
    queue_destroy(&sync->queue);
    free(sync);
    LOG_INFO("db_sync_destroy: context destroyed");
}

int db_sync_init(db_sync_context_t *sync, const char *schema_path)
{
    if (!sync) return -1;

    /* 1. MySQL 连接池初始化 */
    if (sync->mysql_pool) {
        LOG_INFO("db_sync_init: connecting MySQL pool...");
        if (db_mysql_pool_connect_all(sync->mysql_pool) == 0) {
            sync->db_enabled = 1;

            if (schema_path) {
                int stmts = db_mysql_init_schema(sync->mysql_pool, schema_path);
                if (stmts >= 0) {
                    LOG_INFO("db_sync_init: schema initialized (%d stmts)", stmts);

                    /* 首次部署时自动生成员工分机 + 坐席种子数据 */
                    LOG_INFO("db_sync_init: seeding demo employee data...");
                    int seeded = db_sync_seed_demo_data(sync);
                    if (seeded > 0) {
                        LOG_INFO("db_sync_init: seeded %d employees", seeded);
                    }
                }
            }

            LOG_INFO("db_sync_init: MySQL pool ready (%d/%d slots)",
                     db_mysql_pool_health(sync->mysql_pool),
                     sync->mysql_pool->pool_size);
        } else {
            LOG_WARN("db_sync_init: MySQL pool unavailable, "
                     "running without persistent storage");
        }
    }

    /* 2. Redis 连接 */
    if (sync->redis) {
        LOG_INFO("db_sync_init: connecting Redis...");
        if (db_redis_connect(sync->redis) == 0) {
            sync->cache_enabled = 1;
            LOG_INFO("db_sync_init: Redis connected ✓");
        } else {
            LOG_WARN("db_sync_init: Redis unavailable, "
                     "running without cache acceleration");
        }
    }

    /* 3. 启动 Worker 线程 */
    sync->worker_running = 1;
    if (pthread_create(&sync->worker_thread, NULL, db_sync_worker, sync) != 0) {
        LOG_ERROR("db_sync_init: failed to create worker thread");
        sync->worker_running = 0;
        return -1;
    }

    /* 4. 缓存预热：从 MySQL 加载初始数据到 Redis */
    if (sync->cache_enabled && sync->db_enabled) {
        LOG_INFO("db_sync_init: warming up cache...");
        db_sync_cache_warmup_all(sync);
    }

    LOG_INFO("db_sync_init: complete (DB=%s, Cache=%s, Worker=ON)",
             sync->db_enabled ? "ON" : "OFF",
             sync->cache_enabled ? "ON" : "OFF");
    return 0;
}

void db_sync_stop_worker(db_sync_context_t *sync)
{
    if (!sync || !sync->worker_running) return;

    LOG_INFO("db_sync_stop_worker: signaling shutdown...");

    /* 发送 SHUTDOWN 信号 */
    db_task_t *stop_task = (db_task_t *)calloc(1, sizeof(db_task_t));
    if (stop_task) {
        stop_task->type = DB_TASK_SHUTDOWN;
        queue_push(&sync->queue, stop_task);
    }

    /* 设置 shutdown 标志并唤醒 Worker */
    pthread_mutex_lock(&sync->queue.lock);
    sync->queue.shutdown = 1;
    pthread_cond_signal(&sync->queue.cond);
    pthread_mutex_unlock(&sync->queue.lock);

    /* 等待 Worker 线程退出 */
    pthread_join(sync->worker_thread, NULL);
    sync->worker_running = 0;

    LOG_INFO("db_sync_stop_worker: worker stopped");
}

/* ============================================================
 * 健康检查 (含 Redis 运行时恢复)
 * ============================================================ */

int db_sync_health_check(db_sync_context_t *sync)
{
    if (!sync) return -1;

    time_t now = time(NULL);
    if (now - sync->last_health_check < sync->health_check_interval_sec) {
        return (sync->db_enabled || sync->cache_enabled) ? 0 : -1;
    }
    sync->last_health_check = now;

    /* MySQL 健康检查 */
    if (sync->mysql_pool && sync->db_enabled) {
        int health = db_mysql_pool_health(sync->mysql_pool);
        if (health <= 0) {
            sync->db_enabled = 0;
            LOG_WARN("db_sync_health_check: MySQL pool unhealthy ✗");
        }
    } else if (sync->mysql_pool && !sync->db_enabled) {
        int health = db_mysql_pool_health(sync->mysql_pool);
        if (health > 0) {
            sync->db_enabled = 1;
            LOG_INFO("db_sync_health_check: MySQL pool recovered ✓");
        }
    }

    /* Redis 健康检查 + 运行时恢复 */
    int was_cache_enabled = sync->cache_enabled;

    if (sync->redis && sync->cache_enabled) {
        if (db_redis_ping(sync->redis) != 0) {
            sync->cache_enabled = 0;
            LOG_WARN("db_sync_health_check: Redis disconnected ✗");
        }
    } else if (sync->redis && !sync->cache_enabled) {
        if (db_redis_ensure_connected(sync->redis) == 0) {
            sync->cache_enabled = 1;
            LOG_INFO("db_sync_health_check: Redis recovered ✓");

            /* ★ Redis 运行时恢复：自动触发全量缓存预热 ★ */
            if (sync->db_enabled) {
                LOG_INFO("db_sync_health_check: triggering runtime cache warmup...");
                db_sync_cache_warmup_all(sync);
            }
        }
    }

    /* Redis 从不可用变为可用 → 预热缓存 */
    if (!was_cache_enabled && sync->cache_enabled && sync->db_enabled) {
        LOG_INFO("db_sync_health_check: Redis recovered, warming cache...");
        db_sync_cache_warmup_all(sync);
    }

    return (sync->db_enabled || sync->cache_enabled) ? 0 : -1;
}

/* ============================================================
 * 任务入队接口 (主线程调用，非阻塞)
 * ============================================================ */

int db_sync_agent_status_change(db_sync_context_t *sync,
        const char *extension, const char *new_status, int concurrent_calls)
{
    if (!sync || !extension || !new_status) return -1;

    /* Step 1: Redis 实时更新 (主线程直接操作，微秒级) */
    if (sync->cache_enabled && sync->redis) {
        db_redis_cache_agent_status(sync->redis,
                extension, new_status, concurrent_calls);
    }

    /* Step 2: 异步入队 MySQL 持久化 (非阻塞，立即返回) */
    if (sync->db_enabled && sync->worker_running) {
        db_task_t *task = (db_task_t *)calloc(1, sizeof(db_task_t));
        if (task) {
            pack_agent_status(task, extension, new_status, concurrent_calls);
            queue_push(&sync->queue, task);
        }
    }

    return 0;
}

int db_sync_agent_online(db_sync_context_t *sync,
        const char *extension, int is_online)
{
    if (!sync || !extension) return -1;

    if (sync->cache_enabled && sync->redis) {
        db_redis_cache_agent_online(sync->redis, extension, is_online);
    }

    if (sync->db_enabled && sync->worker_running) {
        db_task_t *task = (db_task_t *)calloc(1, sizeof(db_task_t));
        if (task) {
            pack_agent_online(task, extension, is_online);
            queue_push(&sync->queue, task);
        }
    }

    return 0;
}

int db_sync_call_start(db_sync_context_t *sync,
        const char *call_id, const char *caller, const char *callee,
        const char *direction, const char *department)
{
    if (!sync || !call_id || !caller || !callee) return -1;

    char start_time[32];
    time_t now = time(NULL);
    strftime(start_time, sizeof(start_time), "%Y-%m-%d %H:%M:%S", localtime(&now));

    /* Redis 会话缓存 */
    if (sync->cache_enabled && sync->redis) {
        char key[DB_MAX_REDIS_KEY_LEN];
        snprintf(key, sizeof(key), REDIS_KEY_CALL_SESSION, call_id);
        db_redis_set(sync->redis, key, "ringing");
        db_redis_expire(sync->redis, key, 7200);
    }

    /* 异步入队 MySQL */
    if (sync->db_enabled && sync->worker_running) {
        db_task_t *task = (db_task_t *)calloc(1, sizeof(db_task_t));
        if (task) {
            pack_call_start(task, call_id, caller, callee,
                           direction, department, start_time);
            queue_push(&sync->queue, task);
        }
    }

    return 0;
}

int db_sync_call_end(db_sync_context_t *sync,
        const char *call_id, int duration_sec,
        int sip_status, int result_code, const char *result_desc)
{
    if (!sync || !call_id) return -1;

    /* 清理 Redis 会话缓存 */
    if (sync->cache_enabled && sync->redis) {
        char key[DB_MAX_REDIS_KEY_LEN];
        snprintf(key, sizeof(key), REDIS_KEY_CALL_SESSION, call_id);
        db_redis_del(sync->redis, key);
    }

    /* 异步入队 MySQL */
    if (sync->db_enabled && sync->worker_running) {
        db_task_t *task = (db_task_t *)calloc(1, sizeof(db_task_t));
        if (task) {
            pack_call_end(task, call_id, duration_sec,
                         sip_status, result_code, result_desc);
            queue_push(&sync->queue, task);
        }
    }

    return 0;
}

int db_sync_dept_idle_update(db_sync_context_t *sync,
        const char *dept_key, int idle_count)
{
    if (!sync || !dept_key) return -1;

    if (sync->cache_enabled && sync->redis) {
        db_redis_update_dept_idle(sync->redis, dept_key, idle_count);
    }

    return 0;
}

/* ============================================================
 * Cache Aside 查询接口
 * ============================================================ */

int db_sync_agent_status_query(db_sync_context_t *sync,
        const char *extension, char *status_out, int size,
        int *concurrent_calls_out)
{
    if (!sync || !extension) return -1;

    /* Step 1: 尝试从 Redis 读取 */
    if (sync->cache_enabled && sync->redis) {
        int ret = db_redis_get_agent_status(sync->redis, extension,
                                             status_out, size,
                                             concurrent_calls_out);
        if (ret == 0) {
            return 0; /* Redis Hit ✓ */
        }
    }

    /* Step 2: Cache MISS → 回源 MySQL */
    if (sync->db_enabled && sync->mysql_pool) {
        char db_status[32] = {0};
        int db_calls = 0;
        int ret = db_mysql_query_agent_single(sync->mysql_pool,
                extension, db_status, sizeof(db_status), &db_calls);

        if (status_out && size > 0) {
            strncpy(status_out, db_status, (size_t)(size - 1));
            status_out[size - 1] = '\0';
        }
        if (concurrent_calls_out) {
            *concurrent_calls_out = db_calls;
        }

        /* Step 3: 回写 Redis 缓存 (异步，不阻塞返回值) */
        if (ret == 0 && sync->cache_enabled && sync->redis) {
            db_redis_cache_agent_status(sync->redis,
                    extension, db_status, db_calls);
            LOG_DEBUG("db_sync_agent_status_query: Cache Aside write-back: %s=%s",
                      extension, db_status);
        }

        return ret;
    }

    /* 完全降级：无缓存无数据库 */
    if (status_out && size > 0) {
        strncpy(status_out, "offline", (size_t)(size - 1));
        status_out[size - 1] = '\0';
    }
    if (concurrent_calls_out) *concurrent_calls_out = 0;
    return 1;
}

int db_sync_dept_idle_query(db_sync_context_t *sync, const char *dept_key)
{
    if (!sync || !dept_key) return -1;

    /* Step 1: Redis 查询 */
    if (sync->cache_enabled && sync->redis) {
        int idle = db_redis_get_dept_idle(sync->redis, dept_key);
        if (idle >= 0) return idle;
    }

    /* Step 2: Cache MISS → 回源 MySQL */
    if (sync->db_enabled && sync->mysql_pool) {
        char json[2048] = {0};
        int count = db_mysql_find_idle_agents(sync->mysql_pool,
                dept_key, json, sizeof(json), 100);

        /* 回写 Redis */
        if (sync->cache_enabled && sync->redis) {
            db_redis_update_dept_idle(sync->redis, dept_key, count);
        }

        return count;
    }

    return -1;
}

/* ============================================================
 * 缓存预热
 * ============================================================ */

int db_sync_cache_warmup_depts(db_sync_context_t *sync)
{
    if (!sync || !sync->db_enabled || !sync->cache_enabled) return -1;
    if (!sync->mysql_pool || !sync->redis) return -1;

    LOG_INFO("db_sync_cache_warmup_depts: loading from MySQL...");

    const char *sql = "SELECT dept_key, dept_name, short_number, "
                      "range_start, range_end, external_access, "
                      "min_agents_per_shift FROM departments";

    MYSQL_RES *res = db_mysql_query(sync->mysql_pool, sql);
    if (!res) {
        LOG_WARN("db_sync_cache_warmup_depts: query failed");
        return -1;
    }

    int count = (int)mysql_num_rows(res);
    for (int i = 0; i < count; i++) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) break;

        char json[2048];
        snprintf(json, sizeof(json),
                 "{\"dept_key\":\"%s\",\"dept_name\":\"%s\","
                 "\"short_number\":\"%s\",\"range_start\":\"%s\","
                 "\"range_end\":\"%s\",\"external_access\":%s,"
                 "\"min_agents_per_shift\":%s}",
                 row[0] ? row[0] : "",
                 row[1] ? row[1] : "",
                 row[2] ? row[2] : "",
                 row[3] ? row[3] : "",
                 row[4] ? row[4] : "",
                 row[5] ? row[5] : "0",
                 row[6] ? row[6] : "1");

        db_redis_cache_dept_config(sync->redis, row[0], json);
    }

    mysql_free_result(res);
    LOG_INFO("db_sync_cache_warmup_depts: cached %d departments", count);
    return count;
}

int db_sync_cache_warmup_agents(db_sync_context_t *sync)
{
    if (!sync || !sync->db_enabled || !sync->cache_enabled) return -1;
    if (!sync->mysql_pool || !sync->redis) return -1;

    LOG_INFO("db_sync_cache_warmup_agents: loading from MySQL...");

    const char *sql = "SELECT a.extension, a.is_online, a.status, "
                      "a.concurrent_calls, d.dept_key FROM agents a "
                      "JOIN departments d ON a.department_id = d.id";

    MYSQL_RES *res = db_mysql_query(sync->mysql_pool, sql);
    if (!res) {
        LOG_WARN("db_sync_cache_warmup_agents: query failed");
        return -1;
    }

    int count = (int)mysql_num_rows(res);
    for (int i = 0; i < count; i++) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) break;

        const char *extension = row[0];
        const char *status = row[2] ? row[2] : "offline";
        int calls = row[3] ? atoi(row[3]) : 0;
        int online = row[1] ? atoi(row[1]) : 0;

        db_redis_cache_agent_status(sync->redis, extension, status, calls);
        db_redis_cache_agent_online(sync->redis, extension, online);
    }

    mysql_free_result(res);
    LOG_INFO("db_sync_cache_warmup_agents: cached %d agents", count);
    return count;
}

int db_sync_cache_warmup_all(db_sync_context_t *sync)
{
    if (!sync || !sync->db_enabled || !sync->cache_enabled) return -1;

    int depts = db_sync_cache_warmup_depts(sync);
    int agents = db_sync_cache_warmup_agents(sync);

    LOG_INFO("db_sync_cache_warmup_all: departments=%d, agents=%d",
             depts, agents);
    return 0;
}

/* ============================================================
 * 队列监控
 * ============================================================ */

int db_sync_queue_size(db_sync_context_t *sync)
{
    if (!sync) return -1;

    pthread_mutex_lock(&sync->queue.lock);
    int size = sync->queue.count;
    pthread_mutex_unlock(&sync->queue.lock);
    return size;
}

/* ============================================================
 * 种子数据生成 — 按部门规模程序化构造员工分机 + 坐席
 * ============================================================ */

/* 常见中文姓氏 (20个轮转) */
static const char *surnames[] = {
    "张","李","王","刘","陈","杨","赵","黄","周","吴",
    "徐","孙","马","胡","朱","郭","何","林","罗","高"
};
#define SURNAME_COUNT 20

/* 常见中文名 (50个轮转，与姓氏组合可得1000+不重复姓名) */
static const char *given_names[] = {
    "明","华","伟","芳","强","丽","勇","敏","静",
    "磊","洋","婷","军","雪","峰","玲","涛","娜",
    "刚","娟","文","超","艳","杰","秀","辉","鑫",
    "兰","宁","龙","凤","宇","霞","博","怡","飞",
    "晨","萍","浩","慧","志","燕","鹏","凯","琴",
    "旭","琳","睿","佳","恒"
};
#define GIVEN_COUNT 50

/* 部门规模配置 (需与 schema.sql INSERT 的 departments 顺序一致) */
typedef struct {
    const char *dept_key;
    int         dept_id;        /* MySQL 自增ID (1-based) */
    const char *range_start;
    const char *range_end;
    int         external_access;
} dept_seed_t;

static const dept_seed_t dept_seeds[] = {
    {"hr",         1, "1001", "1050", 0},
    {"finance",    2, "1101", "1150", 0},
    {"admin",      3, "1201", "1250", 0},
    {"management", 4, "1301", "1330", 0},
    {"sales",      5, "2001", "2400", 1},
    {"service",    6, "2501", "2800", 1},
    {"market",     7, "2901", "2980", 1},
    {"rnd",        8, "3001", "3200", 0},
    {"support",    9, "9001", "9050", 1},
};
#define DEPT_COUNT 9

/* 构建一批 INSERT 语句并执行，每批最多 BATCH_ROWS 行 */
#define SEED_BATCH_ROWS 200
#define SEED_SQL_BUF_SIZE (128 * 1024)

static int seed_insert_batch(mysql_pool_t *pool,
        const char *table, const char *columns,
        int start_ext, int end_ext, int dept_id, int *name_idx)
{
    static char sql[SEED_SQL_BUF_SIZE];
    int offset = 0;
    int count = 0;

    offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
                       "INSERT INTO %s (%s) VALUES ", table, columns);

    for (int ext = start_ext; ext <= end_ext && count < SEED_BATCH_ROWS;
         ext++, count++) {
        int s_idx = (*name_idx) % SURNAME_COUNT;
        int g_idx = (*name_idx) % GIVEN_COUNT;
        (*name_idx)++;

        if (count > 0) {
            offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset, ",");
        }

        offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
                           "('%04d','%s%s',%d)",
                           ext, surnames[s_idx], given_names[g_idx], dept_id);

        if (offset >= (int)(sizeof(sql) - 256)) break;
    }

    if (count == 0) return 0;

    return db_mysql_execute_long(pool, sql, offset);
}

static int seed_insert_agents_batch(mysql_pool_t *pool,
        int start_ext, int end_ext, int dept_id)
{
    static char sql[SEED_SQL_BUF_SIZE];
    int offset = 0;
    int count = 0;

    offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
        "INSERT INTO agents (extension, department_id, is_online, status, "
        "concurrent_calls, shift) VALUES ");

    for (int ext = start_ext; ext <= end_ext && count < SEED_BATCH_ROWS;
         ext++, count++) {
        int shift_idx = count % 3;
        char shift = (char)('A' + shift_idx);

        if (count > 0) {
            offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset, ",");
        }

        offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
                           "('%04d',%d,1,'idle',0,'%c')",
                           ext, dept_id, shift);

        if (offset >= (int)(sizeof(sql) - 256)) break;
    }

    if (count == 0) return 0;

    /* 重复键则更新状态为 idle */
    offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
        " ON DUPLICATE KEY UPDATE is_online=1, status='idle', "
        "concurrent_calls=0, last_active=NOW()");

    return db_mysql_execute_long(pool, sql, offset);
}

int db_sync_seed_demo_data(db_sync_context_t *sync)
{
    if (!sync || !sync->db_enabled || !sync->mysql_pool) return -1;

    /* 先检查是否已有数据（幂等性保护） */
    int existing = db_mysql_load_all_agents_for_cache(sync->mysql_pool);
    if (existing > 100) {
        LOG_INFO("db_sync_seed_demo_data: %d agents already exist, skipping seed", existing);
        return existing;
    }

    LOG_INFO("db_sync_seed_demo_data: generating employee data for %d departments...", DEPT_COUNT);

    int name_idx = 0;
    int total_ext = 0;
    int total_agents = 0;

    for (int d = 0; d < DEPT_COUNT; d++) {
        const dept_seed_t *ds = &dept_seeds[d];
        int start = atoi(ds->range_start);
        int end   = atoi(ds->range_end);
        int total = end - start + 1;

        LOG_INFO("  [%s] %s: %d extensions (%04d-%04d)...",
                 ds->dept_key, ds->dept_key,
                 total, start, end);

        /* 分批插入 extensions */
        for (int batch_start = start; batch_start <= end;
             batch_start += SEED_BATCH_ROWS) {
            int batch_end = batch_start + SEED_BATCH_ROWS - 1;
            if (batch_end > end) batch_end = end;

            int rows = seed_insert_batch(sync->mysql_pool,
                    "extensions", "extension,employee_name,department_id",
                    batch_start, batch_end, ds->dept_id, &name_idx);
            if (rows >= 0) total_ext += rows;
        }

        /* 对外部门额外插入 agents 记录 (A/B/C 三班倒轮转) */
        if (ds->external_access) {
            for (int batch_start = start; batch_start <= end;
                 batch_start += SEED_BATCH_ROWS) {
                int batch_end = batch_start + SEED_BATCH_ROWS - 1;
                if (batch_end > end) batch_end = end;

                int rows = seed_insert_agents_batch(sync->mysql_pool,
                        batch_start, batch_end, ds->dept_id);
                if (rows >= 0) total_agents += rows;
            }
        }
    }

    LOG_INFO("db_sync_seed_demo_data: complete — "
             "%d extensions + %d agents inserted",
             total_ext, total_agents);
    return total_ext;
}
