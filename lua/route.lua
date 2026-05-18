--
-- route.lua — 千人企业呼叫中心路由脚本
-- ============================================================
-- 本项目核心业务逻辑脚本，由 C 程序在每通电话到达时调用。
--
-- 职责：
--   1. 外部 400 总机 IVR 按键导航（设计方案 3.2 节）
--   2. 部门分机号段匹配与路由（设计方案 2.2 节）
--   3. 内/外权限隔离（设计方案 5.2 节）
--   4. 24h 人工服务台兜底（设计方案 4.3 节）
--   5. 坐席全忙溢出策略（设计方案 7.3 节）
--
-- C ↔ Lua 接口：
--   C 侧调用：
--     lua_vm_call_route(vm, caller_id, digits, &response)
--       → 触发本脚本的 route_call(caller_id, digits)
--       → 返回 table { code, target, department, description }
--       → C 侧解析 table 字段到 route_response_t 结构体
--
-- 热更新：
--   本脚本支持不重启 C 服务直接更新。
--   修改后调用 lua_vm_reload_script(vm) 即可生效。
--   重载失败时旧脚本状态不受影响。
--
-- 对应设计方案：
--   第二章：企业组织与分机整体规划（部门 + 号段）
--   第三章：对外 400 总机系统设计（IVR 按键映射）
--   第四章：24h 人工服务台设计（兜底策略）
--   第五章：内部通话规则设计（分机互拨 + 权限）
--   第七章：完整呼叫流转全流程
--

-- ============================================================
-- 一、全局常量定义
-- ------------------------------------------------------------
-- 这些常量定义了呼叫中心的核心运行参数，可在运行期间
-- 通过 lua_vm_get_global_int / lua_vm_get_global_string 读取。
-- ============================================================

-- IVR 语音导航超时时间（秒）
-- 客户在语音播报结束后 10 秒内未按键，自动转入人工兜底。
local IVR_TIMEOUT = 10

-- 24小时人工服务台兜底号码
-- 所有异常场景（超时/无效按键/全忙溢出）均转入此号码。
local FALLBACK_AGENT = "9000"

-- 对外统一 400 总机号码
-- 匹配设计方案 3.1 节：400-123-4567
local EXTERNAL_GATEWAY = "400-123-4567"

-- ============================================================
-- 二、部门配置表 — DEPARTMENTS
-- ------------------------------------------------------------
-- 定义企业 9 个部门的完整信息，严格对齐设计方案 2.2 节。
--
-- 每个部门字段说明：
--   short       — 部门总机短号（外人拨此号码直达部门队列）
--   range_start — 员工分机号段起始
--   range_end   — 员工分机号段结束
--   name        — 部门中文名称
--   external    — 是否对外暴露（false=仅内部可拨，true=外部可通过 IVR 访问）
--
-- 内外部权限隔离规则（5.2 节）：
--   external = true  → 对外开放（销售 / 售后 / 市场 / 人工台）
--   external = false → 仅内部可拨（人事 / 财务 / 行政 / 管理层 / 研发）
-- ============================================================
local DEPARTMENTS = {
    -- ============ 职能后台部门（对内，外部不可直拨）============
    hr         = { short = "1000", range_start = 1001, range_end = 1050,
                   name = "人事部",     external = false },
    finance    = { short = "1100", range_start = 1101, range_end = 1150,
                   name = "财务部",     external = false },
    admin      = { short = "1200", range_start = 1201, range_end = 1250,
                   name = "行政部",     external = false },
    management = { short = "1300", range_start = 1301, range_end = 1330,
                   name = "管理层",     external = false },

    -- ============ 业务前端部门（对外）============
    sales      = { short = "2000", range_start = 2001, range_end = 2400,
                   name = "销售部",     external = true  },
    service    = { short = "2500", range_start = 2501, range_end = 2800,
                   name = "售后服务部", external = true  },
    market     = { short = "2900", range_start = 2901, range_end = 2980,
                   name = "市场部",     external = true  },

    -- ============ 技术部门（对内）============
    rnd        = { short = "3000", range_start = 3001, range_end = 3200,
                   name = "研发部",     external = false },

    -- ============ 客服中心（24h人工服务台，对外）============
    support    = { short = "9000", range_start = 9001, range_end = 9050,
                   name = "人工服务台", external = true  },
}

-- ============================================================
-- 三、IVR 按键映射表 — IVR_MAP
-- ------------------------------------------------------------
-- 将客户按键（1~4）映射到对应的部门 key。
-- 严格对齐设计方案 3.2 节「按键路由规则」：
--   1 → 销售部、2 → 售后服务部、3 → 市场部、4 → 人工服务台
-- ============================================================
local IVR_MAP = {
    ["1"] = "sales",     -- 按键1 → 销售部（2000号段）
    ["2"] = "service",   -- 按键2 → 售后服务部（2500号段）
    ["3"] = "market",    -- 按键3 → 市场部（2900号段）
    ["4"] = "support",   -- 按键4 → 24小时人工服务台（9000号段）
}

-- ============================================================
-- 四、坐席状态表 — AGENT_STATUS
-- ------------------------------------------------------------
-- 当前为占位实现（空表 = 所有坐席视为在线）。
-- 后续对接 Redis 后，此表将从 Redis 实时同步坐席状态：
--   AGENT_STATUS["9001"] = { online = true, calls = 2, last_active = 123456789 }
--   AGENT_STATUS["9002"] = { online = false, calls = 0, last_active = 123450000 }
-- ============================================================
local AGENT_STATUS = {}

-- ============================================================
-- 五、工具函数：find_department(extension)
-- ------------------------------------------------------------
-- 根据分机号查找对应的部门。
--
-- 遍历 DEPARTMENTS 表，检查分机号是否落在某个部门的号段范围内。
-- 时间复杂度 O(n)，n=部门数（9个），性能足够。
--
-- @param extension  分机号字符串（如 "2005"）
-- @return dept_key  部门 key（如 "sales"），未匹配返回 nil
-- @return dept      部门配置表，未匹配返回 nil
--
-- 示例：
--   find_department("2005") → "sales", { short="2000", ... }
--   find_department("9999") → nil, nil
-- ============================================================
local function find_department(extension)
    -- 将字符串转为数字，方便号段范围比较
    local ext_num = tonumber(extension)
    if not ext_num then return nil end

    -- 遍历所有部门，检查分机号是否在号段范围内
    for dept_key, dept in pairs(DEPARTMENTS) do
        if ext_num >= dept.range_start and ext_num <= dept.range_end then
            return dept_key, dept
        end
    end
    -- 未匹配任何部门
    return nil
end

-- ============================================================
-- 六、工具函数：is_external_caller(caller_id)
-- ------------------------------------------------------------
-- 判断来电是否为外部来电。
--
-- 判断逻辑（按优先级）：
--   1. caller_id 为空或空字符串 → 外部（未知来电）
--   2. caller_id 以 "400" 开头 → 外部（400 总机呼入）
--   3. caller_id 不是纯数字或不在 1000~9999 范围 → 外部
--   4. caller_id 不在任何部门号段内 → 外部
--   5. 以上都不满足 → 内部（合法分机）
--
-- @param caller_id  主叫号码（C 程序传入）
-- @return true=外部来电（需 IVR 导航）, false=内部来电（分机直拨）
-- ============================================================
local function is_external_caller(caller_id)
    -- 规则1: 空身份 = 外部来电
    if not caller_id or caller_id == "" then
        return true
    end

    -- 规则2: 400 开头 = 外部总机呼入
    -- 匹配 "4001234567" 或 "400-123-4567" 开头
    if string.sub(caller_id, 1, 3) == "400" then
        return true
    end

    -- 规则3: 非 4 位数字分机格式 → 外部
    local caller_num = tonumber(caller_id)
    if not caller_num or caller_num < 1000 or caller_num > 9999 then
        return true
    end

    -- 规则4: 不在企业任何部门号段内 → 外部
    -- 即使格式像分机，但不在合法号段内也视为外部
    for _, dept in pairs(DEPARTMENTS) do
        if caller_num >= dept.range_start and caller_num <= dept.range_end then
            return false  -- 找到匹配：确认为内部员工
        end
    end

    -- 最终兜底：视为外部
    return true
end

-- ============================================================
-- 七、工具函数：make_response(code, target, department, description)
-- ------------------------------------------------------------
-- 构建标准路由响应 table，供 C 程序解析。
--
-- 返回的 table 会被 lua_vm_call_route() 逐字段提取到
-- route_response_t C 结构体：
--   { code = 0, target = "2000", department = "销售部",
--     description = "..." }
--       ↓  逐字段 lua_getfield 提取
--   C: route_response_t { .code=0, .target_extension="2000", ... }
--
-- @param code        路由结果码（对应 route_result_t 枚举）
--                        0=成功, 1=无效号码, 2=部门全忙, 4=人工兜底
-- @param target      目标分机/部门短号
-- @param department  目标部门名称（日志可读性）
-- @param description 路由过程描述（调试/日志用）
-- @return table      { code, target, department, description }
-- ============================================================
local function make_response(code, target, department, description)
    return {
        code = code,
        target = target or "",                -- nil 保护：缺省传空字符串
        department = department or "",
        description = description or ""
    }
end

-- ============================================================
-- 八、核心路由入口：route_call(caller_id, digits)
-- ------------------------------------------------------------
-- ★ 这是 C 程序调用的入口函数，是整个路由决策的总控。
--
-- 决策流程：
--   1. 调用 is_external_caller() 判断内外
--   2. 外部来电 → route_external_call()（IVR 导航 + 人工兜底）
--   3. 内部来电 → route_internal_call()（分机号匹配 + 权限校验）
--
-- @param caller_id  主叫号码（外部为 "4001234567"，内部为 "1001"）
-- @param digits     IVR 按键（"1"/"2"/"3"/"4"）或内部分机号（"2001"）
-- @return table     { code, target, department, description }
-- ============================================================
function route_call(caller_id, digits)
    -- 步骤1: 判断来电是外部还是内部
    local external = is_external_caller(caller_id)

    -- 步骤2: 根据来电类型分发到对应的路由函数
    if external then
        -- 外部来电 → 走 IVR 导航流程
        return route_external_call(caller_id, digits)
    end

    -- 内部来电 → 分机直拨流程
    return route_internal_call(caller_id, digits)
end

-- ============================================================
-- 九、外部来电路由：route_external_call(caller_id, digits)
-- ------------------------------------------------------------
-- 处理所有通过 400 总机接入的外部来电。
--
-- 路由优先级（从高到低）：
--   1. 超时未操作（digits 为空）→ 人工兜底（设计 3.2 节）
--   2. 部门全忙溢出（digits 含 ":full" 后缀）→ 人工兜底（设计 4.3 节）
--   3. IVR 按键匹配（1/2/3/4）→ 转接对应部门队列（设计 3.2 节）
--   4. 未匹配任何规则 → 人工兜底（设计 7.3 节）
--
-- 权限检查：只有 external = true 的部门才能被外部访问。
--   如果按 "2" 映射到 "service"，而 service.external = true → 通过
--   如果按了某个键映射到 external=false 的部门（当前设计不存在此情况）→ 兜底
--
-- @param caller_id  主叫号码（外部号码）
-- @param digits     IVR 按键
-- @return table     { code, target, department, description }
-- ============================================================
function route_external_call(caller_id, digits)
    -- 分支1: 超时未操作（digits 为空或 nil）
    -- 对应设计方案 3.2 节：「超时 10 秒未操作 → 自动转入人工服务台兜底」
    if not digits or digits == "" then
        return make_response(
            4,                          -- code: FALLBACK_AGENT
            FALLBACK_AGENT,             -- target: "9000"
            "人工服务台",
            "IVR超时未操作，自动转入24小时人工服务台"
        )
    end

    -- 分支2: 模拟部门坐席全满（digits 含 ":full" 后缀）
    -- 对应设计方案 4.3 节：「部门坐席全忙，排队溢出转入人工」
    -- 此分支用于 Demo 测试，生产环境将从 Redis 队列溢出事件触发
    if string.find(digits, ":full") then
        -- 从 "1:full" 中提取真实按键 "1"
        local real_digits = string.match(digits, "^(%d+)")
        local ivr_key = IVR_MAP[real_digits]     -- 查找部门 key
        local dept = DEPARTMENTS[ivr_key]         -- 获取部门配置
        local dept_name = dept and dept.name or "未知部门"
        return make_response(
            2,                          -- code: DEPT_FULL
            FALLBACK_AGENT,
            "人工服务台",
            dept_name .. "坐席全忙，溢出转入人工服务台"
        )
    end

    -- 分支3: IVR 按键匹配
    -- 对应设计方案 3.2 节「按键路由规则」
    local dept_key = IVR_MAP[digits]   -- 按键 → 部门 key（如 "1" → "sales"）
    if dept_key then
        local dept = DEPARTMENTS[dept_key]  -- 获取部门配置
        -- 权限检查：只有对外部门才能通过 IVR 接入
        if dept and dept.external then
            return make_response(
                0,                      -- code: SUCCESS
                dept.short,             -- target: 部门短号（如 "2000"）
                dept.name,
                "IVR按键[" .. digits .. "] → 转接" .. dept.name ..
                "队列（" .. dept.short .. "号段）"
            )
        end
    end

    -- 分支4: 未匹配任何规则 → 人工兜底
    -- 对应设计方案 7.3 节：「无按键、按键错误→统一转入人工台」
    return make_response(
        1,                              -- code: INVALID_DIGITS
        FALLBACK_AGENT,
        "人工服务台",
        "无效按键[" .. digits .. "]，转接24小时人工服务台"
    )
end

-- ============================================================
-- 十、内部员工通话路由：route_internal_call(caller_id, digits)
-- ------------------------------------------------------------
-- 处理企业内部分机之间的通话。
--
-- 路由流程：
--   1. 解析被叫号码是否为有效数字
--   2. 查找被叫分机所属部门
--   3. 查找主叫分机所属部门（用于日志描述）
--   4. 构建路由响应，返回目标分机
--
-- @param caller_id  主叫分机号（如 "1001"）
-- @param digits     被叫分机号（如 "2005"）
-- @return table     { code, target, department, description }
-- ============================================================
function route_internal_call(caller_id, digits)
    -- 步骤1: 解析主叫分机号（用于日志描述）
    local caller_ext = tonumber(caller_id)

    -- 步骤2: 校验被叫分机号是否为有效数字
    local callee_num = tonumber(digits)
    if not callee_num then
        return make_response(
            1,                          -- code: INVALID_DIGITS
            FALLBACK_AGENT,
            "人工服务台",
            "无效分机号[" .. tostring(digits) .. "]，无法接通"
        )
    end

    -- 步骤3: 查找被叫分机所属部门
    local callee_dept_key, callee_dept = find_department(digits)
    if not callee_dept then
        -- 被叫号码不在任何部门号段内
        return make_response(
            1,
            FALLBACK_AGENT,
            "人工服务台",
            "分机[" .. digits .. "]不在企业号段内，转接人工台"
        )
    end

    -- 步骤4: 查找主叫分机所属部门（用于生成描述信息）
    local caller_dept_key, caller_dept = find_department(caller_id)
    local caller_dept_name = caller_dept and caller_dept.name or "未知"

    -- 步骤5: 构建成功响应
    -- 内部互拨不需要权限检查（设计方案 5.2 节：内部员工可拨打所有分机）
    return make_response(
        0,                              -- code: SUCCESS
        digits,                         -- target: 被叫分机号
        callee_dept.name,
        "内部互拨: " .. caller_dept_name .. "(" .. caller_id .. ") → " ..
        callee_dept.name .. "(" .. digits .. ")"
    )
end

-- ============================================================
-- 十一、辅助接口函数
-- ------------------------------------------------------------
-- 以下函数供 C 程序通过 lua_vm_call_function() 调用，
-- 或供其他 Lua 脚本模块引用。
-- ============================================================

--
-- 获取 IVR 语音导航文案
-- 对应设计方案 3.2 节「语音播报文案」
-- C 侧调用：lua_vm_call_function(vm, "get_ivr_prompt", NULL)
-- @return string  IVR 欢迎语文本
--
function get_ivr_prompt()
    return "欢迎致电XX企业服务热线，按键选择对应服务：" ..
           "1-销售咨询，2-售后服务，3-市场合作，4-人工客服，" ..
           "其他按键返回首页，全天候人工服务为您保驾护航。"
end

--
-- 获取部门详细信息
-- C 侧调用：lua_vm_call_function(vm, "get_department_info", "s", "sales")
-- @param dept_key  部门 key（如 "sales"）
-- @return table    部门完整信息，不存在的部门返回 nil
--
function get_department_info(dept_key)
    local dept = DEPARTMENTS[dept_key]
    if not dept then
        return nil
    end
    -- 返回一个包含统计信息的扩展 table
    return {
        key         = dept_key,                   -- 部门 key
        short       = dept.short,                 -- 总机短号
        name        = dept.name,                  -- 中文名称
        range_start = dept.range_start,           -- 号段起始
        range_end   = dept.range_end,             -- 号段结束
        external    = dept.external,              -- 是否对外
        size        = dept.range_end - dept.range_start + 1,  -- 员工数
    }
end

-- ============================================================
-- 十二、脚本加载信息输出
-- ------------------------------------------------------------
-- 以下代码在脚本被 luaL_loadfile + lua_pcall 执行时运行。
-- 相当于初始化打印，帮助运维人员确认脚本版本和配置正确。
-- 输出会同时出现在终端和日志文件中。
-- ============================================================
print("[route.lua] 路由脚本已加载，版本: 1.0.0")
print("[route.lua] 企业总机: " .. EXTERNAL_GATEWAY)
print("[route.lua] 人工兜底: " .. FALLBACK_AGENT)
print("[route.lua] IVR超时: " .. IVR_TIMEOUT .. "秒")
print("[route.lua] 已注册部门: ")

-- 以表格形式打印所有部门信息
for key, dept in pairs(DEPARTMENTS) do
    -- 权限标识：对外 / 对内
    local access = dept.external and "对外" or "对内"
    print(string.format(
        "  [%s] %-10s | 短号: %-4s | 号段: %04d-%04d | %-4s | %d人",
        key, dept.name, dept.short,
        dept.range_start, dept.range_end,
        access,
        dept.range_end - dept.range_start + 1   -- 部门人数
    ))
end

print("[route.lua] route_call(caller_id, digits) 路由函数就绪")