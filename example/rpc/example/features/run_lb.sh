#!/bin/bash

# Switch to the script's directory so relative paths work
cd "$(dirname "$0")"

# 确保清理之前的进程
killall -9 server registry_server client 2>/dev/null

echo "=================================================="
echo "   RPC Features Demo: Load Balancing"
echo "=================================================="

# 启动 Registry Server (端口 8001)
# Assuming registry server is at ../../../registry/build/registry_server
echo "[1/4] Starting Registry Server on port 8001..."
../../../registry/build/registry_server > /dev/null 2>&1 &
sleep 2

# 启动 3 个服务器，模拟不同的性能（延迟）
# Server 1: 超快 (0ms) - Port 9001
# Server 2: 中等 (20ms) - Port 9002
# Server 3: 慢速 (50ms) - Port 9003
echo "[2/4] Starting 3 servers with different latencies..."
./build/server -p 9001 -d 0 &
./build/server -p 9002 -d 20 &
./build/server -p 9003 -d 50 &
sleep 2

echo ""
echo "--------------------------------------------------"
echo "Test 1: Latency Aware Load Balancing (LALB)"
echo "Expectation: 9001 > 9002 > 9003 (load distribution)"
echo "--------------------------------------------------"
./build/client -t 4 -n 100 -l lalb

echo ""
echo "--------------------------------------------------"
echo "Test 2: Round Robin Load Balancing (RR)"
echo "Expectation: 9001 ≈ 9002 ≈ 9003 (equal distribution)"
echo "--------------------------------------------------"
./build/client -t 4 -n 30 -l rr

echo ""
echo "--------------------------------------------------"
echo "Test 3: Consistent Hash Load Balancing (CH)"
echo "Expectation: Requests with same key hit same server"
echo "--------------------------------------------------"
./build/client -t 1 -n 10 -l ch

# 清理
echo ""
echo "[4/4] Cleaning up..."
killall -9 server registry_server 2>/dev/null
echo "Done."
