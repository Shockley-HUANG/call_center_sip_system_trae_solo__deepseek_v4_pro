/*
 * lua_utils.c — Lua 虚拟机封装层实现
 * ============================================================
 * 实现 lua_utils.h 中声明的所有接口，封装 Lua C API 的底层细节。
 *
 * 本模块是 C ↔ Lua 交互的桥梁，职责包括：
 *   1. Lua 虚拟机生命周期（创建 newstate → 加载脚本 → 销毁 close）
 *   2. 呼叫路由调用（route_call Lua函数 → route_response_t C结构体）
 *   3. 通用 Lua 函数调用（可变参数格式串驱动）
 *   4. 全局变量读取（int / string）
 *   5. Lua 栈调试（lua_stack_dump 打印栈状态）
 *   6. 脚本热重载（不重启 C 服务更新路由规则）
 *
 * 所有函数都包含空指针保护，任何 NULL 参数都会安全返回错误。
 * 所有 Lua 错误都会通过 LOG_ERROR 记录，不会导致 C 程序崩溃。
 *
 * 依赖：
 *   <lua.h> <lauxlib.h> <lualib.h> — Lua 5.x C API
 *   lua_utils.h — 自身接口声明
 *   logger.h    — 日志输出
 */

#include <stdio.h>      /* NULL */
#include <stdlib.h>     /* calloc, free */
#include <string.h>     /* strncpy, memset */
#include <stdarg.h>     /* va_list, va_start, va_arg, va_end */
#include "lua_utils.h"
#include "logger.h"

/* ============================================================
 * 一、虚拟机生命周期管理
 * ============================================================ */

/*
 * 创建 Lua 虚拟机上下文（仅分配 C 结构体内存）
 * 注意：此函数不初始化 lua_State，需要随后调用 lua_vm_init()。
 *
 * @return 成功→堆上的 lua_vm_t 指针（已零初始化），失败→NULL
 */
lua_vm_t *lua_vm_create(void)
{
    /* calloc = malloc + memset(0)，确保所有字段初始为零 */
    lua_vm_t *vm = (lua_vm_t *)calloc(1, sizeof(lua_vm_t));
    if (!vm) {
        LOG_ERROR("Failed to allocate lua_vm_t");
        return NULL;
    }
    return vm;
}

/*
 * 初始化 Lua 虚拟机
 *
 * 流程：
 *   1. luaL_newstate() 创建全新的 lua_State
 *   2. luaL_openlibs() 加载所有标准库（string/table/math/io/os/package/debug/coroutine）
 *
 * 这样 Lua 脚本中就可以使用 print()、string.format()、table.insert() 等标准函数。
 *
 * @param vm  由 lua_vm_create() 创建的上下文
 * @return 0=成功, -1=失败
 */
int lua_vm_init(lua_vm_t *vm)
{
    if (!vm) return -1;

    /* 步骤1: 创建 lua_State */
    vm->L = luaL_newstate();
    if (!vm->L) {
        LOG_ERROR("Failed to create Lua state");
        return -1;
    }

    /* 步骤2: 加载标准库 */
    luaL_openlibs(vm->L);

    LOG_INFO("Lua VM initialized successfully");
    return 0;
}

/*
 * 销毁 Lua 虚拟机
 *
 * 流程：
 *   1. lua_close(L) 释放 Lua 内部资源（GC、注册表、全局表等）
 *   2. free(vm) 释放 C 结构体内存
 *
 * 注意：必须先 lua_close 再 free，否则内存泄漏。
 *
 * @param vm  要销毁的虚拟机上下文
 */
void lua_vm_destroy(lua_vm_t *vm)
{
    if (!vm) return;

    if (vm->L) {
        lua_close(vm->L);   /* 释放 Lua 内部资源 */
        vm->L = NULL;       /* 防止悬空指针 */
        LOG_INFO("Lua VM destroyed");
    }

    free(vm);  /* 释放 C 结构体 */
}

/* ============================================================
 * 二、脚本加载与热重载
 * ============================================================ */

/*
 * 加载并执行 Lua 脚本文件
 *
 * 流程：
 *   1. 保存脚本路径到 vm->script_path（用于后续热重载）
 *   2. luaL_loadfile(L, path) — 编译（不执行），将编译后的 chunk 压栈
 *   3. lua_pcall(L, 0, 0, 0) — 保护执行栈顶的 chunk
 *   4. 如果失败，从栈顶取出错误信息并记录日志，pop 掉错误对象
 *
 * 等价于 Lua 的 dofile(path)，执行后脚本中定义的全局变量/函数可用。
 *
 * @param vm           虚拟机上下文
 * @param script_path  脚本路径（相对或绝对）
 * @return 0=成功, -1=失败
 */
int lua_vm_load_script(lua_vm_t *vm, const char *script_path)
{
    /* 参数校验 */
    if (!vm || !vm->L || !script_path) return -1;

    /* 步骤1: 保存脚本路径 */
    strncpy(vm->script_path, script_path, MAX_LUA_SCRIPT_LEN - 1);

    /* 步骤2+3: 编译 + 保护执行
     * luaL_loadfile 返回 0 表示编译成功
     * lua_pcall 返回 0 表示执行成功
     * 利用 C 短路求值：如果 loadfile 失败，直接走 if 分支，不执行 pcall */
    if (luaL_loadfile(vm->L, script_path) || lua_pcall(vm->L, 0, 0, 0)) {
        /* 步骤4: 从栈顶获取 Lua 错误信息 */
        LOG_ERROR("Failed to load Lua script [%s]: %s",
                  script_path, lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);  /* 弹出错误对象，清理栈 */
        return -1;
    }

    LOG_INFO("Lua script loaded: %s", script_path);
    return 0;
}

/*
 * 热重载脚本
 *
 * 不重启 C 服务，重新执行已加载的脚本文件。
 * 新的全局函数/变量会覆盖旧的同名对象。
 *
 * 使用场景：
 *   - 运营人员修改了 IVR 按键路由，需要立即生效
 *   - 新增了部门，需要更新分机号段映射
 *   - 调整了超时时间或排队策略参数
 *
 * 安全机制：如果重载失败，旧脚本状态不受影响
 * （因为 luaL_loadfile 失败时不会执行 new chunk，旧全局变量仍在）。
 *
 * @param vm  需已加载过脚本的虚拟机上下文
 * @return 0=成功, -1=失败
 */
int lua_vm_reload_script(lua_vm_t *vm)
{
    /* 参数校验：需要已加载过脚本 */
    if (!vm || !vm->L || vm->script_path[0] == '\0') return -1;

    LOG_INFO("Reloading Lua script: %s", vm->script_path);

    /* 重新编译并执行脚本 */
    luaL_loadfile(vm->L, vm->script_path);
    if (lua_pcall(vm->L, 0, 0, 0) != LUA_OK) {
        LOG_ERROR("Failed to reload script: %s", lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);  /* 清理错误对象 */
        return -1;
    }

    LOG_INFO("Lua script reloaded successfully");
    return 0;
}

/* ============================================================
 * 三、呼叫路由接口（核心业务入口）
 * ------------------------------------------------------------
 * 这是本模块最重要的函数，也是 C 调用 Lua 业务逻辑的唯一通道。
 *
 * 每通电话到达时：
 *   C 程序 → lua_vm_call_route() → Lua route_call() → 路由决策
 *
 * Lua 函数签名：
 *   function route_call(caller_id, digits)
 *     return { code=0, target="2000", department="销售部",
 *              description="IVR按键[1] → 销售部队列" }
 *   end
 *
 * 调用流程（Lua 栈视角）：
 *   栈初始状态: [ ]
 *   lua_getglobal("route_call")     → [ function ]
 *   lua_pushstring(caller)           → [ function, string ]
 *   lua_pushstring(digits)           → [ function, string, string ]
 *   lua_pcall(L, 2, 1)               → [ table ]    (调用后弹出函数和参数，压入返回值)
 *   lua_getfield(L, -1, "code")      → [ table, int ]
 *   ... 提取 target, department, description ...
 *   lua_pop(L, 1) 清理 table         → [ ]           (栈恢复)
 * ============================================================ */

int lua_vm_call_route(lua_vm_t *vm, const char *caller_id,
                      const char *digits, route_response_t *response)
{
    /* 参数校验：所有指针必须非空 */
    if (!vm || !vm->L || !digits || !response) return -1;

    /* 步骤1: 初始化响应结构体为零 */
    memset(response, 0, sizeof(route_response_t));

    /* 步骤2: 从 Lua 全局表中取出 route_call 函数，压入栈顶 */
    lua_getglobal(vm->L, "route_call");
    if (!lua_isfunction(vm->L, -1)) {
        LOG_ERROR("Lua function 'route_call' not found in script");
        lua_pop(vm->L, 1);  /* 弹出非函数值，清理栈 */
        return -1;
    }

    /* 步骤3: 压入两个参数
     * caller_id 为空时传空字符串 ""（Lua 会判定为外部来电）
     * digits 为 IVR 按键或分机号 */
    lua_pushstring(vm->L, caller_id ? caller_id : "");
    lua_pushstring(vm->L, digits);

    /* 步骤4: 保护调用
     * pcall(L, nargs=2, nresults=1, errfunc=0)
     * 执行 route_call(caller_id, digits)，期望返回 1 个值 */
    if (lua_pcall(vm->L, 2, 1, 0) != LUA_OK) {
        LOG_ERROR("Lua route_call error: %s", lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);  /* 弹出错误信息 */
        response->code = ROUTE_RESULT_ERROR;
        return -1;
    }

    /* 步骤5: 校验返回值类型（必须是 table） */
    if (!lua_istable(vm->L, -1)) {
        LOG_ERROR("route_call did not return a table");
        lua_pop(vm->L, 1);
        response->code = ROUTE_RESULT_ERROR;
        return -1;
    }

    /* 步骤6: 逐字段提取 table 内容到 C 结构体 */

    /* lua_getfield(L, table_index, "field_name")
     * 将 table["field_name"] 的值压入栈顶
     * 提取后立即 lua_pop 清理，保持栈平衡 */
    lua_getfield(vm->L, -1, "code");
    response->code = (int)lua_tointeger(vm->L, -1);
    lua_pop(vm->L, 1);  /* 弹出 code 值 */

    lua_getfield(vm->L, -1, "target");
    const char *target = lua_tostring(vm->L, -1);
    if (target) strncpy(response->target_extension, target, MAX_EXTENSION_LEN - 1);
    lua_pop(vm->L, 1);  /* 弹出 target 值 */

    lua_getfield(vm->L, -1, "department");
    const char *dept = lua_tostring(vm->L, -1);
    if (dept) strncpy(response->department, dept, MAX_DEPT_NAME_LEN - 1);
    lua_pop(vm->L, 1);  /* 弹出 department 值 */

    lua_getfield(vm->L, -1, "description");
    const char *desc = lua_tostring(vm->L, -1);
    if (desc) strncpy(response->description, desc, MAX_ROUTE_DESC_LEN - 1);
    lua_pop(vm->L, 1);  /* 弹出 description 值 */

    /* 步骤7: 弹出 route_call 返回的 table，栈恢复干净 */
    lua_pop(vm->L, 1);

    /* 记录路由结果到调试日志 */
    LOG_DEBUG("Route result: code=%d target=%s dept=%s desc=%s",
              response->code, response->target_extension,
              response->department, response->description);

    return response->code;
}

/* ============================================================
 * 四、全局变量读取
 * ============================================================ */

/*
 * 读取 Lua 全局整数变量
 *
 * 流程：lua_getglobal → lua_tointeger → lua_pop
 *
 * @param vm   虚拟机上下文
 * @param name 变量名（如 "IVR_TIMEOUT"）
 * @return 整数值（变量不存在时 tointeger 返回 0）
 */
int lua_vm_get_global_int(lua_vm_t *vm, const char *name)
{
    if (!vm || !vm->L || !name) return 0;

    lua_getglobal(vm->L, name);             /* 压入全局变量值 */
    int val = (int)lua_tointeger(vm->L, -1); /* 转换为整数（不存在返回 0） */
    lua_pop(vm->L, 1);                      /* 弹出，恢复栈 */
    return val;
}

/*
 * 读取 Lua 全局字符串变量
 *
 * 流程：lua_getglobal → lua_tostring → lua_pop
 *
 * 注意：返回的指针指向 Lua 栈内存，仅当前调用帧内有效。
 * 调用方应在 lua_pop 之前使用 strncpy() 复制到自己的缓冲区。
 *
 * @param vm   虚拟机上下文
 * @param name 变量名（如 "EXTERNAL_GATEWAY"）
 * @return 字符串指针（变量不存在返回 NULL）
 */
const char *lua_vm_get_global_string(lua_vm_t *vm, const char *name)
{
    if (!vm || !vm->L || !name) return NULL;

    lua_getglobal(vm->L, name);              /* 压入全局变量值 */
    const char *val = lua_tostring(vm->L, -1); /* 获取字符串指针 */
    lua_pop(vm->L, 1);                       /* 弹出，恢复栈 */
    return val;
}

/* ============================================================
 * 五、通用 Lua 函数调用
 * ------------------------------------------------------------
 * 支持调用任意 Lua 全局函数，参数通过格式串指定类型。
 *
 * 格式串规则：
 *   's' → const char*   → lua_pushstring(L, va_arg(ap, const char*))
 *   'd' → int           → lua_pushinteger(L, va_arg(ap, int))
 *   'f' → double        → lua_pushnumber(L, va_arg(ap, double))
 *
 * 示例：
 *   lua_vm_call_function(vm, "get_department_info", "s", "sales");
 *   // Lua: get_department_info("sales")
 *
 * 返回值位于栈顶，调用方可通过 lua_tonumber/lua_tostring 获取。
 * ============================================================ */

int lua_vm_call_function(lua_vm_t *vm, const char *func_name,
                         const char *fmt, ...)
{
    /* 参数校验 */
    if (!vm || !vm->L || !func_name) return -1;

    /* 步骤1: 获取 Lua 全局函数，压入栈顶 */
    lua_getglobal(vm->L, func_name);
    if (!lua_isfunction(vm->L, -1)) {
        LOG_ERROR("Function '%s' not found in Lua", func_name);
        lua_pop(vm->L, 1);  /* 弹出非函数值 */
        return -1;
    }

    /* 步骤2: 解析格式串，依次压入参数 */
    int nargs = 0;  /* 记录压入的参数个数 */
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        /* 遍历格式串字符，每个字符对应一个参数 */
        while (*fmt) {
            switch (*fmt++) {
                case 's':  /* 字符串参数 */
                    lua_pushstring(vm->L, va_arg(args, const char *));
                    break;
                case 'd':  /* 整数参数 */
                    lua_pushinteger(vm->L, va_arg(args, int));
                    break;
                case 'f':  /* 浮点数参数 */
                    lua_pushnumber(vm->L, va_arg(args, double));
                    break;
                default:
                    break;  /* 未知格式字符，跳过 */
            }
            nargs++;
        }
        va_end(args);
    }

    /* 步骤3: 保护调用
     * pcall(L, nargs, nresults=1, errfunc=0)
     * 期望返回 1 个值，留在栈顶供调用方使用 */
    if (lua_pcall(vm->L, nargs, 1, 0) != LUA_OK) {
        LOG_ERROR("Lua call '%s' error: %s",
                  func_name, lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1);  /* 弹出错误对象 */
        return -1;
    }

    return 0;
}

/* ============================================================
 * 六、Lua 栈调试
 * ------------------------------------------------------------
 * 打印当前 Lua 栈的全部内容，用于调试 C↔Lua 交互问题。
 *
 * 常见调试场景：
 *   - 栈不平衡：push 和 pop 次数不匹配
 *   - 类型错误：期望 table 但栈上是 nil
 *   - 参数位置错误：参数压入顺序有问题
 *
 * 输出示例：
 *   === Lua Stack Dump (top=4) ===
 *     [1] string: 'route_call'
 *     [2] string: '4001234567'
 *     [3] string: '1'
 *     [4] table
 * ============================================================ */

void lua_stack_dump(lua_vm_t *vm)
{
    if (!vm || !vm->L) return;

    /* 获取栈顶索引 */
    int top = lua_gettop(vm->L);
    LOG_DEBUG("=== Lua Stack Dump (top=%d) ===", top);

    /* Lua 栈索引从 1 开始（不是 0） */
    for (int i = 1; i <= top; i++) {
        int t = lua_type(vm->L, i);  /* 获取槽位 i 的类型 */
        switch (t) {
            case LUA_TSTRING:
                /* 字符串类型：显示值 */
                LOG_DEBUG("  [%d] string: '%s'", i, lua_tostring(vm->L, i));
                break;
            case LUA_TBOOLEAN:
                /* 布尔类型：显示 true/false */
                LOG_DEBUG("  [%d] boolean: %s", i,
                         lua_toboolean(vm->L, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                /* 数字类型：显示数值 */
                LOG_DEBUG("  [%d] number: %g", i, lua_tonumber(vm->L, i));
                break;
            default:
                /* 其他类型（table/function/nil/userdata/thread）：仅显示类型名 */
                LOG_DEBUG("  [%d] %s", i, lua_typename(vm->L, t));
                break;
        }
    }
}