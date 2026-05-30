/*
 * db_redis.c — Redis 缓存操作实现
 * ============================================================
 * 基于 hiredis C API 封装，提供连接管理、Key/Value、Hash、
 * Sorted Set 等操作能力，适配呼叫中心高频状态缓存场景。
 */

#include "db_redis.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 生命周期管理
 * ============================================================ */

redis_context_t *db_redis_create(const redis_config_t *config)
{
    redis_context_t *ctx = (redis_context_t *)calloc(1, sizeof(redis_context_t));
    if (!ctx) {
        LOG_ERROR("db_redis_create: failed to allocate context");
        return NULL;
    }

    ctx->config = *config;
    LOG_INFO("db_redis_create: context created for %s:%u db=%d",
             config->host, config->port, config->db_index);
    return ctx;
}

void db_redis_destroy(redis_context_t *ctx)
{
    if (!ctx) return;
    db_redis_disconnect(ctx);
    free(ctx);
    LOG_INFO("db_redis_destroy: context destroyed");
}

int db_redis_connect(redis_context_t *ctx)
{
    if (!ctx) return -1;

    struct timeval tv;
    tv.tv_sec = ctx->config.connect_timeout > 0 ? ctx->config.connect_timeout : 3;
    tv.tv_usec = 0;

    ctx->ctx = redisConnectWithTimeout(ctx->config.host, (int)ctx->config.port, tv);
    if (!ctx->ctx || ctx->ctx->err) {
        LOG_ERROR("db_redis_connect: failed to connect to %s:%u - %s",
                  ctx->config.host, ctx->config.port,
                  ctx->ctx ? ctx->ctx->errstr : "unknown error");
        if (ctx->ctx) {
            redisFree(ctx->ctx);
            ctx->ctx = NULL;
        }
        ctx->connected = 0;
        return -1;
    }

    redisSetTimeout(ctx->ctx, tv);

    if (strlen(ctx->config.password) > 0) {
        redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
                "AUTH %s", ctx->config.password);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            LOG_ERROR("db_redis_connect: AUTH failed - %s",
                      reply ? reply->str : "no reply");
            if (reply) freeReplyObject(reply);
            redisFree(ctx->ctx);
            ctx->ctx = NULL;
            ctx->connected = 0;
            return -1;
        }
        freeReplyObject(reply);
    }

    if (ctx->config.db_index > 0) {
        redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
                "SELECT %d", ctx->config.db_index);
        if (reply) freeReplyObject(reply);
    }

    ctx->connected = 1;
    ctx->retry_count = 0;
    LOG_INFO("db_redis_connect: connected to %s:%u db=%d",
             ctx->config.host, ctx->config.port, ctx->config.db_index);
    return 0;
}

void db_redis_disconnect(redis_context_t *ctx)
{
    if (!ctx) return;
    if (ctx->ctx) {
        redisFree(ctx->ctx);
    }
    ctx->ctx = NULL;
    ctx->connected = 0;
}

int db_redis_reconnect(redis_context_t *ctx)
{
    if (!ctx) return -1;

    time_t now = time(NULL);
    if (now - ctx->last_reconnect_attempt < DB_RECONNECT_BASE_SEC) {
        return -1;
    }
    ctx->last_reconnect_attempt = now;

    if (ctx->ctx) {
        redisFree(ctx->ctx);
        ctx->ctx = NULL;
    }

    ctx->retry_count++;
    int ret = db_redis_connect(ctx);
    if (ret == 0) {
        LOG_INFO("db_redis_reconnect: reconnected successfully (attempt %d)",
                 ctx->retry_count);
        ctx->retry_count = 0;
    } else {
        LOG_WARN("db_redis_reconnect: reconnect failed (attempt %d/%d)",
                 ctx->retry_count, DB_MAX_RETRY_COUNT);
    }
    return ret;
}

int db_redis_ping(redis_context_t *ctx)
{
    if (!ctx || !ctx->ctx || !ctx->connected) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx, "PING");
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        LOG_WARN("db_redis_ping: connection lost");
        ctx->connected = 0;
        if (reply) freeReplyObject(reply);
        return -1;
    }
    freeReplyObject(reply);
    return 0;
}

int db_redis_ensure_connected(redis_context_t *ctx)
{
    if (!ctx) return -1;

    if (ctx->connected && ctx->ctx && db_redis_ping(ctx) == 0) {
        return 0;
    }

    return db_redis_reconnect(ctx);
}

/* ============================================================
 * 通用 Key/Value 操作
 * ============================================================ */

int db_redis_set(redis_context_t *ctx, const char *key, const char *value)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "SET %s %s", key, value);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        LOG_WARN("db_redis_set: SET %s failed - %s",
                 key, reply ? reply->str : "no reply");
        if (reply) freeReplyObject(reply);
        return -1;
    }
    freeReplyObject(reply);
    return 0;
}

int db_redis_get(redis_context_t *ctx, const char *key, char *out, int out_size)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "GET %s", key);
    if (!reply) return -1;

    int ret = -1;
    if (reply->type == REDIS_REPLY_STRING && out && out_size > 0) {
        strncpy(out, reply->str, (size_t)(out_size - 1));
        out[out_size - 1] = '\0';
        ret = 0;
    }
    freeReplyObject(reply);
    return ret;
}

int db_redis_del(redis_context_t *ctx, const char *key)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "DEL %s", key);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_exists(redis_context_t *ctx, const char *key)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "EXISTS %s", key);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_expire(redis_context_t *ctx, const char *key, int seconds)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "EXPIRE %s %d", key, seconds);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_incr(redis_context_t *ctx, const char *key)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "INCR %s", key);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_decr(redis_context_t *ctx, const char *key)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "DECR %s", key);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

/* ============================================================
 * Hash 操作 (坐席状态缓存核心)
 * ============================================================ */

int db_redis_hset(redis_context_t *ctx, const char *key,
        const char *field, const char *value)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "HSET %s %s %s", key, field, value);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        LOG_WARN("db_redis_hset: HSET %s %s failed - %s",
                 key, field, reply ? reply->str : "no reply");
        if (reply) freeReplyObject(reply);
        return -1;
    }
    freeReplyObject(reply);
    return 0;
}

int db_redis_hget(redis_context_t *ctx, const char *key,
        const char *field, char *out, int out_size)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "HGET %s %s", key, field);
    if (!reply) return -1;

    int ret = -1;
    if (reply->type == REDIS_REPLY_STRING && out && out_size > 0) {
        strncpy(out, reply->str, (size_t)(out_size - 1));
        out[out_size - 1] = '\0';
        ret = 0;
    }
    freeReplyObject(reply);
    return ret;
}

int db_redis_hgetall(redis_context_t *ctx, const char *key,
        char *result_json, int result_size)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "HGETALL %s", key);
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return -1;
    }

    int offset = 0;
    offset += snprintf(result_json + offset, (size_t)(result_size - offset), "{");

    for (size_t i = 0; i + 1 < reply->elements; i += 2) {
        if (i > 0) {
            offset += snprintf(result_json + offset,
                               (size_t)(result_size - offset), ",");
        }
        offset += snprintf(result_json + offset, (size_t)(result_size - offset),
                           "\"%s\":\"%s\"",
                           reply->element[i]->str,
                           reply->element[i + 1]->str);
    }

    snprintf(result_json + offset, (size_t)(result_size - offset), "}");
    freeReplyObject(reply);
    return 0;
}

int db_redis_hdel(redis_context_t *ctx, const char *key, const char *field)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "HDEL %s %s", key, field);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_hmset(redis_context_t *ctx, const char *key,
        const char *fields[], const char *values[], int count)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;
    if (count <= 0) return 0;

    int argc = 3 + count * 2;
    const char **argv = (const char **)malloc((size_t)argc * sizeof(char *));
    size_t *argvlen = (size_t *)malloc((size_t)argc * sizeof(size_t));

    argv[0] = "HMSET";
    argvlen[0] = 5;
    argv[1] = key;
    argvlen[1] = strlen(key);

    for (int i = 0; i < count; i++) {
        argv[2 + i * 2] = fields[i];
        argvlen[2 + i * 2] = strlen(fields[i]);
        argv[2 + i * 2 + 1] = values[i];
        argvlen[2 + i * 2 + 1] = strlen(values[i]);
    }

    redisReply *reply = (redisReply *)redisCommandArgv(ctx->ctx, argc, argv, argvlen);
    free(argv);
    free(argvlen);

    if (!reply) return -1;
    int ok = (reply->type != REDIS_REPLY_ERROR) ? 0 : -1;
    freeReplyObject(reply);
    return ok;
}

/* ============================================================
 * 有序集合操作 (排队队列)
 * ============================================================ */

int db_redis_zadd(redis_context_t *ctx, const char *key,
        double score, const char *member)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "ZADD %s %.6f %s", key, score, member);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_zrem(redis_context_t *ctx, const char *key, const char *member)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "ZREM %s %s", key, member);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_zcard(redis_context_t *ctx, const char *key)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "ZCARD %s", key);
    if (!reply) return -1;
    int ret = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : -1;
    freeReplyObject(reply);
    return ret;
}

int db_redis_zpopmin(redis_context_t *ctx, const char *key,
        char *member_out, int member_size)
{
    if (db_redis_ensure_connected(ctx) != 0) return -1;

    redisReply *reply = (redisReply *)redisCommand(ctx->ctx,
            "ZPOPMIN %s", key);
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
        if (reply) freeReplyObject(reply);
        return -1;
    }

    if (member_out && member_size > 0) {
        strncpy(member_out, reply->element[0]->str, (size_t)(member_size - 1));
        member_out[member_size - 1] = '\0';
    }
    int ret = 0;
    freeReplyObject(reply);
    return ret;
}

/* ============================================================
 * 业务专用缓存接口 — 坐席状态
 * ============================================================ */

int db_redis_cache_agent_status(redis_context_t *ctx,
        const char *extension, const char *status, int concurrent_calls)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_AGENT_STATUS, extension);

    char calls_str[16];
    snprintf(calls_str, sizeof(calls_str), "%d", concurrent_calls);

    const char *fields[] = {"extension", "status", "concurrent_calls", "updated_at"};
    const char *values[4];
    values[0] = extension;
    values[1] = status;
    values[2] = calls_str;

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%ld", (long)time(NULL));
    values[3] = time_str;

    return db_redis_hmset(ctx, key, fields, values, 4);
}

int db_redis_get_agent_status(redis_context_t *ctx,
        const char *extension, char *status_out, int size,
        int *concurrent_calls_out)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_AGENT_STATUS, extension);

    char status[32] = {0};
    int ret = db_redis_hget(ctx, key, "status", status, sizeof(status));
    if (ret != 0) return -1;

    if (status_out) {
        strncpy(status_out, status, (size_t)(size - 1));
        status_out[size - 1] = '\0';
    }

    if (concurrent_calls_out) {
        char calls[16] = {0};
        if (db_redis_hget(ctx, key, "concurrent_calls", calls, sizeof(calls)) == 0) {
            *concurrent_calls_out = atoi(calls);
        } else {
            *concurrent_calls_out = 0;
        }
    }

    return 0;
}

int db_redis_cache_agent_online(redis_context_t *ctx,
        const char *extension, int is_online)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_AGENT_ONLINE, extension);

    char value[8];
    snprintf(value, sizeof(value), "%d", is_online);
    return db_redis_set(ctx, key, value);
}

/* ============================================================
 * 业务专用缓存接口 — 部门
 * ============================================================ */

int db_redis_update_dept_idle(redis_context_t *ctx,
        const char *dept_key, int idle_count)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_DEPT_IDLE, dept_key);

    char value[16];
    snprintf(value, sizeof(value), "%d", idle_count);
    return db_redis_set(ctx, key, value);
}

int db_redis_get_dept_idle(redis_context_t *ctx, const char *dept_key)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_DEPT_IDLE, dept_key);

    char value[16] = {0};
    if (db_redis_get(ctx, key, value, sizeof(value)) != 0) {
        return -1;
    }
    return atoi(value);
}

int db_redis_cache_dept_config(redis_context_t *ctx,
        const char *dept_key, const char *config_json)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_DEPT_CONFIG, dept_key);
    return db_redis_set(ctx, key, config_json);
}

int db_redis_get_dept_config(redis_context_t *ctx,
        const char *dept_key, char *config_out, int size)
{
    char key[DB_MAX_REDIS_KEY_LEN];
    snprintf(key, sizeof(key), REDIS_KEY_DEPT_CONFIG, dept_key);
    return db_redis_get(ctx, key, config_out, size);
}

/* ============================================================
 * 业务专用缓存接口 — 系统状态
 * ============================================================ */

int db_redis_cache_system_status(redis_context_t *ctx,
        const char *status_json)
{
    return db_redis_set(ctx, REDIS_KEY_SYSTEM_STATUS, status_json);
}

int db_redis_get_system_status(redis_context_t *ctx,
        char *status_out, int size)
{
    return db_redis_get(ctx, REDIS_KEY_SYSTEM_STATUS, status_out, size);
}
