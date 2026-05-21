/*
 * epoll_socket.c — epoll + Socket 底层封装实现
 * ============================================================
 */

#include "epoll_socket.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

/* ============================================================
 * epoll 核心操作
 * ============================================================ */

int es_create(int max_events)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        return ES_ERR_EPOLL;
    }
    (void)max_events;
    return epfd;
}

int es_add_fd(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events;
    ev.data.ptr = ptr;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        return ES_ERR_CTL;
    }
    return ES_OK;
}

int es_mod_fd(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events;
    ev.data.ptr = ptr;

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl MOD");
        return ES_ERR_CTL;
    }
    return ES_OK;
}

int es_del_fd(int epfd, int fd)
{
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("epoll_ctl DEL");
        return ES_ERR_CTL;
    }
    return ES_OK;
}

int es_close(int epfd)
{
    if (close(epfd) < 0) {
        perror("close epoll");
        return ES_ERR_CLOSE;
    }
    return ES_OK;
}

/* ============================================================
 * Socket 服务端创建
 * ============================================================ */

int es_create_tcp_server(uint16_t port, int backlog, int nonblock, int *out_fd)
{
    int fd;
    int ret;

    if (!out_fd) return ES_ERR_PARAM;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return ES_ERR_SOCKET;
    }

    ret = es_set_reuseaddr(fd);
    if (ret != ES_OK) goto fail;

    if (nonblock) {
        ret = es_set_nonblocking(fd);
        if (ret != ES_OK) goto fail;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind TCP");
        ret = ES_ERR_BIND;
        goto fail;
    }

    if (listen(fd, backlog) < 0) {
        perror("listen TCP");
        ret = ES_ERR_LISTEN;
        goto fail;
    }

    *out_fd = fd;
    return ES_OK;

fail:
    es_close_socket(fd);
    return ret;
}

int es_create_udp_server(uint16_t port, int nonblock, int *out_fd)
{
    int fd;
    int ret;

    if (!out_fd) return ES_ERR_PARAM;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket UDP");
        return ES_ERR_SOCKET;
    }

    ret = es_set_reuseaddr(fd);
    if (ret != ES_OK) goto fail;

    if (nonblock) {
        ret = es_set_nonblocking(fd);
        if (ret != ES_OK) goto fail;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind UDP");
        ret = ES_ERR_BIND;
        goto fail;
    }

    *out_fd = fd;
    return ES_OK;

fail:
    es_close_socket(fd);
    return ret;
}

/* ============================================================
 * Socket 属性设置
 * ============================================================ */

int es_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return ES_ERR_SOCKET;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return ES_ERR_SOCKET;
    }
    return ES_OK;
}

int es_set_reuseaddr(int fd)
{
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        return ES_ERR_SOCKET;
    }
    return ES_OK;
}

void es_close_socket(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

const char *es_addr_to_str(const struct sockaddr_in *addr, char *buf, int buflen)
{
    if (!addr || !buf || buflen <= 0) return "(null)";
    snprintf(buf, (size_t)buflen, "%s:%u",
             inet_ntoa(addr->sin_addr),
             (unsigned int)ntohs(addr->sin_port));
    return buf;
}

/* ============================================================
 * 非阻塞发送
 * ============================================================ */

int es_send_all(int fd, const char *buf, int offset, int len,
                int is_udp,
                const struct sockaddr_in *dest_addr, socklen_t dest_len,
                int *out_sent)
{
    ssize_t n;

    if (!buf || len <= 0 || offset < 0 || !out_sent) {
        return ES_SEND_ERR;
    }

    if (is_udp) {
        n = sendto(fd, buf + offset, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL,
                   (const struct sockaddr *)dest_addr, dest_len);
    } else {
        n = send(fd, buf + offset, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *out_sent = 0;
            return ES_SEND_PARTIAL;
        }
        if (errno == EINTR) {
            *out_sent = 0;
            return ES_SEND_PARTIAL;
        }
        perror("es_send_all");
        *out_sent = 0;
        return ES_SEND_ERR;
    }

    *out_sent = (int)n;

    if (n >= (ssize_t)len) {
        return ES_SEND_DONE;
    }
    return ES_SEND_PARTIAL;
}
