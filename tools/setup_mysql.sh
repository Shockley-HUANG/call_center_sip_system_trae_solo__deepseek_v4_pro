#!/bin/bash
sudo service mysql start 2>&1
sleep 2

sudo mysql <<'EOSQL'
CREATE DATABASE IF NOT EXISTS call_center DEFAULT CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'sip_user'@'127.0.0.1' IDENTIFIED BY 'sip_password';
GRANT ALL ON call_center.* TO 'sip_user'@'127.0.0.1';
FLUSH PRIVILEGES;
SELECT 'MySQL ready' AS status;
EOSQL
