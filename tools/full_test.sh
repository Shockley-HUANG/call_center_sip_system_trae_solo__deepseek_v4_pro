#!/bin/bash
# Step 1: Create database + user
sudo mysql <<'SQL'
CREATE DATABASE IF NOT EXISTS call_center DEFAULT CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'sip_user'@'127.0.0.1' IDENTIFIED BY 'sip_password';
GRANT ALL ON call_center.* TO 'sip_user'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
echo "[OK] Database + user created"

# Step 2: Clean + build + run call test
cd /mnt/d/Trae_Project/call_center_sip_system
make clean 2>&1 | tail -2
make 2>&1 | tail -3
echo "a" | LANG=zh_CN.UTF-8 timeout 10 build/sip_server --call-test 2>&1 | tail -30

# Step 3: Query all tables (10 rows each)
echo ""
echo "============================================"
echo "  departments (部门)"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT id, dept_key, dept_name, short_number, range_start, range_end, external_access FROM departments LIMIT 10;"

echo ""
echo "============================================"
echo "  extensions (员工分机) — 10/1210"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT id, extension, employee_name, department_id, is_active FROM extensions LIMIT 10;"
echo "  ..."
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT COUNT(*) AS total_extensions FROM extensions;"

echo ""
echo "============================================"
echo "  agents (坐席) — 10/830"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT id, extension, department_id, is_online, status, concurrent_calls, shift FROM agents LIMIT 10;"
echo "  ..."
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT COUNT(*) AS total_agents FROM agents;"

echo ""
echo "============================================"
echo "  call_records (通话记录) — 10/50"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT id, caller_number, callee_number, direction, result_code, LEFT(result_desc,40) AS result_desc, duration FROM call_records LIMIT 10;"
echo "  ..."
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT COUNT(*) AS total_calls, SUM(duration) AS total_duration_sec FROM call_records;"

echo ""
echo "============================================"
echo "  call_logs (呼叫日志)"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT COUNT(*) AS total_logs FROM call_logs;"

echo ""
echo "============================================"
echo "  voicemails (留言)"
echo "============================================"
mysql -u sip_user -psip_password -h 127.0.0.1 call_center -e \
  "SELECT COUNT(*) AS total_voicemails FROM voicemails;"

echo ""
echo "[DONE] All checks complete"
