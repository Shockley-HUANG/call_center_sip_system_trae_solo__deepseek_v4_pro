/*
 * db_redis.h — Redis 缓存操作接口
 * ============================================================
 * 提供 Redis 连接管理、Hash/Key/Set 操作等核心缓存能力。
 * 基于 hiredis C API 封装，适配呼叫中心高频状态缓存场景。
 */

#ifndef DB_REDIS_H
#define DB_REDIS_H

#include "db_config.h"
#include "common_types.h"

#include <hiredis/hiredis.h>

/* Redis Key 前缀约定 */
#define REDIS_KEY_AGENT_STATUS    "agent:status:%s"     /* 坐席状态 */
#define REDIS_KEY_AGENT_ONLINE    "agent:online:%s"     /* 坐席在线 */
#define REDIS_KEY_DEPT_CONFIG     "dept:config:%s"      /* 部门配置 */
#define REDIS_KEY_DEPT_IDLE       "dept:idle:%s"        /* 部门空闲坐席数 */
#define REDIS_KEY_DEPT_QUEUE      "dept:queue:%s"       /* 部门排队队列 */
#define REDIS_KEY_CALL_SESSION    "call:session:%s"     /* 通话会话 */
#define REDIS_KEY_IVR_CONFIG      "config:ivr"          /* IVR 配置 */
#define REDIS_KEY_ROUTE_CONFIG    "config:route"        /* 路由配置 */
#define REDIS_KEY_SYSTEM_STATUS   "system:status"       /* 系统状态 */

/* ============================================================
 * Redis 上下文结构体
 * ============================================================ */
typedef struct {
    redisContext    *ctx;
    redis_config_t   config;
    int              connected;
    int              retry_count;
    time_t           last_reconnect_attempt;
} redis_context_t;

/* ============================================================
 * 生命周期管理
 * ============================================================ */

redis_context_t *db_redis_create(const redis_config_t *config);
void             db_redis_destroy(redis_context_t *ctx);
int              db_redis_connect(redis_context_t *ctx);
void             db_redis_disconnect(redis_context_t *ctx);
int              db_redis_reconnect(redis_context_t *ctx);
int              db_redis_ping(redis_context_t *ctx);
int              db_redis_ensure_connected(redis_context_t *ctx);

/* ============================================================
 * 通用 Key/Value 操作
 * ============================================================ */

int db_redis_set(redis_context_t *ctx, const char *key, const char *value);
int db_redis_get(redis_context_t *ctx, const char *key, char *out, int out_size);
int db_redis_del(redis_context_t *ctx, const char *key);
int db_redis_exists(redis_context_t *ctx, const char *key);
int db_redis_expire(redis_context_t *ctx, const char *key, int seconds);
int db_redis_incr(redis_context_t *ctx, const char *key);
int db_redis_decr(redis_context_t *ctx, const char *key);

/* ============================================================
 * Hash 操作 (坐席状态缓存核心)
 * ============================================================ */

int db_redis_hset(redis_context_t *ctx, const char *key,
        const char *field, const char *value);

int db_redis_hget(redis_context_t *ctx, const char *key,
        const char *field, char *out, int out_size);

int db_redis_hgetall(redis_context_t *ctx, const char *key,
        char *result_json, int result_size);

int db_redis_hdel(redis_context_t *ctx, const char *key, const char *field);

int db_redis_hmset(redis_context_t *ctx, const char *key,
        const char *fields[], const char *values[], int count);

/* ============================================================
 * 有序集合操作 (排队队列)
 * ============================================================ */

int db_redis_zadd(redis_context_t *ctx, const char *key,
        double score, const char *member);

int db_redis_zrem(redis_context_t *ctx, const char *key, const char *member);

int db_redis_zcard(redis_context_t *ctx, const char *key);

int db_redis_zpopmin(redis_context_t *ctx, const char *key,
        char *member_out, int member_size);

/* ============================================================
 * 业务专用缓存接口
 * ============================================================ */

/* 坐席状态缓存 */
int db_redis_cache_agent_status(redis_context_t *ctx,
        const char *extension, const char *status, int concurrent_calls);

int db_redis_get_agent_status(redis_context_t *ctx,
        const char *extension, char *status_out, int size,
        int *concurrent_calls_out);

int db_redis_cache_agent_online(redis_context_t *ctx,
        const char *extension, int is_online);

/* 部门空闲计数 */
int db_redis_update_dept_idle(redis_context_t *ctx,
        const char *dept_key, int idle_count);

int db_redis_get_dept_idle(redis_context_t *ctx, const char *dept_key);

/* 部门路由配置缓存 */
int db_redis_cache_dept_config(redis_context_t *ctx,
        const char *dept_key, const char *config_json);

int db_redis_get_dept_config(redis_context_t *ctx,
        const char *dept_key, char *config_out, int size);

/* 系统状态 */
int db_redis_cache_system_status(redis_context_t *ctx,
        const char *status_json);
int db_redis_get_system_status(redis_context_t *ctx,
        char *status_out, int size);

#endif /* DB_REDIS_H */
