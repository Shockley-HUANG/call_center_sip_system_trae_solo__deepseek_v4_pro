/*
 * common_types.h — 呼叫中心系统全局类型定义
 * ============================================================
 * 本文件定义了整个呼叫中心系统中所有模块共享的数据结构、
 * 枚举类型和系统常量。所有 .c 源文件通过包含此头文件来
 * 使用统一的类型系统，保证各模块间数据交互的一致性。
 *
 * 对应设计方案：
 *   - 第二章：企业组织与分机规划（号段长度、部门名称）
 *   - 第六章：系统技术架构（SIP/RTP 端口、并发数）
 *   - 第七章：完整呼叫流转全流程（会话状态、路由结果）
 *
 * 依赖：<stdint.h>（uint16_t）、<time.h>（time_t）
 */

#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdint.h>
#include <time.h>

/* ============================================================
 * 一、字符串缓冲区长度常量
 * ------------------------------------------------------------
 * 用途：防止栈溢出，所有字符数组统一使用这些宏定义长度，
 *       确保各模块间字符串拷贝安全且一致。
 * ============================================================ */

/* 分机号最大长度（4位短号 + 前缀 + 结束符），如 "1001" */
#define MAX_EXTENSION_LEN    16

/* 部门名称最大长度，如 "售后服务部" */
#define MAX_DEPT_NAME_LEN    64

/* 路由描述信息最大长度 */
#define MAX_ROUTE_DESC_LEN  128

/* Lua 脚本文件路径最大长度 */
#define MAX_LUA_SCRIPT_LEN  256

/* SIP URI 最大长度，如 "sip:1001@192.168.1.100:5060" */
#define MAX_SIP_URI_LEN     256

/* 单条日志消息最大长度 */
#define MAX_LOG_MSG_LEN    1024

/* 配置文件单行最大长度 */
#define MAX_CONFIG_LINE_LEN 512

/* ============================================================
 * 二、网络通信常量
 * ------------------------------------------------------------
 * 用途：SIP/RTP 协议标准端口范围及系统容量上限。
 *       SIP 信令固定 5060 端口，RTP 媒体流使用动态端口池。
 * ============================================================ */

/* SIP 标准信令端口（RFC 3261） */
#define DEFAULT_SIP_PORT    5060

/* RTP 媒体流动态端口范围：下限 */
#define DEFAULT_RTP_PORT_MIN 16384

/* RTP 媒体流动态端口范围：上限 */
#define DEFAULT_RTP_PORT_MAX 32768

/* 系统最大并发通话数（千人企业峰值预留 2x 冗余） */
#define MAX_CONCURRENT_CALLS 2048

/* epoll 单次最大事件数 */
#define MAX_EPOLL_EVENTS 1024

/* epoll_wait 超时间隔（毫秒） */
#define EPOLL_WAIT_TIMEOUT_MS 100

/* TCP listen backlog */
#define SERVER_TCP_BACKLOG 128

/* 连接空闲超时（秒），超时自动释放 */
#define CONNECTION_IDLE_TIMEOUT_SEC 300

/* 空闲连接检测间隔（秒） */
#define IDLE_CHECK_INTERVAL_SEC 10

/* ============================================================
 * 三、超时时间常量
 * ------------------------------------------------------------
 * 用途：控制呼叫中心各环节的等待时长，防止资源长期占用。
 *       对应设计方案 3.2 节（IVR 10 秒超时）和 5.1 节。
 * ============================================================ */

/* IVR 语音导航超时：客户 10 秒内未按键则转入人工兜底 */
#define IVR_TIMEOUT_SECONDS 10

/* 振铃超时：被叫坐席 30 秒未接听则触发溢出策略 */
#define RING_TIMEOUT_SECONDS 30

/* 呼叫保持最大时长：超过 5 分钟自动挂断 */
#define CALL_HOLD_TIMEOUT_SECONDS 300

/* ============================================================
 * 四、通话状态枚举 — call_state_t
 * ------------------------------------------------------------
 * 用途：描述一路通话从发起到结束的完整生命周期。
 *       C 程序根据此状态驱动 SIP 信令和 RTP 媒体流的流转。
 *
 * 状态流转图：
 *   IDLE → RINGING → CONNECTED → HOLD → TRANSFERRING → DISCONNECTED
 *            ↓                       ↓
 *        DISCONNECTED           CONNECTED
 * ============================================================ */
typedef enum {
    CALL_STATE_IDLE         = 0,  /* 空闲（会话刚创建或已释放） */
    CALL_STATE_RINGING,           /* 振铃中（SIP INVITE 已发送，等待对方应答） */
    CALL_STATE_CONNECTED,         /* 通话中（双方 RTP 媒体流已建立） */
    CALL_STATE_HOLD,              /* 呼叫保持（一方暂停，播放等待音乐） */
    CALL_STATE_TRANSFERRING,      /* 转接中（正在协商第三方加入） */
    CALL_STATE_DISCONNECTED,      /* 已挂断（SIP BYE 已发送/接收） */
} call_state_t;

/* ============================================================
 * 五、通话方向枚举 — call_direction_t
 * ------------------------------------------------------------
 * 用途：标记通话的发起方向，影响权限校验和路由策略。
 *       - INBOUND： 外部 400 总机呼入（需 IVR 导航）
 *       - OUTBOUND：内部分机外呼公网
 *       - INTERNAL：内部分机互拨（短号直拨、无 IVR）
 * ============================================================ */
typedef enum {
    CALL_DIRECTION_INBOUND  = 0,  /* 呼入（外部 → 系统） */
    CALL_DIRECTION_OUTBOUND,      /* 呼出（系统 → 外部） */
    CALL_DIRECTION_INTERNAL,      /* 内部（分机 → 分机） */
} call_direction_t;

/* ============================================================
 * 六、路由结果码枚举 — route_result_t
 * ------------------------------------------------------------
 * 用途：route_call() 调用后返回的结果码，告知上层本次路由
 *       是否成功，以及失败时的具体原因。C 程序据此决定下一步
 *       SIP 信令动作（直接接通/排队/转人工兜底/返回错误）。
 *
 * 对应设计方案：
 *   - 3.2 节 IVR 按键路由 → SUCCESS / INVALID_DIGITS
 *   - 4.3 节 人工兜底场景 → DEPT_FULL / FALLBACK_AGENT
 *   - 5.2 节 权限隔离    → INVALID_DIGITS（无权限视为无效）
 * ============================================================ */
typedef enum {
    ROUTE_RESULT_SUCCESS        = 0,   /* 路由成功，已找到目标分机 */
    ROUTE_RESULT_INVALID_DIGITS,       /* 无效按键/分机号 */
    ROUTE_RESULT_DEPT_FULL,            /* 目标部门坐席全忙 */
    ROUTE_RESULT_AGENT_OFFLINE,        /* 目标坐席离线 */
    ROUTE_RESULT_FALLBACK_AGENT,       /* 已转入人工兜底（9000） */
    ROUTE_RESULT_QUEUED        = 5,    /* 已进入排队队列 */
    ROUTE_RESULT_NIGHT_MODE,           /* 非工作时段夜间模式 */
    ROUTE_RESULT_VOICEMAIL,            /* 无值班坐席，转入留言 */
    ROUTE_RESULT_TIMEOUT_RETRY,        /* 超时重试引导 */
    ROUTE_RESULT_INVALID_KEY_RETRY,    /* 无效按键重新引导 */
    ROUTE_RESULT_ERROR         = 99,   /* 未知错误 */
} route_result_t;

/* ============================================================
 * 七、通话会话结构体 — call_session_t
 * ------------------------------------------------------------
 * 用途：存储一路通话的完整上下文信息，贯穿整个呼叫生命周期。
 *       每个活跃通话对应一个 call_session_t 实例。
 *
 * 字段说明：
 *   caller_id    — 主叫号码（外部为 400 号码，内部为 4 位分机）
 *   callee_id    — 被叫号码（路由后的目标分机/部门总机）
 *   sip_uri      — SIP 信令地址（格式: sip:user@host:port）
 *   direction    — 通话方向（呼入/呼出/内部）
 *   state        — 当前通话状态（对应 call_state_t）
 *   sip_socket_fd — SIP 信令 Socket 文件描述符（UDP, 端口 5060）
 *   rtp_socket_fd — RTP 媒体流 Socket 文件描述符（UDP, 动态端口）
 *   rtp_local_port — 本端 RTP 端口号
 *   rtp_remote_port — 对端 RTP 端口号
 *   start_time   — 通话发起时间戳
 *   answer_time  — 对方接听时间戳
 *   end_time     — 通话结束时间戳
 *   department   — 被叫所属部门名称
 *
 * 后续扩展：SIP 对话标识（Call-ID）、编解码协商结果等。
 * ============================================================ */
typedef struct {
    char caller_id[MAX_EXTENSION_LEN];        /* 主叫号码 */
    char callee_id[MAX_EXTENSION_LEN];        /* 被叫号码 */
    char sip_uri[MAX_SIP_URI_LEN];            /* SIP URI 地址 */
    call_direction_t direction;               /* 通话方向 */
    call_state_t state;                       /* 当前状态 */
    int sip_socket_fd;                        /* SIP Socket fd */
    int rtp_socket_fd;                        /* RTP Socket fd */
    uint16_t rtp_local_port;                  /* 本端 RTP 端口 */
    uint16_t rtp_remote_port;                 /* 对端 RTP 端口 */
    time_t start_time;                        /* 发起时间 */
    time_t answer_time;                       /* 接听时间 */
    time_t end_time;                          /* 结束时间 */
    char department[MAX_DEPT_NAME_LEN];       /* 所属部门 */
} call_session_t;

/* ============================================================
 * 八、坐席信息结构体 — agent_info_t
 * ------------------------------------------------------------
 * 用途：描述一名客服坐席的实时状态，用于坐席分配和负载均衡。
 *       后续对接 Redis 时，此结构体将对应 Redis Hash 字段。
 *
 * 字段说明：
 *   extension        — 坐席分机号（如 "9005"）
 *   department       — 坐席所属部门
 *   dept_short_number — 部门总机短号（如 "9000"）
 *   is_online        — 是否在线（1=在线, 0=离线）
 *   concurrent_calls — 当前并发通话数
 *   last_active      — 最后活跃时间戳
 * ============================================================ */
typedef struct {
    char extension[MAX_EXTENSION_LEN];        /* 分机号 */
    char department[MAX_DEPT_NAME_LEN];       /* 所属部门 */
    char dept_short_number[8];                /* 部门总机短号 */
    int is_online;                            /* 在线状态 */
    int concurrent_calls;                     /* 并发通话数 */
    time_t last_active;                       /* 最后活跃时间 */
} agent_info_t;

/* ============================================================
 * 九、路由响应结构体 — route_response_t
 * ------------------------------------------------------------
 * 用途：route_call() Lua 函数返回的路由结果，由 C 程序解析
 *       后驱动 SIP 信令进行实际呼叫接续。
 *
 * Lua 侧返回 table：
 *   { code = 0, target = "2000", department = "销售部",
 *     description = "IVR按键[1] → 转接销售部队列（2000号段）" }
 *
 * C 侧在 lua_vm_call_route() 中逐字段提取到此结构体。
 *
 * 字段说明：
 *   code              — 路由结果码（对应 route_result_t 枚举值）
 *   target_extension  — 目标分机/部门短号
 *   department        — 目标部门名称
 *   description       — 路由过程描述（便于日志追踪）
 * ============================================================ */
typedef struct {
    int code;                                         /* 结果码 */
    char target_extension[MAX_EXTENSION_LEN];         /* 目标分机/短号 */
    char department[MAX_DEPT_NAME_LEN];               /* 目标部门 */
    char description[MAX_ROUTE_DESC_LEN];             /* 过程描述 */
} route_response_t;

#endif /* COMMON_TYPES_H */
