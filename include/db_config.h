/*
 * db_config.h — 数据库配置类型定义
 * ============================================================
 * 定义 MySQL/Redis 连接配置、连接池槽位、异步任务队列等
 * 基础数据结构。与 conf/sip_server.conf 中的配置段对应。
 *
 * 依赖：<stdint.h> (uint16_t), <pthread.h>
 */

#ifndef DB_CONFIG_H
#define DB_CONFIG_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* ============================================================
 * 缓冲区常量
 * ============================================================ */
#define DB_MAX_HOST_LEN       128
#define DB_MAX_USER_LEN       64
#define DB_MAX_PASS_LEN       64
#define DB_MAX_NAME_LEN       64
#define DB_MAX_SQL_LEN        4096
#define DB_MAX_REDIS_KEY_LEN  256
#define DB_MAX_REDIS_VAL_LEN  4096
#define DB_TASK_DATA_LEN      512
#define DB_QUEUE_MAX_SIZE     10000

/* ============================================================
 * 连接重试配置 (含重连风暴保护)
 * ============================================================ */
#define DB_MAX_RETRY_COUNT        5
#define DB_RECONNECT_BASE_SEC     1
#define DB_RECONNECT_MAX_SEC      30

/* ============================================================
 * MySQL 连接池配置
 * ============================================================ */
#define MYSQL_POOL_DEFAULT_SIZE   8
#define MYSQL_POOL_MAX_SIZE       32
#define MYSQL_POOL_BORROW_TIMEOUT_MS  5000

/* ============================================================
 * 配置结构体
 * ============================================================ */
typedef struct {
    char     host[DB_MAX_HOST_LEN];
    uint16_t port;
    char     user[DB_MAX_USER_LEN];
    char     password[DB_MAX_PASS_LEN];
    char     database[DB_MAX_NAME_LEN];
    int      pool_size;
    int      connect_timeout;
    int      read_timeout;
} mysql_config_t;

typedef struct {
    char     host[DB_MAX_HOST_LEN];
    uint16_t port;
    char     password[DB_MAX_PASS_LEN];
    int      db_index;
    int      pool_size;
    int      connect_timeout;
} redis_config_t;

/* ============================================================
 * 异步任务类型枚举
 * ============================================================ */
typedef enum {
    DB_TASK_AGENT_STATUS = 0,   /* 坐席状态变更 */
    DB_TASK_AGENT_ONLINE,       /* 坐席上下线 */
    DB_TASK_CALL_START,         /* 通话发起 */
    DB_TASK_CALL_END,           /* 通话结束 */
    DB_TASK_DEPT_IDLE,          /* 部门空闲更新 */
    DB_TASK_SHUTDOWN            /* Worker 线程关闭信号 */
} db_task_type_t;

/* ============================================================
 * 异步任务链表节点 (线程安全队列)
 * ============================================================ */
typedef struct db_task_s {
    db_task_type_t   type;
    char             data[DB_TASK_DATA_LEN];
    struct db_task_s *next;
} db_task_t;

/* ============================================================
 * 线程安全任务队列 (生产者-消费者)
 * ============================================================ */
typedef struct {
    db_task_t      *head;
    db_task_t      *tail;
    int             count;
    volatile int    shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} db_task_queue_t;

#endif /* DB_CONFIG_H */
