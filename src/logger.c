/*
 * logger.c — 日志系统实现
 * ============================================================
 * 实现 logger.h 中声明的所有日志接口。
 *
 * 核心设计：
 *   - 全局单例：一个进程只持有一个日志实例（static 全局变量）
 *   - 双通道输出：终端 stderr（带 ANSI 颜色） + 日志文件（纯文本）
 *   - 线程安全：当前为单线程设计，后续接入 epoll 多线程时需加互斥锁
 *   - FATAL 级别：记录后自动调用 exit(EXIT_FAILURE)
 *
 * 日志格式：
 *   终端: [级别颜色][LEVEL][重置] [时间] 文件:行号 [函数] 消息内容
 *   文件: [LEVEL] [时间] 文件:行号 [函数] 消息内容
 *
 * 依赖：
 *   common_types.h — MAX_LOG_MSG_LEN 等宏定义
 *   logger.h       — 日志级别枚举、函数声明
 */

#include <stdio.h>      /* fopen, fprintf, fclose, snprintf, stderr */
#include <stdlib.h>     /* exit, EXIT_FAILURE */
#include <string.h>     /* strncpy, strrchr */
#include <sys/stat.h>   /* stat, mkdir */
#include <sys/types.h>  /* mode_t */
#ifndef _WIN32
#include <unistd.h>     /* POSIX 兼容（Linux 必需） */
#endif
#include "common_types.h"
#include "logger.h"

/* ============================================================
 * 一、全局日志状态
 * ------------------------------------------------------------
 * 使用 static 全局变量，整个进程共享一个日志实例。
 * ============================================================ */

static FILE        *log_file       = NULL;              /* 日志文件句柄，NULL 表示未打开 */
static log_level_t  current_level  = LOG_LEVEL_INFO;    /* 当前过滤级别，默认不输出 DEBUG */
static char         log_dir_path[256] = "./log";        /* 日志目录路径 */
static char         log_filename[128] = "sip_server.log"; /* 日志文件名 */

/* 日志级别名称映射（用于文本输出） */
static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/* 日志级别 ANSI 颜色映射（用于终端彩色输出）
 * DEBUG=青色, INFO=绿色, WARN=黄色, ERROR=红色, FATAL=紫色 */
static const char *level_colors[] = {
    "\033[36m",  /* 青色 - DEBUG */
    "\033[32m",  /* 绿色 - INFO  */
    "\033[33m",  /* 黄色 - WARN  */
    "\033[31m",  /* 红色 - ERROR */
    "\033[35m",  /* 紫色 - FATAL */
};

/* ============================================================
 * 二、内部辅助函数
 * ============================================================ */

/*
 * 确保日志目录存在，不存在则创建
 * 使用 stat() 检测目录，mkdir() 创建。
 * 兼容 Linux（0755 权限）和 Windows（无权限参数）。
 */
static void ensure_log_dir(void)
{
    struct stat st = {0};
    /* stat 返回 -1 表示路径不存在 */
    if (stat(log_dir_path, &st) == -1) {
#ifdef _WIN32
        /* Windows: mkdir 只接受路径参数 */
        mkdir(log_dir_path);
#else
        /* Linux: mkdir 接受路径 + 权限（rwxr-xr-x） */
        mkdir(log_dir_path, 0755);
#endif
    }
}

/* ============================================================
 * 三、日志系统生命周期
 * ============================================================ */

/*
 * 初始化日志系统
 *
 * 流程：
 *   1. 保存用户传入的目录路径和文件名
 *   2. 设置日志过滤级别
 *   3. 确保日志目录存在（不存在则 mkdir）
 *   4. 以追加模式打开日志文件
 *   5. 写入初始化标记行
 *
 * @param log_dir    日志目录路径（如 "./log"）
 * @param filename   日志文件名（如 "sip_server.log"）
 * @param min_level  最低输出级别
 * @return 0=成功，-1=无法打开日志文件
 */
int logger_init(const char *log_dir, const char *filename, log_level_t min_level)
{
    /* 步骤1: 保存配置参数 */
    if (log_dir) {
        strncpy(log_dir_path, log_dir, sizeof(log_dir_path) - 1);
    }
    if (filename) {
        strncpy(log_filename, filename, sizeof(log_filename) - 1);
    }

    /* 步骤2: 设置过滤级别 */
    current_level = min_level;

    /* 步骤3: 确保日志目录存在 */
    ensure_log_dir();

    /* 步骤4: 拼接完整路径并打开文件 */
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", log_dir_path, log_filename);

    /* "a" = append mode，追加写入不覆盖历史日志 */
    log_file = fopen(full_path, "a");
    if (!log_file) {
        fprintf(stderr, "[FATAL] Failed to open log file: %s\n", full_path);
        return -1;
    }

    /* 步骤5: 写入初始化标记（便于区分每次启动的日志段） */
    fprintf(log_file, "========== Logger Initialized ==========\n");
    fflush(log_file);  /* 立即刷新到磁盘 */

    return 0;
}

/*
 * 动态修改日志级别
 * 运行时热调整，不需要重启服务。
 * @param level  新的最低输出级别
 */
void logger_set_level(log_level_t level)
{
    current_level = level;
}

/*
 * 关闭日志系统
 * 写入关闭标记、刷新缓冲区、关闭文件句柄、重置全局变量。
 */
void logger_close(void)
{
    if (log_file) {
        fprintf(log_file, "========== Logger Closed ==========\n");
        fflush(log_file);
        fclose(log_file);
        log_file = NULL;  /* 防止野指针 */
    }
}

/* ============================================================
 * 四、日志写入核心函数
 * ------------------------------------------------------------
 * 这是所有 LOG_XXX 宏最终调用的函数。
 *
 * 流程：
 *   1. 级别过滤：低于 current_level 的日志直接返回
 *   2. 获取当前时间戳，格式化为 "YYYY-MM-DD HH:MM:SS"
 *   3. 格式化用户消息内容（vsnprintf）
 *   4. 提取源文件名（仅文件名，不含路径）
 *   5. 终端输出：带 ANSI 颜色
 *   6. 文件输出：纯文本（便于 grep/日志分析工具处理）
 *   7. FATAL 特殊处理：额外输出退出提示，关闭日志，exit
 *
 * @param level  本条日志的级别
 * @param file   源文件名（由 LOG_XXX 宏传入 __FILE__）
 * @param line   行号（由 LOG_XXX 宏传入 __LINE__）
 * @param func   函数名（由 LOG_XXX 宏传入 __func__）
 * @param fmt    格式化字符串
 * @param ...    可变参数
 */
void logger_write(log_level_t level, const char *file, int line,
                  const char *func, const char *fmt, ...)
{
    /* 步骤1: 级别过滤 */
    if (level < current_level) {
        return;
    }

    /* 步骤2: 获取格式化时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* 步骤3: 格式化用户消息 */
    char msg_buf[MAX_LOG_MSG_LEN];  /* 使用全局统一的缓冲区长度 */
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* 步骤4: 提取纯文件名（去掉目录路径，兼容 Linux '/' 和 Windows '\'） */
    const char *fname = strrchr(file, '/');   /* 查找最后一个 '/' */
    fname = fname ? fname + 1                  /* 找到 → 跳过 '/' */
                 : (strrchr(file, '\\')       /* 未找到 → 查找 '\'（Windows） */
                    ? strrchr(file, '\\') + 1
                    : file);                   /* 都没有 → 使用原始字符串 */

    /* 步骤5: 终端彩色输出
     * 格式: [颜色][LEVEL][重置] [时间] 文件:行号 [函数] 消息 */
    fprintf(stderr, "%s[%s]%s [%s] %s:%d [%s] %s\n",
            level_colors[level],   /* 颜色起始 */
            level_names[level],    /* 级别文本 */
            "\033[0m",             /* 颜色重置 */
            time_buf,              /* 时间戳 */
            fname,                 /* 文件名 */
            line,                  /* 行号 */
            func,                  /* 函数名 */
            msg_buf);              /* 消息内容 */

    /* 步骤6: 文件输出（纯文本，无颜色）
     * 格式: [LEVEL] [时间] 文件:行号 [函数] 消息 */
    if (log_file) {
        fprintf(log_file, "[%s] [%s] %s:%d [%s] %s\n",
                level_names[level], time_buf, fname, line, func, msg_buf);
        fflush(log_file);
    }

    /* 步骤7: FATAL 特殊处理 */
    if (level == LOG_LEVEL_FATAL) {
        fprintf(stderr, "[FATAL] Program will exit.\n");
        if (log_file) {
            fprintf(log_file, "[FATAL] Program will exit.\n");
            fflush(log_file);
        }
        logger_close();          /* 确保日志完整写入后再退出 */
        exit(EXIT_FAILURE);      /* 非零退出码表示异常终止 */
    }
}