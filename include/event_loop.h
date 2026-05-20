/*
 * event_loop.h — epoll 事件循环与连接管理框架
 * ============================================================
 * 基于 epoll 的高并发事件驱动服务端框架。封装了：
 *   - 服务端配置（端口、超时、连接上限）
 *   - 连接生命周期管理（创建/激活/超时/关闭）
 *   - 事件分发回调（accept/read/write/error/close）
 *   - 主事件循环（epoll_wait + 空闲检测）
 *   - 发送缓冲区管理（EPOLLOUT 按需注册/取消）
 *
 * 为 SIP 信令、RTP 媒体流等业务模块提供统一的 IO 事件接口。
 *
 * 依赖：epoll_socket.h, common_types.h, logger.h
 */

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <time.h>
#include <stdint.h>
#include <netinet/in.h>
#include "common_types.h"

/* 接收缓冲区大小（UDP 最大数据报 65535 + 头） */
#define EL_RECV_BUF_SIZE  65536

/* 发送缓冲区大小 */
#define EL_SEND_BUF_SIZE  65536

/* 发送结果码 */
#define EL_SEND_OK          0
#define EL_SEND_ERR_FULL   (-1)
#define EL_SEND_ERR_PARAM  (-2)

/* ============================================================
 * 连接类型枚举
 * ============================================================ */
typedef enum {
    CONN_TYPE_FREE       = 0,
    CONN_TYPE_TCP_LISTEN,
    CONN_TYPE_TCP_CLIENT,
    CONN_TYPE_UDP_SIP,
    CONN_TYPE_UDP_RTP,
} conn_type_t;

/* ============================================================
 * 连接状态枚举
 * ============================================================ */
typedef enum {
    CONN_STATE_ACTIVE   = 0,
    CONN_STATE_CLOSING,
    CONN_STATE_ERROR,
} conn_state_t;

/* ============================================================
 * 事件类型枚举
 * ============================================================ */
typedef enum {
    EL_EVENT_READ       = (1 << 0),
    EL_EVENT_WRITE      = (1 << 1),
    EL_EVENT_ERROR      = (1 << 2),
    EL_EVENT_HANGUP     = (1 << 3),
    EL_EVENT_TIMEOUT    = (1 << 4),
} el_event_type_t;

/* ============================================================
 * 连接结构体
 * ============================================================ */
typedef struct {
    int fd;
    conn_type_t  type;
    conn_state_t state;
    time_t create_time;
    time_t last_active;
    void *user_data;
    char remote_addr[64];
    uint16_t remote_port;

    char send_buf[EL_SEND_BUF_SIZE];
    int  send_len;
    int  send_offset;
} connection_t;

/* ============================================================
 * 前向声明
 * ============================================================ */
typedef struct event_loop_s event_loop_t;

/* ============================================================
 * 事件回调函数类型
 * ============================================================ */

typedef void (*el_on_accept_fn)(event_loop_t *el, int listen_fd,
                                int client_fd,
                                const struct sockaddr_in *client_addr);

typedef void (*el_on_read_fn)(event_loop_t *el, connection_t *conn);

typedef void (*el_on_write_fn)(event_loop_t *el, connection_t *conn);

typedef void (*el_on_error_fn)(event_loop_t *el, connection_t *conn,
                               int error_code);

typedef void (*el_on_close_fn)(event_loop_t *el, connection_t *conn);

typedef void (*el_on_idle_fn)(event_loop_t *el);

/* ============================================================
 * 事件回调集合
 * ============================================================ */
typedef struct {
    el_on_accept_fn  on_accept;
    el_on_read_fn    on_read;
    el_on_write_fn   on_write;
    el_on_error_fn   on_error;
    el_on_close_fn   on_close;
    el_on_idle_fn    on_idle;
} el_callbacks_t;

/* ============================================================
 * 服务端配置
 * ============================================================ */
typedef struct {
    uint16_t sip_port;
    uint16_t rtp_port_min;
    uint16_t rtp_port_max;
    int      tcp_backlog;
    int      max_connections;
    int      idle_timeout_sec;
    int      epoll_timeout_ms;
    int      epoll_max_events;
} server_config_t;

/* ============================================================
 * 事件循环主结构体
 * ============================================================ */
struct event_loop_s {
    int             epfd;
    volatile int    running;
    server_config_t config;
    el_callbacks_t  callbacks;

    connection_t   *connections;
    int             conn_capacity;
    int             active_count;

    int             sip_listen_fd;
    int             rtp_base_fd;

    time_t          last_idle_check;
    time_t          start_time;

    void           *user_data;
};

/* ============================================================
 * 事件循环 API
 * ============================================================ */

event_loop_t *el_create(const server_config_t *config);

int el_run(event_loop_t *el);

void el_stop(event_loop_t *el);

void el_destroy(event_loop_t *el);

/* ============================================================
 * 连接管理 API
 * ============================================================ */

connection_t *el_add_connection(event_loop_t *el, int fd, conn_type_t type,
                                uint32_t events,
                                const char *remote_addr, uint16_t remote_port);

void el_remove_connection(event_loop_t *el, connection_t *conn);

connection_t *el_find_connection(event_loop_t *el, int fd);

void el_touch_connection(connection_t *conn);

void el_set_callbacks(event_loop_t *el, const el_callbacks_t *cb);

int el_get_sip_fd(const event_loop_t *el);

/* ============================================================
 * 发送 API（EPOLLOUT 按需注册）
 * ============================================================ */

/*
 * 向连接写入数据
 * 先尝试立即发送，写不完则暂存剩余数据并注册 EPOLLOUT。
 * EPOLLOUT 触发后 on_write 回调会继续发送，发完自动取消 EPOLLOUT。
 *
 * @param el    事件循环实例
 * @param conn  目标连接
 * @param data  数据指针
 * @param len   数据长度
 * @param is_udp  1=UDP 模式（使用 sendto），0=TCP 模式（使用 send）
 * @param dest_addr  仅 UDP 模式下使用，目标地址（TCP 传 NULL）
 * @param dest_len   仅 UDP 模式下使用，目标地址长度（TCP 传 0）
 * @return EL_SEND_OK / EL_SEND_ERR_*
 */
int el_send_data(event_loop_t *el, connection_t *conn,
                 const char *data, int len,
                 int is_udp,
                 const struct sockaddr_in *dest_addr, socklen_t dest_len);

#endif /* EVENT_LOOP_H */
