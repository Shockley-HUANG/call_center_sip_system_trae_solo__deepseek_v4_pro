-- ============================================================
--  Call Center SIP System - MySQL Database Schema
--  Version: 1.0.0
--  企业呼叫中心专属数据库结构设计
-- ============================================================

CREATE DATABASE IF NOT EXISTS call_center
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE call_center;

-- ============================================================
--  1. departments — 部门配置表
--  存储企业组织架构，对应 conf/sip_server.conf 部门配置段
-- ============================================================
CREATE TABLE IF NOT EXISTS departments (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    dept_key        VARCHAR(32)  NOT NULL UNIQUE COMMENT '部门标识键 (如 sales, hr)',
    dept_name       VARCHAR(64)  NOT NULL COMMENT '部门中文名称',
    short_number    VARCHAR(8)   NOT NULL COMMENT '部门总机短号 (如 1000)',
    range_start     VARCHAR(16)  NOT NULL COMMENT '分机号段起始',
    range_end       VARCHAR(16)  NOT NULL COMMENT '分机号段结束',
    external_access TINYINT(1)   DEFAULT 0 COMMENT '是否允许外线呼入',
    min_agents_per_shift INT     DEFAULT 1 COMMENT '每班最低坐席数',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_dept_key (dept_key)
) ENGINE=InnoDB COMMENT='部门配置表';

-- ============================================================
--  2. extensions — 员工分机信息表
--  记录每位员工的分机号、姓名、归属部门
-- ============================================================
CREATE TABLE IF NOT EXISTS extensions (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    extension       VARCHAR(16)  NOT NULL UNIQUE COMMENT '分机号 (如 1001)',
    employee_name   VARCHAR(64)  NOT NULL COMMENT '员工姓名',
    department_id   INT          NOT NULL COMMENT '所属部门ID',
    is_active       TINYINT(1)   DEFAULT 1 COMMENT '分机是否启用',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_department (department_id),
    INDEX idx_extension (extension),
    FOREIGN KEY (department_id) REFERENCES departments(id) ON DELETE CASCADE
) ENGINE=InnoDB COMMENT='员工分机信息表';

-- ============================================================
--  3. agents — 坐席信息表
--  实时坐席状态，对接 Redis Hash 高速缓存
-- ============================================================
CREATE TABLE IF NOT EXISTS agents (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    extension       VARCHAR(16)  NOT NULL UNIQUE COMMENT '坐席分机号',
    department_id   INT          NOT NULL COMMENT '所属部门ID',
    is_online       TINYINT(1)   DEFAULT 0 COMMENT '在线状态 (1=在线 0=离线)',
    status          ENUM('idle','busy','ringing','hold','offline')
                                  DEFAULT 'offline' COMMENT '当前工作状态',
    concurrent_calls INT         DEFAULT 0 COMMENT '当前并发通话数',
    last_active     TIMESTAMP    NULL COMMENT '最后活跃时间',
    shift           VARCHAR(8)   DEFAULT NULL COMMENT '值班班次 (A/B/C)',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_department (department_id),
    INDEX idx_status (status),
    INDEX idx_extension (extension),
    FOREIGN KEY (department_id) REFERENCES departments(id) ON DELETE CASCADE
) ENGINE=InnoDB COMMENT='坐席信息表';

-- ============================================================
--  4. call_records — 通话记录表
--  每通电话的完整生命周期记录，用于话单统计与分析
-- ============================================================
CREATE TABLE IF NOT EXISTS call_records (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    call_id         VARCHAR(256) NOT NULL COMMENT 'SIP Call-ID',
    caller_number   VARCHAR(16)  NOT NULL COMMENT '主叫号码',
    callee_number   VARCHAR(16)  NOT NULL COMMENT '被叫号码',
    direction       ENUM('inbound','outbound','internal')
                                  NOT NULL COMMENT '通话方向',
    department      VARCHAR(64)  DEFAULT NULL COMMENT '归属部门',
    start_time      DATETIME     NOT NULL COMMENT '通话发起时间',
    answer_time     DATETIME     DEFAULT NULL COMMENT '接听时间',
    end_time        DATETIME     DEFAULT NULL COMMENT '挂断时间',
    duration        INT          DEFAULT 0 COMMENT '通话时长(秒)',
    result_code     INT          DEFAULT 0 COMMENT '路由结果码',
    result_desc     VARCHAR(128) DEFAULT NULL COMMENT '结果描述',
    sip_status      INT          DEFAULT 0 COMMENT 'SIP最终状态码',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_call_id (call_id),
    INDEX idx_caller (caller_number),
    INDEX idx_callee (callee_number),
    INDEX idx_start_time (start_time),
    INDEX idx_department (department),
    INDEX idx_direction (direction)
) ENGINE=InnoDB COMMENT='通话记录表';

-- ============================================================
--  5. call_logs — 呼叫日志表
--  呼叫过程中的关键事件日志，低频写入
-- ============================================================
CREATE TABLE IF NOT EXISTS call_logs (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    call_id         VARCHAR(256) DEFAULT NULL COMMENT '关联Call-ID',
    log_level       ENUM('DEBUG','INFO','WARN','ERROR')
                                  DEFAULT 'INFO' COMMENT '日志级别',
    event_type      VARCHAR(64)  NOT NULL COMMENT '事件类型 (如 ROUTE_START/ROUTE_END)',
    message         TEXT         DEFAULT NULL COMMENT '日志内容',
    caller_number   VARCHAR(16)  DEFAULT NULL COMMENT '主叫号码',
    callee_number   VARCHAR(16)  DEFAULT NULL COMMENT '被叫号码',
    department      VARCHAR(64)  DEFAULT NULL COMMENT '归属部门',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_call_id (call_id),
    INDEX idx_created (created_at),
    INDEX idx_event_type (event_type)
) ENGINE=InnoDB COMMENT='呼叫日志表';

-- ============================================================
--  6. voicemails — 用户留言表
--  夜间/全忙场景下的留言记录
-- ============================================================
CREATE TABLE IF NOT EXISTS voicemails (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    caller_number   VARCHAR(16)  NOT NULL COMMENT '主叫号码',
    department      VARCHAR(64)  DEFAULT NULL COMMENT '目标部门',
    message_text    TEXT         DEFAULT NULL COMMENT '留言文本(语音转写)',
    file_path       VARCHAR(512) DEFAULT NULL COMMENT '语音文件路径',
    duration        INT          DEFAULT 0 COMMENT '语音时长(秒)',
    is_read         TINYINT(1)   DEFAULT 0 COMMENT '是否已读',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_caller (caller_number),
    INDEX idx_created (created_at),
    INDEX idx_is_read (is_read)
) ENGINE=InnoDB COMMENT='用户留言表';

-- ============================================================
--  初始数据：部门配置 (与 conf/sip_server.conf 同步)
-- ============================================================
INSERT INTO departments (dept_key, dept_name, short_number, range_start, range_end, external_access) VALUES
    ('hr',         '人事部',     '1000', '1001', '1050', 0),
    ('finance',    '财务部',     '1100', '1101', '1150', 0),
    ('admin',      '行政部',     '1200', '1201', '1250', 0),
    ('management', '管理层',     '1300', '1301', '1330', 0),
    ('sales',      '销售部',     '2000', '2001', '2400', 1),
    ('service',    '售后服务部', '2500', '2501', '2800', 1),
    ('market',     '市场部',     '2900', '2901', '2980', 1),
    ('rnd',        '研发部',     '3000', '3001', '3200', 0),
    ('support',    '人工服务台', '9000', '9001', '9050', 1);
