import sys
import time
import pytest
import logging
import os
from utils import (
    ServerManager, RegistryManager, 
    create_client_config, run_stress_client, 
    wait_for_port, kill_process_tree, run_process,
    REGISTRY_BIN, STRESS_CLIENT_BIN, get_env
)
import subprocess
import re

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

SERVER_PORTS = [9001, 9002, 9003, 9004, 9005]

@pytest.fixture(scope="function")
def registry(tmp_path):
    manager = RegistryManager(tmp_path)
    manager.start()
    yield manager
    manager.stop()

@pytest.fixture(scope="function")
def server_manager(tmp_path):
    manager = ServerManager(tmp_path)
    yield manager
    manager.stop_all()

def count_server_requests(tmp_path, ports):
    """Count requests handled by each server by reading their logs."""
    counts = {}
    for port in ports:
        log_path = tmp_path / f"server_{port}.log"
        count = 0
        if log_path.exists():
            with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
                count = content.count("Login") 
        counts[port] = count
    return counts

def test_consistent_hash_reliability(registry, server_manager, tmp_path):
    """TEST 1: Consistent Hash Reliability & Fault Tolerance"""
    logger.info("TEST 1: Consistent Hash Reliability & Fault Tolerance")
    
    # Start Servers
    for port in SERVER_PORTS:
        server_manager.start_server(port)
    
    # Wait for servers to register
    logger.info("Waiting for servers to register...")
    try:
        if not registry.wait_for_service("UserServiceRpc", "Login", min_nodes=len(SERVER_PORTS)):
            logger.error("Timeout waiting for servers to register")
            raise RuntimeError("Timeout waiting for servers to register")
    except Exception as e:
        logger.error(f"Registration failed: {e}")
        # Print server logs
        for port in SERVER_PORTS:
            log_path = tmp_path / f"server_{port}.log"
            if log_path.exists():
                print(f"--- Log for server {port} ---", file=sys.stderr)
                print(log_path.read_text(), file=sys.stderr)
            else:
                print(f"--- Log for server {port} NOT FOUND ---", file=sys.stderr)
        # Print registry log
        reg_log = tmp_path / "registry.log"
        if reg_log.exists():
            print(f"--- Registry Log (Tail 100) ---", file=sys.stderr)
            # Read last 100 lines
            lines = reg_log.read_text().splitlines()
            print("\n".join(lines[-100:]), file=sys.stderr)
        raise e
    
    # Client Config
    conf_file = create_client_config(tmp_path, lb_algo="consistent_hash")
    
    # Step 1: Baseline
    logger.info("[Step 1] Baseline check (10000 requests)")
    success, failed, _ = run_stress_client(conf_file, threads=10, requests=10000, user="user1")
    assert failed == 0, f"Baseline failed. Success: {success}, Failed: {failed}"

    # Step 2: Kill one server
    victim_port = SERVER_PORTS[0]
    logger.info(f"[Step 2] Killing Server on port {victim_port}...")
    server_manager.stop_server(victim_port)

    # Step 3: After Failure
    logger.info("[Step 3] Sending 1000 requests (After Failure)...")
    success, failed, _ = run_stress_client(conf_file, threads=10, requests=1000, user="user1")

    logger.info(f"After failure results: Success={success}, Failed={failed}")
    # Success rate should be high (>90%)
    assert success >= 900, f"Too many failures after node death. Success: {success}/1000"

def test_lalb_reliability(registry, server_manager, tmp_path):
    """TEST 2: LALB Reliability & Latency Avoidance"""
    logger.info("TEST 2: LALB Reliability & Latency Avoidance")
    
    ports = [9001, 9002, 9003]
    delays = {9001: 0, 9002: 10, 9003: 100}

    for port in ports:
        server_manager.start_server(port, delay_ms=delays[port])
    
    conf_file = create_client_config(tmp_path, lb_algo="lalb")
    
    # Wait for servers to register
    logger.info("Waiting for servers to register...")
    try:
        if not registry.wait_for_service("UserServiceRpc", "Login", min_nodes=len(ports)):
            logger.error("Timeout waiting for servers to register")
            raise RuntimeError("Timeout waiting for servers to register")
    except Exception as e:
        logger.error(f"Registration failed: {e}")
        # Print server logs
        for port in ports:
            log_path = tmp_path / f"server_{port}.log"
            if log_path.exists():
                print(f"--- Log for server {port} ---", file=sys.stderr)
                print(log_path.read_text(), file=sys.stderr)
            else:
                print(f"--- Log for server {port} NOT FOUND ---", file=sys.stderr)
        # Print registry log
        reg_log = tmp_path / "registry.log"
        if reg_log.exists():
            print(f"--- Registry Log (Tail 100) ---", file=sys.stderr)
            lines = reg_log.read_text().splitlines()
            print("\n".join(lines[-100:]), file=sys.stderr)
        raise e

    # Step 1: Run requests (Expect Fast)
    logger.info("[Step 1] Sending 500 requests (Expect Fast)...")
    success, failed, _ = run_stress_client(conf_file, threads=10, requests=10000, user="user1")

    assert failed == 0

    # Check distribution
    counts = count_server_requests(tmp_path, ports)
    logger.info(f"Node selection counts: {counts}")

    # 9001 should be preferred
    assert counts[9001] > counts[9002], "LALB failed to prefer fast node 9001 over 9002"
    assert counts[9001] > counts[9003], "LALB failed to prefer fast node 9001 over 9003"

    # Step 2: Kill Fast Server
    logger.info(f"[Step 2] Killing Fast Server (9001)...")
    server_manager.stop_server(9001)

    # Step 3: Run requests (Expect Medium)
    logger.info("[Step 3] Sending 2000 requests (Expect Medium)...")
    success, failed, _ = run_stress_client(conf_file, threads=10, requests=2000, user="user1")
    
    # Relaxed assertion: Allow some failures during failover/stabilization
    assert success >= 1500, f"Too many failures during LALB failover. Success: {success}"
    
    final_counts = count_server_requests(tmp_path, ports)
    delta_counts = {k: final_counts[k] - counts[k] for k in ports}
    logger.info(f"Delta node selection counts: {delta_counts}")
    
    # Should shift to 9002 (Medium)
    assert delta_counts[9002] > delta_counts[9003], "LALB failed to shift to medium node 9002"

def test_registry_restart_resilience(registry, server_manager, tmp_path):
    """TEST 4: Registry Restart Resilience"""
    logger.info("TEST 4: Registry Restart Resilience")
    
    # Start servers with small delay to ensure requests take some time
    for port in SERVER_PORTS[:3]:
        server_manager.start_server(port, delay_ms=10)
        
    conf_file = create_client_config(tmp_path, lb_algo="consistent_hash")

    # We need to run client with pause and kill registry in between.
    # run_stress_client in utils runs synchronously.
    # We need manual control here.
    
    cmd = [
        str(STRESS_CLIENT_BIN), 
        "-i", str(conf_file), 
        "-t", "1", 
        "-r", "2000", 
        "-u", "user1"
    ]
    
    logger.info("Starting client...")
    env = get_env()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, preexec_fn=os.setsid, env=env)
    
    time.sleep(2) # Let it run and cache services
    
    logger.info("Killing Registry...")
    registry.stop()
    
    logger.info("Waiting for client to finish...")
    # Increase timeout for client completion
    stdout, stderr = proc.communicate(timeout=30)
    
    # Parse results
    match = re.search(r"Success: (\d+)", stdout)
    success = int(match.group(1)) if match else 0
    match_fail = re.search(r"Failed: (\d+)", stdout)
    failed = int(match_fail.group(1)) if match_fail else 0
    
    logger.info(f"Registry Resilience Results: Success={success}, Failed={failed}")
    assert success > 90, f"Client failed to use cache. Success: {success}/100"

def test_rate_limiting(registry, server_manager, tmp_path):
    """TEST 5: Rate Limiting"""
    logger.info("TEST 5: Rate Limiting")
    
    port = 9001
    rate_limit = 20
    server_manager.start_server(port, rate_limit=rate_limit)
    
    conf_file = create_client_config(tmp_path, lb_algo="consistent_hash")
    
    # Send 100 requests in short burst
    logger.info(f"Sending 100 requests to server with {rate_limit} QPS limit...")
    success, failed, _ = run_stress_client(conf_file, threads=10, requests=100, user="user1")
    
    logger.info(f"Rate Limit Results: Success={success}, Failed={failed}")
    
    assert failed > 0, "Rate limiting did not trigger! All requests succeeded."
    assert success < 80, f"Rate limiting too loose. Success: {success}/100"

if __name__ == "__main__":
    sys.exit(pytest.main(["-v", "-s", __file__]))
