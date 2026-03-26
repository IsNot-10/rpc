#!/bin/bash

# Switch to the script's directory so relative paths work
cd "$(dirname "$0")"

# 确保清理之前的进程
killall -9 server registry_server 2>/dev/null

# 启动 Registry Server
echo "Starting Registry Server on port 8001..."
../../../registry/build/registry_server > /dev/null 2>&1 &
sleep 2

# 启动 1 个服务器，限流 10 QPS
echo "Starting 1 server with Rate Limit = 10 QPS..."
./build/server -p 9001 -l 10 &

sleep 2

echo "=================================================="
echo "Testing Rate Limiting..."
echo "Sending 50 requests with 4 threads (High Concurrency)."
echo "Expect QPS to be around 10, or many failures/timeouts."
echo "=================================================="
./build/client -t 4 -n 20 -l random

# 清理
killall -9 server registry_server
