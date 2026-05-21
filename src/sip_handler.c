#include "sip_handler.h"
#include "epoll_socket.h"
#include "logger.h"
#include "lua_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>

static sip_method_t sip_method_from_string(const char *method_str)
{
    if (!method_str) return SIP_METHOD_UNKNOWN;
    if (strcasecmp(method_str, "INVITE")    == 0) return SIP_METHOD_INVITE;
    if (strcasecmp(method_str, "ACK")       == 0) return SIP_METHOD_ACK;
    if (strcasecmp(method_str, "BYE")       == 0) return SIP_METHOD_BYE;
    if (strcasecmp(method_str, "CANCEL")    == 0) return SIP_METHOD_CANCEL;
    if (strcasecmp(method_str, "REGISTER")  == 0) return SIP_METHOD_REGISTER;
    if (strcasecmp(method_str, "OPTIONS")   == 0) return SIP_METHOD_OPTIONS;
    if (strcasecmp(method_str, "REFER")     == 0) return SIP_METHOD_REFER;
    if (strcasecmp(method_str, "INFO")      == 0) return SIP_METHOD_INFO;
    if (strcasecmp(method_str, "UPDATE")    == 0) return SIP_METHOD_UPDATE;
    if (strcasecmp(method_str, "SUBSCRIBE") == 0) return SIP_METHOD_SUBSCRIBE;
    if (strcasecmp(method_str, "NOTIFY")    == 0) return SIP_METHOD_NOTIFY;
    if (strcasecmp(method_str, "MESSAGE")   == 0) return SIP_METHOD_MESSAGE;
    return SIP_METHOD_UNKNOWN;
}

static const char *sip_method_name(sip_method_t method)
{
    switch (method) {
        case SIP_METHOD_INVITE:    return "INVITE";
        case SIP_METHOD_ACK:       return "ACK";
        case SIP_METHOD_BYE:       return "BYE";
        case SIP_METHOD_CANCEL:    return "CANCEL";
        case SIP_METHOD_REGISTER:  return "REGISTER";
        case SIP_METHOD_OPTIONS:   return "OPTIONS";
        case SIP_METHOD_REFER:     return "REFER";
        case SIP_METHOD_INFO:      return "INFO";
        case SIP_METHOD_UPDATE:    return "UPDATE";
        case SIP_METHOD_SUBSCRIBE: return "SUBSCRIBE";
        case SIP_METHOD_NOTIFY:    return "NOTIFY";
        case SIP_METHOD_MESSAGE:   return "MESSAGE";
        default:                   return "UNKNOWN";
    }
}

static int sip_parse_start_line(const char *data, int len,
                                sip_message_t *msg, int *out_offset)
{
    const char *crlf;
    const char *line;
    int line_len;

    if (len <= 0) return SIP_PARSE_ERR_EMPTY;

    crlf = strstr(data, "\r\n");
    if (!crlf) return SIP_PARSE_ERR_NO_CRLF;

    line = data;
    line_len = (int)(crlf - data);

    if (line_len < 7) return SIP_PARSE_ERR_SHORT_LINE;

    if (strncasecmp(line, "SIP/2.0", 7) == 0) {
        const char *sp1 = strchr(line, ' ');
        if (!sp1) return SIP_PARSE_ERR_SHORT_LINE;
        msg->status_code = atoi(sp1 + 1);
        msg->method = SIP_METHOD_UNKNOWN;
    } else {
        char method_buf[32] = {0};
        const char *sp1 = strchr(line, ' ');
        if (!sp1) return SIP_PARSE_ERR_BAD_METHOD;

        int mlen = (int)(sp1 - line);
        if (mlen <= 0 || mlen >= (int)sizeof(method_buf)) {
            return SIP_PARSE_ERR_BAD_METHOD;
        }
        memcpy(method_buf, line, (size_t)mlen);
        msg->method = sip_method_from_string(method_buf);

        if (msg->method == SIP_METHOD_UNKNOWN) {
            LOG_DEBUG("SIP: unknown method '%.*s'", mlen, line);
        }

        const char *sp2 = strchr(sp1 + 1, ' ');
        if (sp2) {
            int uri_len = (int)(sp2 - sp1 - 1);
            if (uri_len > 0 && uri_len < SIP_MAX_URI_LEN) {
                memcpy(msg->request_uri, sp1 + 1, (size_t)uri_len);
                msg->request_uri[uri_len] = '\0';
            }
        }
    }

    *out_offset = (int)(crlf - data) + 2;
    return SIP_PARSE_OK;
}

static void sip_parse_headers(const char *data, int len, int offset,
                              sip_message_t *msg, int *out_body_offset)
{
    int pos = offset;
    const char *p = data;

    while (pos < len) {
        const char *line_start = p + pos;
        int remaining = len - pos;

        if (remaining <= 0) break;

        if (remaining >= 2 && line_start[0] == '\r' && line_start[1] == '\n') {
            pos += 2;
            break;
        }

        const char *crlf = (const char *)memmem(line_start, (size_t)remaining,
                                                "\r\n", 2);
        if (!crlf) break;

        int hdr_len = (int)(crlf - line_start);
        pos += hdr_len + 2;

        const char *colon = (const char *)memchr(line_start, ':', (size_t)hdr_len);
        if (!colon) continue;

        int key_len = (int)(colon - line_start);
        const char *value_start = colon + 1;
        while (value_start < crlf && *value_start == ' ') value_start++;
        int value_len = (int)(crlf - value_start);

        if (key_len == 7 && strncasecmp(line_start, "Call-ID", 7) == 0) {
            int cp_len = value_len < SIP_MAX_CALLID_LEN - 1
                         ? value_len : SIP_MAX_CALLID_LEN - 1;
            memcpy(msg->call_id, value_start, (size_t)cp_len);
            msg->call_id[cp_len] = '\0';
        } else if (key_len == 4 && strncasecmp(line_start, "From", 4) == 0) {
            int cp_len = value_len < SIP_MAX_HEADER_LEN - 1
                         ? value_len : SIP_MAX_HEADER_LEN - 1;
            memcpy(msg->from, value_start, (size_t)cp_len);
            msg->from[cp_len] = '\0';
        } else if (key_len == 2 && strncasecmp(line_start, "To", 2) == 0) {
            int cp_len = value_len < SIP_MAX_HEADER_LEN - 1
                         ? value_len : SIP_MAX_HEADER_LEN - 1;
            memcpy(msg->to, value_start, (size_t)cp_len);
            msg->to[cp_len] = '\0';
        } else if (key_len == 4 && strncasecmp(line_start, "CSeq", 4) == 0) {
            int cp_len = value_len < SIP_MAX_CSEQ_LEN - 1
                         ? value_len : SIP_MAX_CSEQ_LEN - 1;
            memcpy(msg->cseq, value_start, (size_t)cp_len);
            msg->cseq[cp_len] = '\0';
        } else if (key_len == 7 && strncasecmp(line_start, "Contact", 7) == 0) {
            int cp_len = value_len < SIP_MAX_HEADER_LEN - 1
                         ? value_len : SIP_MAX_HEADER_LEN - 1;
            memcpy(msg->contact, value_start, (size_t)cp_len);
            msg->contact[cp_len] = '\0';
        } else if (key_len == 12 && strncasecmp(line_start, "Content-Type", 12) == 0) {
            int cp_len = value_len < SIP_MAX_HEADER_LEN - 1
                         ? value_len : SIP_MAX_HEADER_LEN - 1;
            memcpy(msg->content_type, value_start, (size_t)cp_len);
            msg->content_type[cp_len] = '\0';
        } else if (key_len == 3 && strncasecmp(line_start, "Via", 3) == 0) {
            int cp_len = value_len < SIP_MAX_VIA_LEN - 1
                         ? value_len : SIP_MAX_VIA_LEN - 1;
            memcpy(msg->via, value_start, (size_t)cp_len);
            msg->via[cp_len] = '\0';
        } else if (key_len == 14 && strncasecmp(line_start, "Content-Length", 14) == 0) {
            msg->content_length = atoi(value_start);
        }
    }

    *out_body_offset = pos;
}

static void sip_extract_uri_user(const char *header_value,
                                 char *user_buf, int buf_size)
{
    const char *sip_start;
    const char *at;
    const char *colon;
    const char *semi;
    const char *end;

    if (!header_value || !user_buf || buf_size <= 0) return;
    user_buf[0] = '\0';

    sip_start = strstr(header_value, "sip:");
    if (!sip_start) {
        sip_start = strstr(header_value, "SIP:");
        if (!sip_start) {
            sip_start = strchr(header_value, ':');
            if (sip_start) sip_start++;
            else sip_start = header_value;
        } else {
            sip_start += 4;
        }
    } else {
        sip_start += 4;
    }

    at = strchr(sip_start, '@');
    colon = strchr(sip_start, ':');
    semi = strchr(sip_start, ';');

    if (at) {
        end = at;
    } else if (colon) {
        end = colon;
    } else if (semi) {
        end = semi;
    } else {
        end = sip_start + strlen(sip_start);
    }

    int user_len = (int)(end - sip_start);
    if (user_len <= 0) return;
    if (user_len >= buf_size) user_len = buf_size - 1;

    memcpy(user_buf, sip_start, (size_t)user_len);
    user_buf[user_len] = '\0';
}

int sip_parse_message(const char *data, int len, sip_message_t *msg)
{
    int offset = 0;
    int body_offset = 0;
    int ret;

    if (!data || !msg) return SIP_PARSE_ERR_EMPTY;
    if (len <= 0) return SIP_PARSE_ERR_EMPTY;

    memset(msg, 0, sizeof(sip_message_t));
    msg->raw_data = data;
    msg->raw_len = len;
    msg->content_length = 0;

    ret = sip_parse_start_line(data, len, msg, &offset);
    if (ret != SIP_PARSE_OK) {
        return ret;
    }

    sip_parse_headers(data, len, offset, msg, &body_offset);

    sip_extract_uri_user(msg->from, msg->from_uri, sizeof(msg->from_uri));
    sip_extract_uri_user(msg->to, msg->to_uri, sizeof(msg->to_uri));

    if (msg->from[0]) {
        const char *lt = strchr(msg->from, '<');
        if (lt) {
            int d_len = (int)(lt - msg->from);
            while (d_len > 0 && msg->from[d_len - 1] == ' ') d_len--;
            if (d_len > 0 && msg->from[0] == '"') {
                int cp = d_len - 2;
                if (cp > 0 && cp < SIP_MAX_HEADER_LEN) {
                    memcpy(msg->from_display, msg->from + 1, (size_t)cp);
                    msg->from_display[cp] = '\0';
                }
            }
        }
    }

    sip_extract_uri_user(msg->from, msg->caller_number, sizeof(msg->caller_number));
    sip_extract_uri_user(msg->to, msg->callee_number, sizeof(msg->callee_number));

    if (msg->content_length > 0) {
        int body_start = body_offset;
        int remain = len - body_start;
        int cp_len = msg->content_length;
        if (cp_len > remain) cp_len = remain;
        if (cp_len > SIP_MAX_BODY_LEN - 1) cp_len = SIP_MAX_BODY_LEN - 1;
        if (cp_len > 0) {
            memcpy(msg->body, data + body_start, (size_t)cp_len);
            msg->body[cp_len] = '\0';
        }
    }

    return SIP_PARSE_OK;
}

int sip_validate_message(const sip_message_t *msg)
{
    if (!msg) return -1;

    if (msg->status_code > 0) return 0;

    if (msg->method == SIP_METHOD_UNKNOWN) {
        LOG_DEBUG("SIP: unknown method, discarding");
        return -2;
    }

    if (msg->call_id[0] == '\0') {
        LOG_DEBUG("SIP: missing Call-ID header, discarding");
        return -3;
    }

    if (msg->from[0] == '\0') {
        LOG_DEBUG("SIP: missing From header, discarding");
        return -4;
    }

    if (msg->to[0] == '\0') {
        LOG_DEBUG("SIP: missing To header, discarding");
        return -5;
    }

    if (msg->cseq[0] == '\0') {
        LOG_DEBUG("SIP: missing CSeq header, discarding");
        return -6;
    }

    return 0;
}

static void sip_handle_invite(event_loop_t *el, connection_t *conn,
                              sip_message_t *msg)
{
    lua_vm_t *vm;
    route_response_t route_resp;

    LOG_INFO("SIP INVITE: caller=%s callee=%s Call-ID=%s",
             msg->caller_number, msg->callee_number, msg->call_id);

    if (msg->caller_number[0] == '\0' || msg->callee_number[0] == '\0') {
        LOG_WARN("SIP INVITE: missing caller or callee, sending 400 Bad Request");
        sip_send_response(el, conn, msg, 400, "Bad Request");
        return;
    }

    vm = (lua_vm_t *)el->user_data;
    if (!vm) {
        LOG_ERROR("SIP INVITE: Lua VM not available, sending 500");
        sip_send_response(el, conn, msg, 500, "Server Internal Error");
        return;
    }

    memset(&route_resp, 0, sizeof(route_resp));
    int result = lua_vm_call_route(vm, msg->caller_number,
                                   msg->callee_number, &route_resp);

    if (result < 0) {
        LOG_ERROR("SIP INVITE: Lua route_call failed, sending 500");
        sip_send_response(el, conn, msg, 500, "Server Internal Error");
        return;
    }

    LOG_INFO("SIP INVITE: route result code=%d target=%s desc=%s",
             route_resp.code, route_resp.target_extension,
             route_resp.description);

    sip_send_response(el, conn, msg, 100, "Trying");

    switch (route_resp.code) {
        case ROUTE_RESULT_SUCCESS:
            sip_send_response(el, conn, msg, 180, "Ringing");
            break;
        case ROUTE_RESULT_DEPT_FULL:
        case ROUTE_RESULT_AGENT_OFFLINE:
            sip_send_response(el, conn, msg, 486, "Busy Here");
            break;
        case ROUTE_RESULT_INVALID_DIGITS:
            sip_send_response(el, conn, msg, 404, "Not Found");
            break;
        case ROUTE_RESULT_NIGHT_MODE:
        case ROUTE_RESULT_VOICEMAIL:
            sip_send_response(el, conn, msg, 480, "Temporarily Unavailable");
            break;
        case ROUTE_RESULT_QUEUED:
        case ROUTE_RESULT_FALLBACK_AGENT:
        case ROUTE_RESULT_TIMEOUT_RETRY:
        case ROUTE_RESULT_INVALID_KEY_RETRY:
            sip_send_response(el, conn, msg, 180, "Ringing");
            break;
        default:
            sip_send_response(el, conn, msg, 500, "Server Internal Error");
            break;
    }
}

static void sip_handle_options(event_loop_t *el, connection_t *conn,
                               sip_message_t *msg)
{
    LOG_DEBUG("SIP OPTIONS heartbeat from Call-ID=%s", msg->call_id);
    sip_send_response(el, conn, msg, 200, "OK");
}

static void sip_handle_bye(event_loop_t *el, connection_t *conn,
                           sip_message_t *msg)
{
    LOG_INFO("SIP BYE: caller=%s callee=%s Call-ID=%s",
             msg->caller_number[0] ? msg->caller_number : "unknown",
             msg->callee_number[0] ? msg->callee_number : "unknown",
             msg->call_id);
    sip_send_response(el, conn, msg, 200, "OK");
}

static void sip_handle_cancel(event_loop_t *el, connection_t *conn,
                              sip_message_t *msg)
{
    LOG_INFO("SIP CANCEL: Call-ID=%s", msg->call_id);
    sip_send_response(el, conn, msg, 200, "OK");
}

static void sip_handle_refer(event_loop_t *el, connection_t *conn,
                             sip_message_t *msg)
{
    LOG_INFO("SIP REFER (transfer): caller=%s callee=%s Call-ID=%s",
             msg->caller_number[0] ? msg->caller_number : "unknown",
             msg->callee_number[0] ? msg->callee_number : "unknown",
             msg->call_id);
    sip_send_response(el, conn, msg, 202, "Accepted");
}

void sip_dispatch(event_loop_t *el, connection_t *conn, sip_message_t *msg)
{
    if (!el || !conn || !msg) return;

    if (msg->status_code > 0) {
        LOG_DEBUG("SIP: received response %d, ignoring", msg->status_code);
        return;
    }

    switch (msg->method) {
        case SIP_METHOD_INVITE:
            sip_handle_invite(el, conn, msg);
            break;
        case SIP_METHOD_ACK:
            LOG_DEBUG("SIP ACK: Call-ID=%s", msg->call_id);
            break;
        case SIP_METHOD_BYE:
            sip_handle_bye(el, conn, msg);
            break;
        case SIP_METHOD_CANCEL:
            sip_handle_cancel(el, conn, msg);
            break;
        case SIP_METHOD_OPTIONS:
            sip_handle_options(el, conn, msg);
            break;
        case SIP_METHOD_REFER:
            sip_handle_refer(el, conn, msg);
            break;
        case SIP_METHOD_REGISTER:
            LOG_DEBUG("SIP REGISTER: from=%s", msg->caller_number);
            sip_send_response(el, conn, msg, 200, "OK");
            break;
        default:
            LOG_DEBUG("SIP: unhandled method %s, sending 405",
                      sip_method_name(msg->method));
            sip_send_response(el, conn, msg, 405, "Method Not Allowed");
            break;
    }
}

int sip_generate_response(const sip_message_t *request,
                          int status_code, const char *reason_phrase,
                          char *buf, int buf_size)
{
    int written;

    if (!buf || buf_size <= 0) return -1;

    written = snprintf(buf, (size_t)buf_size,
        "SIP/2.0 %d %s\r\n"
        "Via: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        status_code, reason_phrase ? reason_phrase : "OK",
        request && request->via[0] ? request->via : "SIP/2.0/UDP unknown",
        request && request->from[0] ? request->from : "<sip:unknown>",
        request && request->to[0] ? request->to : "<sip:unknown>",
        request && request->call_id[0] ? request->call_id : "unknown",
        request && request->cseq[0] ? request->cseq : "0 UNKNOWN");

    if (written < 0 || written >= buf_size) {
        LOG_ERROR("SIP: response buffer overflow");
        return -1;
    }

    return written;
}

void sip_send_response(event_loop_t *el, connection_t *conn,
                       const sip_message_t *request,
                       int status_code, const char *reason_phrase)
{
    char buf[SIP_RESP_BUF_SIZE];
    int len;

    (void)el;

    len = sip_generate_response(request, status_code, reason_phrase,
                                buf, sizeof(buf));
    if (len <= 0) return;

    if (request && request->src_len > 0) {
        int sent;
        int ret = es_send_all(conn->fd, buf, 0, len,
                              1, &request->src_addr, request->src_len, &sent);

        if (ret == ES_SEND_DONE) {
            LOG_DEBUG("SIP: sent %d %s (%d bytes) to %s:%d",
                      status_code, reason_phrase, sent,
                      inet_ntoa(request->src_addr.sin_addr),
                      ntohs(request->src_addr.sin_port));
        } else if (ret == ES_SEND_PARTIAL) {
            LOG_WARN("SIP: partial send %d/%d bytes for %d %s",
                     sent, len, status_code, reason_phrase);
        } else {
            LOG_ERROR("SIP: send failed for %d %s", status_code, reason_phrase);
        }
    }
}

void sip_handler_on_read(event_loop_t *el, connection_t *conn)
{
    if (conn->type != CONN_TYPE_UDP_SIP) {
        el_default_on_read(el, conn);
        return;
    }

    {
        char buf[SIP_MAX_RAW_LEN];
        struct sockaddr_in peer_addr;
        socklen_t peer_len;
        ssize_t n;
        sip_message_t msg;
        int parse_ret;
        int valid_ret;

        while (1) {
            memset(&peer_addr, 0, sizeof(peer_addr));
            peer_len = sizeof(peer_addr);
            n = recvfrom(conn->fd, buf, sizeof(buf) - 1,
                         MSG_DONTWAIT,
                         (struct sockaddr *)&peer_addr, &peer_len);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                LOG_WARN("SIP UDP recvfrom error on fd=%d: %s",
                         conn->fd, strerror(errno));
                break;
            }

            if (n == 0) continue;

            buf[n] = '\0';

            LOG_DEBUG("SIP UDP received %zd bytes from %s:%d",
                      n, inet_ntoa(peer_addr.sin_addr),
                      ntohs(peer_addr.sin_port));

            parse_ret = sip_parse_message(buf, (int)n, &msg);
            if (parse_ret != SIP_PARSE_OK) {
                LOG_DEBUG("SIP: parse error code=%d, discarding %zd bytes",
                          parse_ret, n);
                continue;
            }

            msg.src_addr = peer_addr;
            msg.src_len = peer_len;

            valid_ret = sip_validate_message(&msg);
            if (valid_ret != 0) {
                LOG_DEBUG("SIP: validation failed code=%d, discarding", valid_ret);
                continue;
            }

            sip_dispatch(el, conn, &msg);
        }
    }
}
