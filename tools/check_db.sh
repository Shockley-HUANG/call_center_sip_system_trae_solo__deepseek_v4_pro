#!/bin/bash
MYSQL="mysql -u sip_user -psip_password -h 127.0.0.1 call_center"

echo "=== departments (部门) ==="
$MYSQL -e "SELECT id, dept_key, dept_name, short_number, range_start, range_end FROM departments LIMIT 10;"

echo ""
echo "=== extensions (员工分机) ==="
$MYSQL -e "SELECT id, extension, employee_name, department_id FROM extensions LIMIT 10;"
$MYSQL -e "SELECT COUNT(*) AS total FROM extensions;"

echo ""
echo "=== agents (坐席) ==="
$MYSQL -e "SELECT id, extension, department_id, is_online, status, concurrent_calls, shift FROM agents LIMIT 10;"
$MYSQL -e "SELECT COUNT(*) AS total FROM agents;"

echo ""
echo "=== call_records (通话记录) ==="
$MYSQL -e "SELECT id, caller_number, callee_number, direction, result_code, LEFT(result_desc,40), duration FROM call_records LIMIT 10;"
$MYSQL -e "SELECT COUNT(*) AS total, SUM(duration) AS total_sec FROM call_records;"

echo ""
echo "=== call_logs ==="
$MYSQL -e "SELECT COUNT(*) AS total FROM call_logs;"

echo ""
echo "=== voicemails ==="
$MYSQL -e "SELECT COUNT(*) AS total FROM voicemails;"
