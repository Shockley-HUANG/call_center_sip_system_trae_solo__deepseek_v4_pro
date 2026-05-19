/*
 * lua_utils.h — Lua 虚拟机封装层头文件
 * ============================================================
 * 将 Lua C API 的原始操作封装为项目专用的高层接口，实现：
 *   - Lua 虚拟机生命周期管理（创建/初始化/销毁）
 *   - Lua 脚本加载与热重载（支持不重启更新路由规则）
 *   - 呼叫路由调用（route_call → route_response_t）
 *   - 通用 Lua 函数调用与参数传递
 *   - Lua 栈调试与全局变量读取
 *
 * 这是 C（底层通信引擎）与 Lua（业务路由脚本）之间的桥梁层。
 * 所有 Lua C API 的细节被封装在本模块内，上层 main.c 只需要
 * 调用 lua_vm_xxx() 系列函数即可完成 C↔Lua 交互。
 *
 * 依赖：<lua.h> <lauxlib.h> <lualib.h>（系统 Lua 库） + common_types.h
 */

#ifndef LUA_UTILS_H
#define LUA_UTILS_H

/* Lua 5.x C API 头文件（需安装 liblua-dev） */
#include <lua.h>       /* lua_State、lua_pushstring、lua_pcall 等基础 API */
#include <lauxlib.h>   /* luaL_newstate、luaL_openlibs、luaL_loadfile 等辅助 API */
#include <lualib.h>    /* 标准库注册声明 */
#include "common_types.h"

/* ============================================================
 * 一、Lua 虚拟机上下文结构体 — lua_vm_t
 * ------------------------------------------------------------
 * 封装一个 Lua 运行实例，包含：
 *   L            — lua_State 指针，Lua 虚拟机核心句柄
 *   script_path  — 当前加载的脚本路径，用于热重载
 *
 * 一个进程通常只创建一个 lua_vm_t 实例（单线程模型）或
 * 每个工作线程持有一个（epoll 多线程模型）。
 * ============================================================ */
typedef struct {
    lua_State *L;                              /* Lua 虚拟机句柄 */
    char       script_path[MAX_LUA_SCRIPT_LEN]; /* 已加载脚本路径 */
} lua_vm_t;

/* ============================================================
 * 二、虚拟机生命周期管理
 * ============================================================ */

/*
 * 创建 Lua 虚拟机上下文（仅分配内存，不初始化 Lua State）
 * @return 成功返回堆上的 lua_vm_t 指针，失败返回 NULL
 */
lua_vm_t *lua_vm_create(void);

/*
 * 初始化 Lua 虚拟机
 * 步骤：
 *   1. 调用 luaL_newstate() 创建 lua_State
 *   2. 调用 luaL_openlibs() 加载标准库（string/table/math/io 等）
 * @param vm  由 lua_vm_create() 创建的上下文
 * @return 0=成功, -1=失败
 */
int lua_vm_init(lua_vm_t *vm);

/*
 * 销毁 Lua 虚拟机，释放所有资源
 * 流程：lua_close(L) → free(vm)
 * @param vm  要销毁的虚拟机上下文
 */
void lua_vm_destroy(lua_vm_t *vm);

/* ============================================================
 * 三、脚本加载与热重载
 * ============================================================ */

/*
 * 加载并执行 Lua 脚本文件
 * 等同于标准 Lua 的 dofile()，脚本中的全局变量/函数将注册到 vm->L 中。
 * 加载成功后记录 script_path，后续可调用 lua_vm_reload_script() 热更新。
 *
 * 流程：luaL_loadfile() → lua_pcall()
 * @param vm           虚拟机上下文
 * @param script_path  脚本文件相对/绝对路径（如 "lua/route.lua"）
 * @return 0=成功, -1=加载或执行失败（错误信息已记录日志）
 */
int lua_vm_load_script(lua_vm_t *vm, const char *script_path);

/*
 * 热重载：重新执行已加载的脚本
 * 适用于不重启 C 服务的情况下更新 Lua 路由规则。
 * 内部调用 luaL_loadfile() + lua_pcall() 覆盖旧的全局函数/变量。
 *
 * @param vm  虚拟机上下文（需已通过 lua_vm_load_script 加载过脚本）
 * @return 0=成功, -1=重载失败（旧脚本状态保持不变）
 */
int lua_vm_reload_script(lua_vm_t *vm);

/* ============================================================
 * 四、呼叫路由接口（核心业务入口）
 * ============================================================ */

/*
 * 调用 Lua 的 route_call(caller_id, digits) 函数
 * ------------------------------------------------------------
 * 这是 C 程序与 Lua 路由层的核心交互接口。每次通话请求到达时，
 * C 程序调用此函数获取路由结果，再根据 result.code 驱动 SIP 信令。
 *
 * 调用流程：
 *   1. 将 "route_call" 函数压入 Lua 栈
 *   2. 压入 caller_id（主叫号码）和 digits（按键/被叫号码）
 *   3. lua_pcall(L, 2, 1) → Lua 执行 route_call，返回 1 个值
 *   4. 解析 Lua 返回的 table，逐字段提取到 route_response_t
 *   5. 清理 Lua 栈
 *
 * @param vm         虚拟机上下文
 * @param caller_id  主叫号码（外部为 "4001234567"，内部为 4 位分机）
 * @param digits     IVR 按键（"1"/"2"/"3"/"4"）或内部分机号（"2001"）
 * @param response   输出参数，Lua 返回的 table 解析到此结构体
 * @return response->code 的值（路由结果码），调用方可据此决定后续动作
 *
 * Lua → C 数据映射：
 *   Lua table { code=0, target="2000", department="销售部", description="..." }
 *     ↓ 逐字段 lua_getfield + lua_tostring/lua_tointeger
 *   C 结构体 route_response_t { .code=0, .target_extension="2000", ... }
 */
int lua_vm_call_route(lua_vm_t *vm, const char *caller_id,
                      const char *digits, route_response_t *response);

/*
 * 通用路由调度：调用任意Lua函数并提取route_response_t
 * ------------------------------------------------------------
 * 支持迭代2新增的7个标准化路由接口：
 *   get_ivr_route / check_agent_status / overflow_route /
 *   time_judge_route / invalid_key_fallback /
 *   timeout_fallback / dept_internal_call_route
 *
 * 使用格式字符串 's'/'d' 指定参数类型：
 *   's' — const char* → lua_pushstring
 *   'd' — int         → lua_pushinteger
 *
 * 调用示例：
 *   lua_vm_route_dispatch(vm, "get_ivr_route", "ss", "4001234567", "1", &resp);
 *   // Lua: get_ivr_route("4001234567", "1") → route_response_t
 *
 * @param vm         虚拟机上下文
 * @param func_name  Lua 函数名
 * @param fmt        参数格式串（'s'/'d'）
 * @param response   输出参数，路由结果
 * @param ...        与格式串对应的参数值
 * @return response->code 的值
 */
int lua_vm_route_dispatch(lua_vm_t *vm, const char *func_name,
                          route_response_t *response,
                          const char *fmt, ...);

/* ============================================================
 * 五、通用 Lua 交互工具
 * ============================================================ */

/*
 * 读取 Lua 全局整数变量
 * @param vm   虚拟机上下文
 * @param name 变量名（如 "IVR_TIMEOUT"）
 * @return 变量值，不存在则返回 0
 */
int lua_vm_get_global_int(lua_vm_t *vm, const char *name);

/*
 * 读取 Lua 全局字符串变量
 * @param vm   虚拟机上下文
 * @param name 变量名（如 "EXTERNAL_GATEWAY"）
 * @return 变量值指针（指向 Lua 栈内存，仅当前帧有效），不存在返回 NULL
 */
const char *lua_vm_get_global_string(lua_vm_t *vm, const char *name);

/*
 * 通用 Lua 函数调用（支持参数传递）
 * ------------------------------------------------------------
 * 使用格式字符串 's'/'d'/'f' 指定参数类型：
 *   's' — const char* → lua_pushstring
 *   'd' — int         → lua_pushinteger
 *   'f' — double      → lua_pushnumber
 *
 * 调用示例：
 *   lua_vm_call_function(vm, "get_department_info", "s", "sales");
 *   // 相当于在 Lua 中调用: get_department_info("sales")
 *
 * 注意：Lua 函数返回值留在栈顶，调用方需自行处理。
 *
 * @param vm        虚拟机上下文
 * @param func_name Lua 函数名
 * @param fmt       参数类型格式串（'s'/'d'/'f'），NULL 表示无参数
 * @param ...       与格式串对应的参数值
 * @return 0=成功, -1=函数不存在或执行失败
 */
int lua_vm_call_function(lua_vm_t *vm, const char *func_name,
                         const char *fmt, ...);

/*
 * 打印 Lua 栈的当前状态（调试用）
 * ------------------------------------------------------------
 * 从栈底到栈顶遍历，依次输出每个槽位的类型和值。
 * 用于排查 C↔Lua 交互时的栈不平衡问题。
 *
 * 输出示例：
 *   === Lua Stack Dump (top=3) ===
 *     [1] string: 'route_call'
 *     [2] table
 *     [3] number: 42
 *
 * @param vm  虚拟机上下文
 */
void lua_stack_dump(lua_vm_t *vm);

#endif /* LUA_UTILS_H */