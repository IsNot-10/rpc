#!/bin/bash

# Switch to the script's directory so relative paths work
cd "$(dirname "$0")"

killall -9 server registry_server client 2>/dev/null

echo "=================================================="
echo "   RPC Features Demo: High Availability"
echo "=================================================="

echo "[1/4] Starting Registry..."
../../../registry/build/registry_server > /dev/null 2>&1 &
sleep 2

echo ""
echo "--------------------------------------------------"
echo "Test 1: Backup Request (Latency Hedging)"
echo "Scenario: Server 9001 has 50ms delay. Backup timeout is 10ms."
echo "Expectation: Client sends request to 9001, waits 10ms, then sends to 9002 (0ms)."
echo "             Total latency should be ~10ms + network (much less than 50ms)."
echo "--------------------------------------------------"

# Start servers
./build/server -p 9001 -d 50 &
./build/server -p 9002 -d 0 &
sleep 2

# Run client with backup request = 10ms
# We use Round Robin to ensure we hit both servers
./build/client -t 1 -n 20 -b 10 -l rr

killall -9 server
sleep 1

echo ""
echo "--------------------------------------------------"
echo "Test 2: Circuit Breaker"
echo "Scenario: Server 9003 has 80% error rate. Server 9004 is healthy."
echo "Expectation: Circuit Breaker should isolate 9003."
echo "             Majority of requests should go to 9004."
echo "--------------------------------------------------"

./build/server -p 9003 -e 80 -c 10000 &
./build/server -p 9004 -e 0 &
sleep 2

# Run client with many requests to trigger CB
# We use Random LB to randomly pick nodes, relying on CB to reject the bad one
./build/client -t 1 -n 200 -l random

echo ""
echo "[4/4] Cleaning up..."
killall -9 server registry_server 2>/dev/null
echo "Done."
