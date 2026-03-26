#!/bin/bash

# Color codes
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Switch to the script's directory so relative paths work
cd "$(dirname "$0")"

RPC_ROOT=../../..
REGISTRY_BIN=../../../registry/build/registry_server
SERVER_BIN=./build/server
CLIENT_BIN=./build/client

function cleanup {
    echo -e "${RED}Cleaning up processes...${NC}"
    killall -9 server registry_server client 2>/dev/null
}

trap cleanup EXIT

echo -e "${BLUE}==================================================${NC}"
echo -e "${BLUE}       Muduo-X RPC Features Demo                  ${NC}"
echo -e "${BLUE}==================================================${NC}"

# 1. Start Registry
echo -e "${GREEN}[1/4] Starting Registry Server...${NC}"
if [ -f "$REGISTRY_BIN" ]; then
    $REGISTRY_BIN > /dev/null 2>&1 &
    REGISTRY_PID=$!
    echo "Registry started (PID $REGISTRY_PID)"
    sleep 2
else
    echo -e "${RED}Error: Registry binary not found at $REGISTRY_BIN${NC}"
    echo "Please build the registry server first."
    exit 1
fi

# 2. Load Balancing Demo
echo -e "\n${BLUE}==================================================${NC}"
echo -e "${BLUE}       Demo: Load Balancing (RR vs LALB)          ${NC}"
echo -e "${BLUE}==================================================${NC}"

echo -e "${GREEN}Starting 3 servers with different latencies:${NC}"
echo "  - Port 9001: 0ms (Fast)"
echo "  - Port 9002: 50ms (Medium)"
echo "  - Port 9003: 100ms (Slow)"

$SERVER_BIN -p 9001 -d 0 > /dev/null 2>&1 &
$SERVER_BIN -p 9002 -d 50 > /dev/null 2>&1 &
$SERVER_BIN -p 9003 -d 100 > /dev/null 2>&1 &
sleep 2

echo -e "\n${GREEN}>>> Running Client with Round Robin (RR)...${NC}"
echo "Expect requests to be distributed equally, regardless of latency."
$CLIENT_BIN -t 4 -n 20 -l rr
sleep 1

echo -e "\n${GREEN}>>> Running Client with Latency Aware Load Balancing (LALB)...${NC}"
echo "Expect requests to favor the fast node (9001)."
$CLIENT_BIN -t 4 -n 20 -l lalb

# 3. High Availability Demo
echo -e "\n${BLUE}==================================================${NC}"
echo -e "${BLUE}       Demo: High Availability (Error Handling)   ${NC}"
echo -e "${BLUE}==================================================${NC}"

echo -e "${GREEN}Stopping previous servers...${NC}"
killall -9 server
sleep 1

echo -e "${GREEN}Starting 1 Flaky Server (Port 9004) with 25% Error Rate...${NC}"
echo -e "${GREEN}Starting 1 Reliable Server (Port 9005)...${NC}"
$SERVER_BIN -p 9004 -e 25 > /dev/null 2>&1 &
$SERVER_BIN -p 9005 -d 0 > /dev/null 2>&1 &
sleep 2

echo -e "\n${GREEN}>>> Running Client (Verbose Mode)...${NC}"
echo "Watch for 'RPC Failed' logs and automatic retries/circuit breaking if enabled."
# Using random LB to hit both.
$CLIENT_BIN -t 1 -n 20 -l random -v

# 4. Tracing & Metrics Demo
echo -e "\n${BLUE}==================================================${NC}"
echo -e "${BLUE}       Demo: Tracing & Metrics                    ${NC}"
echo -e "${BLUE}==================================================${NC}"

echo "Check the verbose output above for:"
echo "  - TraceID: Unique ID for request chain"
echo "  - Metrics Dump: Prometheus-formatted metrics at the end"

echo -e "\n${GREEN}Demo Completed!${NC}"
echo "Log files are located in: $(pwd)"
ls -lh server*.log client*.log 2>/dev/null
# cleanup happens via trap
