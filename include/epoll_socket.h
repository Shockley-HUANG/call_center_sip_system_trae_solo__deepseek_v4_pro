/*
 * epoll_socket.h — epoll + Socket 底层封装
 * ============================================================
 * 封装 Linux epoll 和 Socket 系统调用的底层原语，为上层
 * 事件循环框架提供统一的操作接口。
 *
 * 功能覆盖：
 *   - epoll 实例生命周期（创建/销毁）
 *   - 文件描述符注册/修改/删除
 *   - TCP/UDP 服务端 Socket 创建（socket+bind+listen）
 *   - Socket 属性设置（非阻塞/地址复用）
 *   - 非阻塞发送（支持部分写入 + EPOLLOUT 续传）
 *
 * 依赖：<sys/epoll.h>, <sys/socket.h>, <fcntl.h>, <unistd.h>
 */

#ifndef EPOLL_SOCKET_H
#define EPOLL_SOCKET_H

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

/* 错误码 */
#define ES_OK         0
#define ES_ERR_EPOLL  (-1)
#define ES_ERR_SOCKET (-2)
#define ES_ERR_BIND   (-3)
#define ES_ERR_LISTEN (-4)
#define ES_ERR_CTL    (-5)
#define ES_ERR_PARAM  (-6)
#define ES_ERR_CLOSE  (-7)

/* es_send_all 返回值 */
#define ES_SEND_DONE       0
#define ES_SEND_PARTIAL    1
#define ES_SEND_ERR       (-1)

/* ============================================================
 * epoll 核心操作
 * ============================================================ */

int es_create(int max_events);

int es_add_fd(int epfd, int fd, uint32_t events, void *ptr);

int es_mod_fd(int epfd, int fd, uint32_t events, void *ptr);

int es_del_fd(int epfd, int fd);

int es_close(int epfd);

/* ============================================================
 * Socket 服务端创建
 * ============================================================ */

int es_create_tcp_server(uint16_t port, int backlog, int nonblock, int *out_fd);

int es_create_udp_server(uint16_t port, int nonblock, int *out_fd);

/* ============================================================
 * Socket 属性设置
 * ============================================================ */

int es_set_nonblocking(int fd);

int es_set_reuseaddr(int fd);

void es_close_socket(int fd);

const char *es_addr_to_str(const struct sockaddr_in *addr, char *buf, int buflen);

/* ============================================================
 * 非阻塞发送
 * ============================================================ */

/*
 * 尝试从 buf+offset 发送剩余 len 字节
 * TCP 用 send()，UDP 用 sendto()
 *
 * @param fd         socket fd
 * @param buf        发送缓冲区起始地址
 * @param offset     已发送偏移量
 * @param len        剩余待发送长度
 * @param is_udp     1=UDP(sendto), 0=TCP(send)
 * @param dest_addr  UDP 目标地址（TCP 传 NULL）
 * @param dest_len   UDP 目标地址长度（TCP 传 0）
 * @param out_sent   输出：本次实际发送字节数
 * @return ES_SEND_DONE（全部发完）/ ES_SEND_PARTIAL（部分）/ ES_SEND_ERR（错误）
 */
int es_send_all(int fd, const char *buf, int offset, int len,
                int is_udp,
                const struct sockaddr_in *dest_addr, socklen_t dest_len,
                int *out_sent);

#endif /* EPOLL_SOCKET_H */
