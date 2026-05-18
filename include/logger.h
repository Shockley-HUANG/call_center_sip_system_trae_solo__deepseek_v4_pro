/*
 * logger.h — 日志系统头文件
 * ============================================================
 * 提供呼叫中心系统全局统一的日志记录接口，支持：
 *   - 五级日志：DEBUG / INFO / WARN / ERROR / FATAL
 *   - 双输出：终端彩色输出 + 文件持久化
 *   - 自动标记：文件名、行号、函数名、时间戳
 *   - 调用方式：通过 LOG_XXX() 宏，无需手动传 __FILE__ 等
 *
 * 使用示例：
 *   logger_init("./log", "sip.log", LOG_LEVEL_DEBUG);
 *   LOG_INFO("Server started on port %d", 5060);
 *   LOG_ERROR("Failed to bind socket: %s", strerror(errno));
 *   logger_close();
 *
 * 注意：LOG_FATAL 会调用 exit(EXIT_FAILURE) 终止程序。
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* ============================================================
 * 一、日志级别枚举 — log_level_t
 * ------------------------------------------------------------
 * 级别从低到高排列，logger_write 会过滤低于当前设定级别的日志。
 * 默认级别为 LOG_LEVEL_INFO（即不输出 DEBUG）。
 * ============================================================ */
typedef enum {
    LOG_LEVEL_DEBUG = 0,  /* 调试信息：函数调用、变量值、Lua栈状态 */
    LOG_LEVEL_INFO,        /* 正常运行信息：启动、连接、配置加载 */
    LOG_LEVEL_WARN,        /* 警告：非致命异常，如重试、降级 */
    LOG_LEVEL_ERROR,       /* 错误：操作失败但程序可继续运行 */
    LOG_LEVEL_FATAL,       /* 致命错误：程序无法继续，记录后 exit */
} log_level_t;

/* ============================================================
 * 二、日志系统核心函数声明
 * ============================================================ */

/*
 * 初始化日志系统
 * @param log_dir    日志文件存放目录（如 "./log"），不存在则自动创建
 * @param filename   日志文件名（如 "sip_server.log"）
 * @param min_level  最低输出级别，低于此级别的日志将被过滤
 * @return 0=成功, -1=打开日志文件失败
 */
int logger_init(const char *log_dir, const char *filename, log_level_t min_level);

/*
 * 动态修改日志级别（运行时热调整，不重启服务）
 * @param level  新的最低输出级别
 */
void logger_set_level(log_level_t level);

/*
 * 写入一条日志（由宏调用，一般不直接使用）
 * @param level  本条日志级别
 * @param file   源文件名（__FILE__）
 * @param line   行号（__LINE__）
 * @param func   函数名（__func__）
 * @param fmt    格式化字符串（printf 风格）
 * @param ...    可变参数
 */
void logger_write(log_level_t level, const char *file, int line,
                  const char *func, const char *fmt, ...);

/*
 * 关闭日志系统
 * 刷新缓冲区、关闭文件句柄、释放资源
 */
void logger_close(void);

/* ============================================================
 * 三、日志宏（推荐使用方式）
 * ------------------------------------------------------------
 * 每个宏自动填充 __FILE__、__LINE__、__func__，
 * 调用方只需提供格式字符串和参数即可。
 *
 * 级别含义：
 *   LOG_DEBUG — 调试用，生产环境通常关闭
 *   LOG_INFO  — 关键节点记录（启动/连接/配置变更）
 *   LOG_WARN  — 需关注但不影响服务的异常
 *   LOG_ERROR — 需要人工介入的错误
 *   LOG_FATAL — 不可恢复错误，记录后退出
 * ============================================================ */

/* 调试日志：如 Lua 栈状态、变量值、路由中间结果 */
#define LOG_DEBUG(fmt, ...) \
    logger_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* 信息日志：服务启动、脚本加载、客户端连接等关键事件 */
#define LOG_INFO(fmt, ...) \
    logger_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* 警告日志：如超时重试、降级路由、配置缺失用默认值 */
#define LOG_WARN(fmt, ...) \
    logger_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* 错误日志：如脚本执行失败、数据库连接断开 */
#define LOG_ERROR(fmt, ...) \
    logger_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* 致命日志：记录后立即调用 exit(EXIT_FAILURE) 终止程序 */
#define LOG_FATAL(fmt, ...) \
    logger_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */