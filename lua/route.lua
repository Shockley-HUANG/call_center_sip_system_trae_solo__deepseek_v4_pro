--
-- route.lua -- 企业400呼叫中心全场景商用路由脚本 (V2.0)
-- ============================================================
-- 迭代2：从基础路由升级为全场景商用路由能力
-- 覆盖：400IVR导航 / 内外双模路由 / 坐席状态管理 / 全忙溢出
--        / 排队策略 / 工作日时段判断 / 夜间兜底 / 无效按键容错
--        / 超时无操作兜底 / 留言提示 / 热配置
--
-- C <-> Lua 接口：
--   C 侧 route_call(caller_id, digits) -- 主入口（兼容旧版）
--   7个标准化接口供 C 层通过 lua_vm_route_dispatch() 调用
--
-- 热更新：修改后 lua_vm_reload_script() 即可生效，无需重启 C 服务
--

-- ============================================================
-- 一、可配置参数中心 -- ROUTE_CONFIG
-- ============================================================
local ROUTE_CONFIG = {
    -- IVR 超时（秒）
    ivr_timeout = 5,

    -- 外呼总机号码
    external_gateway = "400-123-4567",

    -- 人工兜底号码
    fallback_agent = "9000",

    -- 无效按键连续错误上限
    max_invalid_keys = 3,

    -- 排队最大人数
    max_queue_size = 10,

    -- 排队超时（秒）
    queue_timeout = 60,

    -- 工作日时段（24小时制）
    work_hours = {
        start_hour = 8,
        start_minute = 0,
        end_hour = 18,
        end_minute = 0,
    },

    -- 工作日（1=周一, 7=周日）
    work_days = { true, true, true, true, true, false, false },

    -- 夜间值班部门
    night_duty_dept = "support",

    -- 留言语音文件标识
    voicemail_prompt = "voicemail_no_agent",

    -- 溢出优先级（从高到低）
    overflow_priority = { "support", "sales", "service", "market" },
}

-- ============================================================
-- 二、部门资源配置 -- DEPARTMENTS
-- ============================================================
local DEPARTMENTS = {
    hr         = { short = "1000", range_start = 1001, range_end = 1050,
                   name = "人事部",     external = false, ivr_key = nil },
    finance    = { short = "1100", range_start = 1101, range_end = 1150,
                   name = "财务部",     external = false, ivr_key = nil },
    admin      = { short = "1200", range_start = 1201, range_end = 1250,
                   name = "行政部",     external = false, ivr_key = nil },
    management = { short = "1300", range_start = 1301, range_end = 1330,
                   name = "管理层",     external = false, ivr_key = nil },
    sales      = { short = "2000", range_start = 2001, range_end = 2400,
                   name = "销售部",     external = true,  ivr_key = "1" },
    service    = { short = "2500", range_start = 2501, range_end = 2800,
                   name = "售后服务部", external = true,  ivr_key = "2" },
    market     = { short = "2900", range_start = 2901, range_end = 2980,
                   name = "市场部",     external = true,  ivr_key = "3" },
    rnd        = { short = "3000", range_start = 3001, range_end = 3200,
                   name = "研发部",     external = false, ivr_key = nil },
    support    = { short = "9000", range_start = 9001, range_end = 9050,
                   name = "人工服务台", external = true,  ivr_key = "0" },
}

-- IVR 按键映射表（从 DEPARTMENTS 的 ivr_key 自动构建）
local IVR_MAP = {}
for k, v in pairs(DEPARTMENTS) do
    if v.ivr_key then
        IVR_MAP[v.ivr_key] = k
    end
end

-- ============================================================
-- 三、坐席状态管理 -- AGENT_STATUS
-- ============================================================
-- 状态：idle（空闲）/ talking（通话中）/ offline（离线）/ busy（忙碌）
-- 格式：{ state = "idle", calls = 0, last_active = timestamp, extension = "9001" }
-- 生产环境对接 Redis Hash 实时同步
local AGENT_STATUS = {}

-- 初始化所有坐席为空闲在线
local function init_agent_status()
    for dept_key, dept in pairs(DEPARTMENTS) do
        for ext = dept.range_start, dept.range_end do
            local ext_str = tostring(ext)
            AGENT_STATUS[ext_str] = {
                state = "idle",
                calls = 0,
                last_active = os.time(),
                extension = ext_str,
                department = dept_key,
            }
        end
    end
end

-- 获取部门所有坐席状态列表
local function get_dept_agents(dept_id)
    local dept = DEPARTMENTS[dept_id]
    if not dept then return {} end
    local agents = {}
    for ext = dept.range_start, dept.range_end do
        local status = AGENT_STATUS[tostring(ext)]
        if status then
            table.insert(agents, status)
        end
    end
    return agents
end

-- 获取部门空闲坐席数
local function count_idle_agents(dept_id)
    local count = 0
    local agents = get_dept_agents(dept_id)
    for _, a in ipairs(agents) do
        if a.state == "idle" then
            count = count + 1
        end
    end
    return count
end

-- 获取一个空闲坐席（轮询分配--按最后活跃时间升序）
local function pick_idle_agent(dept_id)
    local agents = get_dept_agents(dept_id)
    local idles = {}
    for _, a in ipairs(agents) do
        if a.state == "idle" then
            table.insert(idles, a)
        end
    end
    if #idles == 0 then return nil end
    -- 按最后活跃时间升序（最久未接听的优先分配）
    table.sort(idles, function(a, b) return a.last_active < b.last_active end)
    return idles[1]
end

-- 标记坐席状态
local function set_agent_state(extension, state)
    local agent = AGENT_STATUS[extension]
    if agent then
        agent.state = state
        agent.last_active = os.time()
        if state == "talking" then
            agent.calls = agent.calls + 1
        elseif state == "idle" then
            agent.calls = math.max(0, (agent.calls or 1) - 1)
        end
    end
end

-- ============================================================
-- 四、排队队列管理 -- QUEUE
-- ============================================================
-- 按部门维护：QUEUE["sales"] = { {caller, arrival_time}, ... }
local QUEUE = {}

local function queue_push(dept_id, caller_id)
    if not QUEUE[dept_id] then QUEUE[dept_id] = {} end
    local q = QUEUE[dept_id]
    if #q >= ROUTE_CONFIG.max_queue_size then
        return false -- 队列满
    end
    table.insert(q, { caller = caller_id, arrival = os.time() })
    return true
end

local function queue_pop(dept_id)
    local q = QUEUE[dept_id]
    if not q or #q == 0 then return nil end
    return table.remove(q, 1)
end

local function queue_size(dept_id)
    local q = QUEUE[dept_id]
    return q and #q or 0
end

-- 清理超时排队项
local function queue_cleanup()
    local now = os.time()
    for dept_id, q in pairs(QUEUE) do
        local i = 1
        while i <= #q do
            if now - q[i].arrival > ROUTE_CONFIG.queue_timeout then
                table.remove(q, i)
            else
                i = i + 1
            end
        end
    end
end

-- ============================================================
-- 五、时段判断 -- WORKTIME
-- ============================================================
-- 返回 true=工作时段, false=非工作时段
local function is_work_time()
    local cfg = ROUTE_CONFIG.work_hours
    local now = os.date("*t")
    -- 先判断星期
    local wday = now.wday -- 1=周日, 2=周一 ... 7=周六
    local day_idx = wday == 1 and 7 or (wday - 1) -- 转为 1=周一
    if not ROUTE_CONFIG.work_days[day_idx] then
        return false
    end
    -- 判断时间
    local now_mins = now.hour * 60 + now.min
    local start_mins = cfg.start_hour * 60 + cfg.start_minute
    local end_mins = cfg.end_hour * 60 + cfg.end_minute
    return now_mins >= start_mins and now_mins < end_mins
end

-- 获取当前时间描述
local function get_time_description()
    local now = os.date("*t")
    if is_work_time() then
        return string.format("工作日 %02d:%02d (工作时段)", now.hour, now.min)
    else
        return string.format("非工作日 %02d:%02d (夜间/休息时段)", now.hour, now.min)
    end
end

-- ============================================================
-- 六、服务日志
-- ============================================================
-- 呼叫日志缓冲区（环形），生产环境对接文件或数据库
local CALL_LOG = {}
local CALL_LOG_INDEX = 0
local CALL_LOG_MAX = 100

local function log_call(level, caller_id, event, detail)
    CALL_LOG_INDEX = CALL_LOG_INDEX % CALL_LOG_MAX + 1
    CALL_LOG[CALL_LOG_INDEX] = {
        time = os.time(),
        level = level or "INFO",
        caller = caller_id or "unknown",
        event = event or "",
        detail = detail or "",
    }
    print(string.format("[route][%s] caller=%s event=%s detail=%s",
        level, caller_id, event, detail))
end

local function get_recent_logs(n)
    n = n or 10
    local logs = {}
    local start = math.max(1, CALL_LOG_INDEX - n + 1)
    for i = start, CALL_LOG_INDEX do
        if CALL_LOG[i] then
            table.insert(logs, CALL_LOG[i])
        end
    end
    for i = 1, CALL_LOG_INDEX + n - CALL_LOG_MAX - 1 do
        if i <= CALL_LOG_MAX and CALL_LOG[i] then
            table.insert(logs, CALL_LOG[i])
        end
    end
    return logs
end

-- ============================================================
-- 七、通用工具
-- ============================================================
local function make_response(code, target, department, description)
    return {
        code = code,
        target = target or "",
        department = department or "",
        description = description or "",
    }
end

local function find_department(extension)
    local ext_num = tonumber(extension)
    if not ext_num then return nil end
    for dept_key, dept in pairs(DEPARTMENTS) do
        if ext_num >= dept.range_start and ext_num <= dept.range_end then
            return dept_key, dept
        end
    end
    return nil
end

local function is_external_caller(caller_id)
    if not caller_id or caller_id == "" then
        return true
    end
    if string.sub(caller_id, 1, 3) == "400" then
        return true
    end
    local caller_num = tonumber(caller_id)
    if not caller_num or caller_num < 1000 or caller_num > 9999 then
        return true
    end
    for _, dept in pairs(DEPARTMENTS) do
        if caller_num >= dept.range_start and caller_num <= dept.range_end then
            return false
        end
    end
    return true
end

-- 获取溢出目标部门
local function find_overflow_dept(exclude_dept)
    for _, dept_key in ipairs(ROUTE_CONFIG.overflow_priority) do
        if dept_key ~= exclude_dept then
            local idle_count = count_idle_agents(dept_key)
            if idle_count > 0 then
                return DEPARTMENTS[dept_key], dept_key
            end
        end
    end
    return nil, nil
end

-- ============================================================
-- 八、7大核心路由接口
-- ============================================================

-- [接口1] IVR 按键路由分发
-- @param phone     主叫号码（外部400来电）
-- @param key_input 用户按键（"1"/"2"/"3"/"0"）
-- @return route_response_t
function get_ivr_route(phone, key_input)
    log_call("INFO", phone, "get_ivr_route", "key=" .. (key_input or "nil"))

    -- 空输入 = 超时
    if not key_input or key_input == "" then
        return timeout_fallback(phone)
    end

    local dept_key = IVR_MAP[key_input]
    if not dept_key then
        return invalid_key_fallback(phone, key_input)
    end

    local dept = DEPARTMENTS[dept_key]
    if not dept or not dept.external then
        return make_response(
            4, -- FALLBACK_AGENT
            ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            "IVR按键[" .. key_input .. "] 部门不可对外访问，转接人工台"
        )
    end

    -- 检查部门坐席状态
    local status = check_agent_status(dept_key)
    if status.code == 2 then -- DEPT_FULL
        return overflow_route(dept_key, phone)
    elseif status.code == 3 then -- AGENT_OFFLINE
        return make_response(
            3, -- AGENT_OFFLINE
            ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            dept.name .. "无空闲坐席在线，转接人工台"
        )
    end

    -- 空闲坐席负载均衡
    local agent = pick_idle_agent(dept_key)
    local target_ext = agent and agent.extension or dept.short

    return make_response(
        0, -- SUCCESS
        target_ext,
        dept.name,
        "IVR按键[" .. key_input .. "] → 转接" .. dept.name .. "坐席(" .. target_ext .. ")"
    )
end

-- [接口2] 部门坐席状态检测
-- @param dept_id  部门 key（如 "sales"）
-- @return route_response_t (code=0有闲/2全忙/3全离线)
function check_agent_status(dept_id)
    queue_cleanup()
    local dept = DEPARTMENTS[dept_id]
    if not dept then
        return make_response(99, "", "", "部门[" .. tostring(dept_id) .. "]不存在")
    end

    local agents = get_dept_agents(dept_id)
    local total = #agents
    local idle = 0
    local talking = 0
    local offline = 0
    local busy = 0

    for _, a in ipairs(agents) do
        if a.state == "idle" then idle = idle + 1
        elseif a.state == "talking" then talking = talking + 1
        elseif a.state == "busy" then busy = busy + 1
        else offline = offline + 1
        end
    end

    local online = total - offline
    if online == 0 then
        return make_response(
            3, -- AGENT_OFFLINE
            dept.short,
            dept.name,
            string.format("%s 全部离线 (总%d/在线0/空闲0)",
                dept.name, total)
        )
    end

    if idle == 0 then
        return make_response(
            2, -- DEPT_FULL
            dept.short,
            dept.name,
            string.format("%s 坐席全忙 (总%d/在线%d/通话%d/忙碌%d)",
                dept.name, total, online, talking, busy)
        )
    end

    return make_response(
        0, -- SUCCESS
        dept.short,
        dept.name,
        string.format("%s 有空闲坐席 (总%d/在线%d/空闲%d/通话%d)",
            dept.name, total, online, idle, talking)
    )
end

-- [接口3] 坐席全忙溢出路由
-- @param dept_id        全忙部门 key
-- @param caller_id      主叫号码（可选，用于排队）
-- @return route_response_t
function overflow_route(dept_id, caller_id)
    caller_id = caller_id or ""
    local dept = DEPARTMENTS[dept_id]
    local dept_name = dept and dept.name or tostring(dept_id)

    log_call("WARN", caller_id, "overflow", "部门[" .. dept_name .. "]全忙溢出")

    -- 策略1：溢出到公共人工组
    local idle_support = count_idle_agents("support")
    if idle_support > 0 then
        local agent = pick_idle_agent("support")
        return make_response(
            4, -- FALLBACK_AGENT
            agent and agent.extension or ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            dept_name .. "坐席全忙，溢出转入人工服务台坐席"
        )
    end

    -- 策略2：按优先级溢出到其他对外部门
    local overflow_dept, ok = find_overflow_dept(dept_id)
    if overflow_dept and ok then
        local agent = pick_idle_agent(ok)
        return make_response(
            4, -- FALLBACK_AGENT
            agent and agent.extension or overflow_dept.short,
            overflow_dept.name,
            dept_name .. "坐席全忙，优先溢出至" .. overflow_dept.name
        )
    end

    -- 策略3：所有对外部门全忙 -- 进入排队
    if queue_push(dept_id, caller_id) then
        local pos = queue_size(dept_id)
        return make_response(
            5, -- QUEUED
            dept.short,
            dept_name,
            string.format("所有部门全忙，已进入排队队列 (位置:%d/%d)，预计等待%d秒",
                pos, ROUTE_CONFIG.max_queue_size, pos * 30)
        )
    end

    -- 策略4：排队也满了 -- 留言
    log_call("ERROR", caller_id, "queue_full", "排队已满")
    return make_response(
        7, -- VOICEMAIL
        "",
        dept_name,
        "当前排队人数已满，请稍后再拨或留言，我们将尽快回电"
    )
end

-- [接口4] 时段判断+夜间兜底路由
-- @return route_response_t
function time_judge_route()
    if is_work_time() then
        return make_response(
            0, -- SUCCESS
            "",
            "",
            "工作日时段，使用标准路由策略"
        )
    end

    local time_desc = get_time_description()
    -- 非工作时段 → 检查值班人工
    local night_dept = ROUTE_CONFIG.night_duty_dept
    local agents = get_dept_agents(night_dept)
    local has_duty = false
    for _, a in ipairs(agents) do
        if a.state == "idle" or a.state == "talking" then
            has_duty = true
            break
        end
    end

    if has_duty then
        local agent = pick_idle_agent(night_dept)
        if agent then
            return make_response(
                6, -- NIGHT_MODE
                agent.extension,
                DEPARTMENTS[night_dept].name,
                time_desc .. " 夜间模式，转接值班人工坐席(" .. agent.extension .. ")"
            )
        end
        return make_response(
            6, -- NIGHT_MODE
            ROUTE_CONFIG.fallback_agent,
            DEPARTMENTS[night_dept].name,
            time_desc .. " 夜间模式，值班坐席全忙，请稍候"
        )
    end

    -- 无值班坐席 → 留言提示
    log_call("WARN", "system", "no_night_duty", "夜间无值班坐席")
    return make_response(
        7, -- VOICEMAIL
        "",
        "",
        time_desc .. " 当前为非工作时段，暂无值班坐席，请留言，工作时间将尽快回复"
    )
end

-- [接口5] 无效按键容错兜底
-- @param caller_id  主叫号码
-- @param key_input  无效按键值
-- @return route_response_t
-- 全局累计错误计数（按 caller_id）
local INVALID_KEY_COUNT = {}

function invalid_key_fallback(caller_id, key_input)
    caller_id = caller_id or "unknown"
    key_input = key_input or "nil"

    INVALID_KEY_COUNT[caller_id] = (INVALID_KEY_COUNT[caller_id] or 0) + 1
    local err_count = INVALID_KEY_COUNT[caller_id]
    local max_err = ROUTE_CONFIG.max_invalid_keys

    log_call("WARN", caller_id, "invalid_key",
        "key=" .. key_input .. " count=" .. err_count .. "/" .. max_err)

    if err_count >= max_err then
        -- 超限 → 转人工
        INVALID_KEY_COUNT[caller_id] = 0 -- 重置计数
        local agent = pick_idle_agent("support")
        return make_response(
            4, -- FALLBACK_AGENT
            agent and agent.extension or ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            string.format("连续%d次无效按键(最后:[%s])，自动转接人工坐席",
                err_count, key_input)
        )
    end

    -- 未超限 → 重新引导
    return make_response(
        9, -- INVALID_KEY_RETRY
        "",
        "",
        string.format("按键[%s]无效(第%d/%d次)，请重新选择：" ..
            "1-销售咨询 2-售后服务 3-市场合作 0-人工客服",
            key_input, err_count, max_err)
    )
end

-- [接口6] 超时无操作兜底
-- @param caller_id  主叫号码
-- @return route_response_t
local TIMEOUT_RETRY_COUNT = {}

function timeout_fallback(caller_id)
    caller_id = caller_id or "unknown"

    TIMEOUT_RETRY_COUNT[caller_id] = (TIMEOUT_RETRY_COUNT[caller_id] or 0) + 1
    local retry = TIMEOUT_RETRY_COUNT[caller_id]

    log_call("INFO", caller_id, "timeout", "retry=" .. retry)

    if retry >= 2 then
        -- 二次超时 → 人工兜底
        TIMEOUT_RETRY_COUNT[caller_id] = 0
        local agent = pick_idle_agent("support")
        return make_response(
            4, -- FALLBACK_AGENT
            agent and agent.extension or ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            "连续" .. retry .. "次超时无操作，自动转入人工坐席"
        )
    end

    -- 首次超时 → 重试引导
    return make_response(
        8, -- TIMEOUT_RETRY
        "",
        "",
        string.format("超时未操作(第%d次)，%d秒后重试引导..." ..
            "请按键选择：1-销售咨询 2-售后服务 3-市场合作 0-人工客服",
            retry, ROUTE_CONFIG.ivr_timeout)
    )
end

-- [接口7] 内部分机互呼路由
-- @param caller  主叫分机号
-- @param callee  被叫分机号
-- @return route_response_t
function dept_internal_call_route(caller, callee)
    log_call("INFO", caller, "internal_call", "to " .. callee)

    -- 校验主叫
    local caller_dept_key, caller_dept = find_department(caller)
    if not caller_dept then
        return make_response(
            1, -- INVALID_DIGITS
            ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            "主叫分机[" .. caller .. "]无效，不在企业号段内"
        )
    end

    -- 校验被叫
    local callee_num = tonumber(callee)
    if not callee_num or callee_num < 1000 or callee_num > 9999 then
        return make_response(
            1, -- INVALID_DIGITS
            ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            "被叫号码[" .. tostring(callee) .. "]格式无效"
        )
    end

    local callee_dept_key, callee_dept = find_department(callee)
    if not callee_dept then
        return make_response(
            1,
            ROUTE_CONFIG.fallback_agent,
            "人工服务台",
            "被叫分机[" .. callee .. "]不在企业号段内，转接人工台"
        )
    end

    -- 检查被叫坐席在线状态
    local callee_status = AGENT_STATUS[callee]
    if callee_status and callee_status.state == "offline" then
        return make_response(
            3, -- AGENT_OFFLINE
            callee_dept.short,
            callee_dept.name,
            "被叫分机[" .. callee .. "]当前离线，请稍后再拨"
        )
    end

    -- 坐席通话中
    if callee_status and callee_status.state == "talking" then
        return make_response(
            2, -- DEPT_FULL (被叫正忙)
            callee,
            callee_dept.name,
            "被叫分机[" .. callee .. "]正在通话中，请稍后再拨或留言"
        )
    end

    -- 成功：直连被叫
    return make_response(
        0, -- SUCCESS
        callee,
        callee_dept.name,
        "内部互拨: " .. caller_dept.name .. "(" .. caller .. ") → " ..
        callee_dept.name .. "(" .. callee .. ")"
    )
end

-- ============================================================
-- 九、主入口（兼容旧版 C 调用）
-- ============================================================
function route_call(caller_id, digits)
    local external = is_external_caller(caller_id)

    if external then
        -- 先做时段判断
        local time_result = time_judge_route()
        if time_result.code == 7 then
            -- VOICEMAIL：非工作时段无值班，直接返回留言
            return make_response(7, "", "",
                get_time_description() .. " 非工作时段暂无人工服务，" ..
                time_result.description)
        end
        if time_result.code == 6 then
            -- NIGHT_MODE：夜间走值班路由
            return time_result
        end

        -- 工作时段：走 IVR
        return get_ivr_route(caller_id, digits)
    end

    -- 内部来电 → 分机直拨
    return dept_internal_call_route(caller_id, digits)
end

-- ============================================================
-- 十、辅助接口（供 C 程序 lua_vm_call_function 调用）
-- ============================================================

function get_ivr_prompt()
    return "欢迎致电XX企业服务热线，按键选择对应服务：" ..
           "1-销售咨询，2-售后服务，3-市场合作，0-人工客服，" ..
           "其他按键返回首页，全天候人工服务为您保驾护航。"
end

function get_department_info(dept_key)
    local dept = DEPARTMENTS[dept_key]
    if not dept then return nil end
    local idle = count_idle_agents(dept_key)
    return {
        key = dept_key,
        short = dept.short,
        name = dept.name,
        range_start = dept.range_start,
        range_end = dept.range_end,
        external = dept.external,
        ivr_key = dept.ivr_key,
        size = dept.range_end - dept.range_start + 1,
        idle_agents = idle,
    }
end

-- 获取所有对外部门 IVR 菜单
function get_ivr_menu()
    local menu = {}
    for k, v in pairs(DEPARTMENTS) do
        if v.external and v.ivr_key then
            table.insert(menu, {
                key = v.ivr_key,
                name = v.name,
                dept = k,
            })
        end
    end
    table.sort(menu, function(a, b) return a.key < b.key end)
    return menu
end

-- 获取当前系统工作时段状态
function get_work_time_status()
    local now = os.date("*t")
    return {
        is_work_time = is_work_time(),
        time_desc = get_time_description(),
        current_hour = now.hour,
        current_minute = now.min,
        work_start = string.format("%02d:%02d",
            ROUTE_CONFIG.work_hours.start_hour,
            ROUTE_CONFIG.work_hours.start_minute),
        work_end = string.format("%02d:%02d",
            ROUTE_CONFIG.work_hours.end_hour,
            ROUTE_CONFIG.work_hours.end_minute),
    }
end

-- 获取系统配置
function get_route_config()
    return {
        ivr_timeout = ROUTE_CONFIG.ivr_timeout,
        external_gateway = ROUTE_CONFIG.external_gateway,
        fallback_agent = ROUTE_CONFIG.fallback_agent,
        max_invalid_keys = ROUTE_CONFIG.max_invalid_keys,
        max_queue_size = ROUTE_CONFIG.max_queue_size,
        queue_timeout = ROUTE_CONFIG.queue_timeout,
        work_hours = ROUTE_CONFIG.work_hours,
        night_duty_dept = ROUTE_CONFIG.night_duty_dept,
    }
end

-- 更新运行时配置（热更新）
function update_route_config(new_config)
    if not new_config then return false end
    for k, v in pairs(new_config) do
        if ROUTE_CONFIG[k] ~= nil then
            ROUTE_CONFIG[k] = v
        end
    end
    log_call("INFO", "system", "config_update", "运行时配置已更新")
    return true
end

-- 获取最近呼叫日志
function get_call_logs(n)
    return get_recent_logs(n or 20)
end

-- 获取排队状态
function get_queue_status()
    local status = {}
    for dept_id, q in pairs(QUEUE) do
        status[dept_id] = {
            size = #q,
            max = ROUTE_CONFIG.max_queue_size,
        }
    end
    return status
end

-- 重置无效按键计数（通话结束调用）
function reset_invalid_key_count(caller_id)
    INVALID_KEY_COUNT[caller_id] = 0
    TIMEOUT_RETRY_COUNT[caller_id] = 0
end

-- ============================================================
-- 十一、脚本加载输出
-- ============================================================
init_agent_status()

print("[route.lua] ══════════════════════════════════════════════")
print("[route.lua]  企业400呼叫中心路由脚本 V2.0 已加载")
print("[route.lua]  迭代2 - 全场景商用路由能力")
print("[route.lua] ══════════════════════════════════════════════")
print(string.format("[route.lua]  外呼总机: %s", ROUTE_CONFIG.external_gateway))
print(string.format("[route.lua]  人工兜底: %s", ROUTE_CONFIG.fallback_agent))
print(string.format("[route.lua]  IVR超时: %ds | 无效按键上限: %d | 排队上限: %d",
    ROUTE_CONFIG.ivr_timeout, ROUTE_CONFIG.max_invalid_keys,
    ROUTE_CONFIG.max_queue_size))
print(string.format("[route.lua]  工作时段: %02d:%02d - %02d:%02d (周一至周五)",
    ROUTE_CONFIG.work_hours.start_hour, ROUTE_CONFIG.work_hours.start_minute,
    ROUTE_CONFIG.work_hours.end_hour, ROUTE_CONFIG.work_hours.end_minute))
print(string.format("[route.lua]  当前时间: %s", get_time_description()))
print("[route.lua]  已注册部门及坐席:")
for key, dept in pairs(DEPARTMENTS) do
    local access = dept.external and "对外" or "对内"
    local ivr_info = dept.ivr_key and ("IVR:" .. dept.ivr_key) or "---"
    local idle = count_idle_agents(key)
    print(string.format("    [%s] %-8s | 短号:%-4s | %-4s | %04d-%04d | %d人(闲%d)",
        key, dept.name, dept.short, access .. "/" .. ivr_info,
        dept.range_start, dept.range_end,
        dept.range_end - dept.range_start + 1, idle))
end
print("[route.lua]  7个标准化路由接口已就绪:")
print("[route.lua]    1. get_ivr_route          2. check_agent_status")
print("[route.lua]    3. overflow_route         4. time_judge_route")
print("[route.lua]    5. invalid_key_fallback   6. timeout_fallback")
print("[route.lua]    7. dept_internal_call_route")
print("[route.lua] ══════════════════════════════════════════════")
