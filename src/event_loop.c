/*
 * event_loop.c — epoll 事件循环与连接管理框架实现
 * ============================================================
 * V4.0: ET 模式循环读 + EPOLLOUT 按需注册 + UDP SIP 集成 + el_default_on_read 公开接口
 */

#include "event_loop.h"
#include "epoll_socket.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>

static connection_t *alloc_connection(event_loop_t *el);
static void free_connection(connection_t *conn);
static void handle_accept(event_loop_t *el, connection_t *listen_conn);
static void handle_client_event(event_loop_t *el, connection_t *conn,
                                uint32_t events);
static void cleanup_idle_connections(event_loop_t *el);
static void default_on_accept(event_loop_t *el, int listen_fd,
                              int client_fd,
                              const struct sockaddr_in *client_addr);
static void default_on_write(event_loop_t *el, connection_t *conn);
static void default_on_error(event_loop_t *el, connection_t *conn,
                             int error_code);
static void default_on_close(event_loop_t *el, connection_t *conn);

/* 默认监听事件掩码（ET + 读 + 对端关闭 + 错误） */
#define DEFAULT_TCP_EVENTS (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP | EPOLLET)
#define DEFAULT_UDP_EVENTS (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLET)

/* ============================================================
 * 事件循环 API
 * ============================================================ */

event_loop_t *el_create(const server_config_t *config)
{
    event_loop_t *el;

    if (!config) {
        LOG_ERROR("el_create: config is NULL");
        return NULL;
    }

    el = (event_loop_t *)calloc(1, sizeof(event_loop_t));
    if (!el) {
        LOG_ERROR("el_create: calloc failed for event_loop_t");
        return NULL;
    }

    memcpy(&el->config, config, sizeof(server_config_t));

    if (el->config.max_connections <= 0) {
        el->config.max_connections = MAX_CONCURRENT_CALLS;
    }
    if (el->config.idle_timeout_sec <= 0) {
        el->config.idle_timeout_sec = CONNECTION_IDLE_TIMEOUT_SEC;
    }
    if (el->config.epoll_timeout_ms <= 0) {
        el->config.epoll_timeout_ms = EPOLL_WAIT_TIMEOUT_MS;
    }
    if (el->config.epoll_max_events <= 0) {
        el->config.epoll_max_events = MAX_EPOLL_EVENTS;
    }
    if (el->config.tcp_backlog <= 0) {
        el->config.tcp_backlog = SERVER_TCP_BACKLOG;
    }

    int epfd = es_create(el->config.epoll_max_events);
    if (epfd < 0) {
        LOG_ERROR("el_create: es_create failed: %d", epfd);
        free(el);
        return NULL;
    }
    el->epfd = epfd;

    el->conn_capacity = el->config.max_connections;
    el->connections = (connection_t *)calloc(
        (size_t)el->conn_capacity, sizeof(connection_t));
    if (!el->connections) {
        LOG_ERROR("el_create: calloc failed for connections array");
        es_close(el->epfd);
        free(el);
        return NULL;
    }

    for (int i = 0; i < el->conn_capacity; i++) {
        el->connections[i].fd   = -1;
        el->connections[i].type = CONN_TYPE_FREE;
    }

    el_callbacks_t defaults = {
        default_on_accept,
        el_default_on_read,
        default_on_write,
        default_on_error,
        default_on_close,
        NULL
    };
    el_set_callbacks(el, &defaults);

    if (el->config.sip_port > 0) {
        int fd;
        int ret = es_create_udp_server(el->config.sip_port, 1, &fd);
        if (ret != ES_OK) {
            LOG_WARN("el_create: failed to create SIP UDP socket on port %u: %d",
                     (unsigned int)el->config.sip_port, ret);
        } else {
            connection_t *conn = el_add_connection(el, fd, CONN_TYPE_UDP_SIP,
                                                   DEFAULT_UDP_EVENTS, NULL, 0);
            if (!conn) {
                LOG_WARN("el_create: failed to register SIP socket");
                es_close_socket(fd);
            } else {
                el->sip_listen_fd = fd;
                LOG_INFO("SIP UDP server listening on port %u (ET mode)",
                         (unsigned int)el->config.sip_port);
            }
        }
    }

    el->start_time     = time(NULL);
    el->last_idle_check = el->start_time;

    LOG_INFO("Event loop created: max_connections=%d, idle_timeout=%ds, "
             "epoll_timeout=%dms (ET mode)",
             el->config.max_connections,
             el->config.idle_timeout_sec,
             el->config.epoll_timeout_ms);

    return el;
}

int el_run(event_loop_t *el)
{
    struct epoll_event *events;
    int nfds;
    int i;

    if (!el) return -1;

    events = (struct epoll_event *)calloc(
        (size_t)el->config.epoll_max_events, sizeof(struct epoll_event));
    if (!events) {
        LOG_ERROR("el_run: calloc failed for epoll events buffer");
        return -1;
    }

    el->running = 1;
    LOG_INFO("Event loop starting... (active_connections=%d)", el->active_count);

    while (el->running) {
        nfds = epoll_wait(el->epfd, events, el->config.epoll_max_events,
                          el->config.epoll_timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (i = 0; i < nfds; i++) {
            connection_t *conn = (connection_t *)events[i].data.ptr;

            if (!conn) {
                LOG_WARN("epoll event with NULL data.ptr, skipping");
                continue;
            }

            if (conn->state == CONN_STATE_CLOSING ||
                conn->state == CONN_STATE_ERROR) {
                continue;
            }

            if (conn->type == CONN_TYPE_TCP_LISTEN) {
                if (events[i].events & EPOLLIN) {
                    handle_accept(el, conn);
                }
            } else {
                handle_client_event(el, conn, events[i].events);
            }
        }

        time_t now = time(NULL);
        if (now - el->last_idle_check >= IDLE_CHECK_INTERVAL_SEC) {
            cleanup_idle_connections(el);
            el->last_idle_check = now;

            if (el->callbacks.on_idle) {
                el->callbacks.on_idle(el);
            }
        }
    }

    free(events);
    LOG_INFO("Event loop stopped (active_connections=%d)", el->active_count);
    return 0;
}

void el_stop(event_loop_t *el)
{
    if (el) {
        el->running = 0;
    }
}

void el_destroy(event_loop_t *el)
{
    if (!el) return;

    LOG_INFO("Destroying event loop...");

    for (int i = 0; i < el->conn_capacity; i++) {
        if (el->connections[i].type != CONN_TYPE_FREE) {
            connection_t *conn = &el->connections[i];

            if (el->callbacks.on_close) {
                el->callbacks.on_close(el, conn);
            }

            if (conn->fd >= 0) {
                es_del_fd(el->epfd, conn->fd);
                es_close_socket(conn->fd);
            }

            free_connection(conn);
        }
    }

    es_close(el->epfd);
    free(el->connections);
    free(el);

    LOG_INFO("Event loop destroyed");
}

/* ============================================================
 * 连接管理 API
 * ============================================================ */

connection_t *el_add_connection(event_loop_t *el, int fd, conn_type_t type,
                                uint32_t events,
                                const char *remote_addr, uint16_t remote_port)
{
    connection_t *conn;

    if (!el || fd < 0) return NULL;
    if (el->active_count >= el->conn_capacity) {
        LOG_WARN("el_add_connection: connection limit reached (%d/%d)",
                 el->active_count, el->conn_capacity);
        return NULL;
    }

    conn = alloc_connection(el);
    if (!conn) {
        LOG_WARN("el_add_connection: no free connection slots");
        return NULL;
    }

    conn->fd          = fd;
    conn->type        = type;
    conn->state       = CONN_STATE_ACTIVE;
    conn->create_time = time(NULL);
    conn->last_active = conn->create_time;

    if (remote_addr) {
        strncpy(conn->remote_addr, remote_addr, sizeof(conn->remote_addr) - 1);
        conn->remote_addr[sizeof(conn->remote_addr) - 1] = '\0';
    }
    conn->remote_port = remote_port;

    int ret = es_add_fd(el->epfd, fd, events, conn);
    if (ret != ES_OK) {
        LOG_ERROR("el_add_connection: es_add_fd failed for fd=%d", fd);
        free_connection(conn);
        return NULL;
    }

    el->active_count++;
    return conn;
}

void el_remove_connection(event_loop_t *el, connection_t *conn)
{
    if (!el || !conn) return;
    if (conn->type == CONN_TYPE_FREE) return;

    if (conn->fd >= 0) {
        es_del_fd(el->epfd, conn->fd);
        es_close_socket(conn->fd);
    }

    free_connection(conn);
    el->active_count--;
}

connection_t *el_find_connection(event_loop_t *el, int fd)
{
    if (!el || fd < 0) return NULL;

    for (int i = 0; i < el->conn_capacity; i++) {
        if (el->connections[i].fd == fd &&
            el->connections[i].type != CONN_TYPE_FREE) {
            return &el->connections[i];
        }
    }
    return NULL;
}

void el_touch_connection(connection_t *conn)
{
    if (conn) {
        conn->last_active = time(NULL);
    }
}

void el_set_callbacks(event_loop_t *el, const el_callbacks_t *cb)
{
    if (el && cb) {
        memcpy(&el->callbacks, cb, sizeof(el_callbacks_t));
    }
}

int el_get_sip_fd(const event_loop_t *el)
{
    return el ? el->sip_listen_fd : -1;
}

/* ============================================================
 * 发送 API（EPOLLOUT 按需注册）
 * ============================================================ */

int el_send_data(event_loop_t *el, connection_t *conn,
                 const char *data, int len,
                 int is_udp,
                 const struct sockaddr_in *dest_addr, socklen_t dest_len)
{
    int sent;
    int ret;

    if (!el || !conn || !data || len <= 0) {
        return EL_SEND_ERR_PARAM;
    }
    if (len > EL_SEND_BUF_SIZE) {
        LOG_WARN("el_send_data: data too large (%d > %d)", len, EL_SEND_BUF_SIZE);
        return EL_SEND_ERR_FULL;
    }

    if (conn->send_len > 0) {
        LOG_WARN("el_send_data: previous send still pending, replacing");
    }

    memcpy(conn->send_buf, data, (size_t)len);
    conn->send_len    = len;
    conn->send_offset = 0;

    ret = es_send_all(conn->fd, conn->send_buf, 0, conn->send_len,
                      is_udp, dest_addr, dest_len, &sent);
    conn->send_offset = sent;

    if (ret == ES_SEND_DONE) {
        conn->send_len    = 0;
        conn->send_offset = 0;
        return EL_SEND_OK;
    }

    if (ret == ES_SEND_PARTIAL) {
        uint32_t reg_events;
        if (conn->type == CONN_TYPE_TCP_CLIENT) {
            reg_events = DEFAULT_TCP_EVENTS | EPOLLOUT;
        } else {
            reg_events = DEFAULT_UDP_EVENTS | EPOLLOUT;
        }

        int mod_ret = es_mod_fd(el->epfd, conn->fd, reg_events, conn);
        if (mod_ret != ES_OK) {
            LOG_ERROR("el_send_data: es_mod_fd(EPOLLOUT) failed");
            conn->send_len    = 0;
            conn->send_offset = 0;
            return EL_SEND_ERR_PARAM;
        }

        LOG_DEBUG("el_send_data: partial send (%d/%d), registered EPOLLOUT",
                  sent, len);
        return EL_SEND_OK;
    }

    LOG_ERROR("el_send_data: send failed");
    conn->send_len    = 0;
    conn->send_offset = 0;
    return EL_SEND_ERR_PARAM;
}

/* ============================================================
 * 内部函数 — 连接槽位管理
 * ============================================================ */

static connection_t *alloc_connection(event_loop_t *el)
{
    for (int i = 0; i < el->conn_capacity; i++) {
        if (el->connections[i].type == CONN_TYPE_FREE) {
            memset(&el->connections[i], 0, sizeof(connection_t));
            el->connections[i].fd = -1;
            return &el->connections[i];
        }
    }
    return NULL;
}

static void free_connection(connection_t *conn)
{
    if (conn) {
        conn->fd          = -1;
        conn->type        = CONN_TYPE_FREE;
        conn->state       = 0;
        conn->user_data   = NULL;
        conn->remote_addr[0] = '\0';
        conn->remote_port = 0;
        conn->send_len    = 0;
        conn->send_offset = 0;
        memset(conn->send_buf, 0, EL_SEND_BUF_SIZE);
    }
}

/* ============================================================
 * 内部函数 — 事件处理
 * ============================================================ */

static void handle_accept(event_loop_t *el, connection_t *listen_conn)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd;

    while (1) {
        memset(&client_addr, 0, sizeof(client_addr));
        addr_len = sizeof(client_addr);
        client_fd = accept(listen_conn->fd,
                           (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("accept failed: %s (listen_fd=%d)",
                      strerror(errno), listen_conn->fd);
            break;
        }

        if (el->active_count >= el->conn_capacity) {
            LOG_WARN("accept: connection limit reached (%d/%d), rejecting",
                     el->active_count, el->conn_capacity);
            es_close_socket(client_fd);
            continue;
        }

        if (es_set_nonblocking(client_fd) != ES_OK) {
            LOG_WARN("accept: failed to set nonblocking for fd=%d", client_fd);
            es_close_socket(client_fd);
            continue;
        }

        if (el->callbacks.on_accept) {
            el->callbacks.on_accept(el, listen_conn->fd, client_fd,
                                    &client_addr);
        }
    }
}

static void handle_client_event(event_loop_t *el, connection_t *conn,
                                uint32_t events)
{
    if (!conn || conn->state != CONN_STATE_ACTIVE) return;

    conn->last_active = time(NULL);

    if ((events & (EPOLLERR | EPOLLHUP)) && el->callbacks.on_error) {
        int error_code = 0;
        if (events & EPOLLERR) {
            socklen_t len = sizeof(error_code);
            getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error_code, &len);
        }
        LOG_WARN("Connection error on fd=%d (events=0x%x, errno=%d)",
                 conn->fd, events, error_code);
        el->callbacks.on_error(el, conn, error_code);
        return;
    }

    if ((events & EPOLLRDHUP) && el->callbacks.on_close) {
        LOG_INFO("Client disconnected (RDHUP) fd=%d", conn->fd);
        el->callbacks.on_close(el, conn);
        return;
    }

    if ((events & EPOLLOUT) && el->callbacks.on_write) {
        el->callbacks.on_write(el, conn);
    }

    if ((events & EPOLLIN) && el->callbacks.on_read) {
        el->callbacks.on_read(el, conn);
    }
}

static void cleanup_idle_connections(event_loop_t *el)
{
    time_t now = time(NULL);
    int cleaned = 0;

    for (int i = 0; i < el->conn_capacity; i++) {
        connection_t *conn = &el->connections[i];

        if (conn->type == CONN_TYPE_FREE)           continue;
        if (conn->type == CONN_TYPE_TCP_LISTEN)     continue;
        if (conn->state != CONN_STATE_ACTIVE)       continue;

        int idle_sec = (int)(now - conn->last_active);
        if (idle_sec >= el->config.idle_timeout_sec) {
            LOG_INFO("Idle connection timeout: fd=%d idle=%ds type=%d",
                     conn->fd, idle_sec, (int)conn->type);

            if (el->callbacks.on_close) {
                el->callbacks.on_close(el, conn);
            }
            el_remove_connection(el, conn);
            cleaned++;
        }
    }

    if (cleaned > 0) {
        LOG_INFO("Cleaned %d idle connections (active=%d)",
                 cleaned, el->active_count);
    }
}

/* ============================================================
 * 默认事件回调
 * ============================================================ */

static void default_on_accept(event_loop_t *el, int listen_fd,
                              int client_fd,
                              const struct sockaddr_in *client_addr)
{
    char addr_buf[64];
    connection_t *conn;

    conn = el_add_connection(el, client_fd, CONN_TYPE_TCP_CLIENT,
                             DEFAULT_TCP_EVENTS,
                             es_addr_to_str(client_addr, addr_buf, sizeof(addr_buf)),
                             ntohs(client_addr->sin_port));

    if (!conn) {
        LOG_WARN("default_on_accept: failed to register client fd=%d", client_fd);
        es_close_socket(client_fd);
        return;
    }

    LOG_INFO("New TCP connection: fd=%d from %s (ET mode) (listen_fd=%d)",
             client_fd, conn->remote_addr, listen_fd);
}

void el_default_on_read(event_loop_t *el, connection_t *conn)
{
    int is_udp = (conn->type == CONN_TYPE_UDP_SIP ||
                  conn->type == CONN_TYPE_UDP_RTP);

    if (is_udp) {
        char buf[EL_RECV_BUF_SIZE];
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);

        while (1) {
            memset(&peer_addr, 0, sizeof(peer_addr));
            peer_len = sizeof(peer_addr);
            ssize_t n = recvfrom(conn->fd, buf, sizeof(buf),
                                 MSG_DONTWAIT,
                                 (struct sockaddr *)&peer_addr, &peer_len);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                LOG_WARN("UDP recvfrom error on fd=%d: %s", conn->fd,
                         strerror(errno));
                break;
            }

            LOG_DEBUG("UDP received %zd bytes from fd=%d", n, conn->fd);
            buf[(n < (ssize_t)sizeof(buf) - 1) ? n : (ssize_t)sizeof(buf) - 1] = '\0';
        }
    } else {
        char buf[EL_RECV_BUF_SIZE];

        while (1) {
            ssize_t n = read(conn->fd, buf, sizeof(buf) - 1);

            if (n > 0) {
                buf[n] = '\0';
                LOG_DEBUG("Read %zd bytes from fd=%d", n, conn->fd);
                continue;
            }

            if (n == 0) {
                LOG_INFO("Connection closed by peer: fd=%d", conn->fd);
                if (el->callbacks.on_close) {
                    el->callbacks.on_close(el, conn);
                }
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }

            LOG_WARN("Read error on fd=%d: %s", conn->fd, strerror(errno));
            if (el->callbacks.on_error) {
                el->callbacks.on_error(el, conn, errno);
            }
            break;
        }
    }
}

static void default_on_write(event_loop_t *el, connection_t *conn)
{
    if (conn->send_len <= 0 || conn->send_offset >= conn->send_len) {
        LOG_DEBUG("default_on_write: no pending data, removing EPOLLOUT");
        uint32_t reg_events;
        if (conn->type == CONN_TYPE_TCP_CLIENT) {
            reg_events = DEFAULT_TCP_EVENTS;
        } else {
            reg_events = DEFAULT_UDP_EVENTS;
        }
        es_mod_fd(el->epfd, conn->fd, reg_events, conn);
        conn->send_len    = 0;
        conn->send_offset = 0;
        return;
    }

    int remaining = conn->send_len - conn->send_offset;
    int sent;
    int is_udp = (conn->type == CONN_TYPE_UDP_SIP ||
                  conn->type == CONN_TYPE_UDP_RTP);

    int ret = es_send_all(conn->fd, conn->send_buf, conn->send_offset,
                          remaining, is_udp, NULL, 0, &sent);

    if (ret == ES_SEND_ERR) {
        LOG_ERROR("default_on_write: es_send_all failed fd=%d", conn->fd);
        conn->send_len    = 0;
        conn->send_offset = 0;
        if (el->callbacks.on_error) {
            el->callbacks.on_error(el, conn, errno);
        }
        return;
    }

    conn->send_offset += sent;

    if (conn->send_offset >= conn->send_len) {
        LOG_DEBUG("default_on_write: send complete, removing EPOLLOUT");
        uint32_t reg_events;
        if (conn->type == CONN_TYPE_TCP_CLIENT) {
            reg_events = DEFAULT_TCP_EVENTS;
        } else {
            reg_events = DEFAULT_UDP_EVENTS;
        }
        es_mod_fd(el->epfd, conn->fd, reg_events, conn);
        conn->send_len    = 0;
        conn->send_offset = 0;
    }
}

static void default_on_error(event_loop_t *el, connection_t *conn,
                             int error_code)
{
    LOG_WARN("Connection error: fd=%d error=%d (%s)",
             conn->fd, error_code, strerror(error_code));

    conn->state = CONN_STATE_ERROR;

    if (el->callbacks.on_close) {
        el->callbacks.on_close(el, conn);
    }

    el_remove_connection(el, conn);
}

static void default_on_close(event_loop_t *el, connection_t *conn)
{
    LOG_INFO("Closing connection: fd=%d type=%d", conn->fd, (int)conn->type);
    el_remove_connection(el, conn);
}
