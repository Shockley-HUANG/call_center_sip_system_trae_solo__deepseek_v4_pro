/*
 * sip_handler.h — SIP 协议报文处理模块
 * ============================================================
 * 实现 SIP（Session Initiation Protocol, RFC 3261）报文的接收、
 * 解析、验证、分发与响应生成，为呼叫中心提供底层信令通信
 * 能力。适配 Linux epoll 高并发监听框架。
 *
 * 功能覆盖：
 *   - SIP 报文解析（请求行/状态行、头部、SDP 消息体）
 *   - SIP 请求方法识别（INVITE/ACK/BYE/CANCEL/OPTIONS/REFER 等）
 *   - 报文合法性校验（必填头部、格式合规）
 *   - 异常报文自动过滤与丢弃
 *   - 请求分发：INVITE → Lua 路由 / OPTIONS → 心跳应答
 *   - 基础 SIP 响应构造（100/180/200/404/486/500/603）
 *
 * 依赖：common_types.h, event_loop.h, lua_utils.h, logger.h
 */

#ifndef SIP_HANDLER_H
#define SIP_HANDLER_H

#include <netinet/in.h>
#include "common_types.h"
#include "event_loop.h"

#define SIP_MAX_HEADER_LEN    256
#define SIP_MAX_BODY_LEN     4096
#define SIP_MAX_RAW_LEN      65536
#define SIP_MAX_URI_LEN       256
#define SIP_MAX_CALLID_LEN    256
#define SIP_MAX_CSEQ_LEN       64
#define SIP_MAX_VIA_LEN       256
#define SIP_RESP_BUF_SIZE    2048

typedef enum {
    SIP_METHOD_UNKNOWN = 0,
    SIP_METHOD_INVITE,
    SIP_METHOD_ACK,
    SIP_METHOD_BYE,
    SIP_METHOD_CANCEL,
    SIP_METHOD_REGISTER,
    SIP_METHOD_OPTIONS,
    SIP_METHOD_REFER,
    SIP_METHOD_INFO,
    SIP_METHOD_UPDATE,
    SIP_METHOD_SUBSCRIBE,
    SIP_METHOD_NOTIFY,
    SIP_METHOD_MESSAGE,
} sip_method_t;

typedef enum {
    SIP_PARSE_OK              = 0,
    SIP_PARSE_ERR_EMPTY       = -1,
    SIP_PARSE_ERR_NO_CRLF     = -2,
    SIP_PARSE_ERR_SHORT_LINE  = -3,
    SIP_PARSE_ERR_BAD_METHOD  = -4,
    SIP_PARSE_ERR_NO_SIP_VER  = -5,
} sip_parse_error_t;

typedef struct {
    sip_method_t method;
    int          status_code;
    char         request_uri[SIP_MAX_URI_LEN];

    char call_id[SIP_MAX_CALLID_LEN];
    char from[SIP_MAX_HEADER_LEN];
    char to[SIP_MAX_HEADER_LEN];
    char cseq[SIP_MAX_CSEQ_LEN];
    char contact[SIP_MAX_HEADER_LEN];
    char content_type[SIP_MAX_HEADER_LEN];
    char via[SIP_MAX_VIA_LEN];
    int  content_length;

    char from_uri[SIP_MAX_URI_LEN];
    char from_display[SIP_MAX_HEADER_LEN];
    char to_uri[SIP_MAX_URI_LEN];
    char to_display[SIP_MAX_HEADER_LEN];

    char caller_number[MAX_EXTENSION_LEN];
    char callee_number[MAX_EXTENSION_LEN];

    char body[SIP_MAX_BODY_LEN];

    struct sockaddr_in src_addr;
    socklen_t          src_len;

    const char *raw_data;
    int         raw_len;
} sip_message_t;

void sip_handler_on_read(event_loop_t *el, connection_t *conn);

int sip_parse_message(const char *data, int len, sip_message_t *msg);

int sip_validate_message(const sip_message_t *msg);

void sip_dispatch(event_loop_t *el, connection_t *conn, sip_message_t *msg);

int sip_generate_response(const sip_message_t *request,
                          int status_code, const char *reason_phrase,
                          char *buf, int buf_size);

void sip_send_response(event_loop_t *el, connection_t *conn,
                       const sip_message_t *request,
                       int status_code, const char *reason_phrase);

#endif /* SIP_HANDLER_H */
