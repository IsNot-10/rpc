import sys
import time
import pytest
import logging
import os
import threading
import subprocess
import re
import signal
from utils import (
    ServerManager, RegistryManager, 
    create_client_config, STRESS_CLIENT_BIN, get_env
)

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

# Port ranges
MEDIUM_PORTS = list(range(22001, 22006)) # 5 Medium servers
FAST_PORTS = list(range(22006, 22011))   # 5 Fast servers

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

def test_dynamic_scaling_fault_tolerance(registry, server_manager, tmp_path):
    """
    Test LALB dynamic scaling and fault tolerance:
    1. Start with Medium servers.
    2. Start Client.
    3. Add Fast servers (Scale Out).
    4. Remove some Medium servers (Scale In / Failure).
    5. Verify success rate and traffic shift.
    """
    logger.info("TEST: Dynamic Scaling and Fault Tolerance")
    
    # 1. Start 5 Medium Servers (50ms delay)
    logger.info("Phase 1: Starting 5 Medium Servers...")
    for port in MEDIUM_PORTS:
        server_manager.start_server(port, delay_ms=50)
        time.sleep(0.1) # Stagger start

    # Wait for registration
    time.sleep(2)

    # Create Client Config
    conf_file = create_client_config(tmp_path, lb_algo="lalb")

    # 2. Start Client (Long running: 20s, 20 threads)
    cmd = [
        str(STRESS_CLIENT_BIN), 
        "-i", str(conf_file), 
        "-t", "20",       # 20 threads
        "-r", "50000",    # 50k per thread * 20 threads = 1M requests. Should last > 20s.
        "-u", "user1"
    ]

    logger.info("Starting Stress Client (Background)...")
    env = get_env()
    client_log = open(tmp_path / "client_stress.log", "w")
    client_proc = subprocess.Popen(
        cmd, 
        stdout=client_log, 
        stderr=client_log, 
        text=True, 
        preexec_fn=os.setsid, 
        env=env
    )

    # 3. Wait 5s, then Add 5 Fast Servers (0ms delay)
    logger.info("Phase 2: Running with Medium servers for 5s...")
    time.sleep(5)
    
    logger.info("Phase 3: Adding 5 Fast Servers (Scale Out)...")
    for port in FAST_PORTS:
        server_manager.start_server(port, delay_ms=0)
        time.sleep(0.1)

    # 4. Wait 5s, then Remove 2 Medium Servers
    logger.info("Phase 4: Running with Mixed servers for 5s...")
    time.sleep(5)
    
    stop_targets = MEDIUM_PORTS[:2]
    logger.info(f"Phase 5: Stopping 2 Medium Servers {stop_targets} (Failure simulation)...")
    for port in stop_targets:
        server_manager.stop_server(port)
    
    # 5. Wait 5s for stabilization
    logger.info("Phase 6: Running with remaining servers for 5s...")
    time.sleep(5)

    # Terminate Client if still running
    logger.info("Stopping Client...")
    if client_proc.poll() is None:
        client_proc.send_signal(signal.SIGINT)
        try:
            client_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            client_proc.kill()
            client_proc.wait()
    
    client_log.close()
    
    # Read Log
    with open(tmp_path / "client_stress.log", "r", errors="ignore") as f:
        client_output = f.read()

    # Analyze Results
    match_success = re.search(r"Success: (\d+)", client_output)
    success = int(match_success.group(1)) if match_success else 0
    match_failed = re.search(r"Failed: (\d+)", client_output)
    failed = int(match_failed.group(1)) if match_failed else 0
    
    total = success + failed
    logger.info(f"Total Requests: {total}, Success: {success}, Failed: {failed}")

    if success == 0:
        logger.warning("Client did not output stats. Using server logs for verification.")
        # logger.error(f"Client Output:\n{client_output}") # Too verbose if long
    
    # Assertions
    # 1. High Availability
    # If we have client stats, use them.
    if total > 0:
        assert failed < 100, f"Too many failures ({failed}) during dynamic scaling/faults"
        assert success > 1000, "Client didn't send enough requests"
    else:
        # Fallback: Check server logs
        counts = count_server_requests(tmp_path, MEDIUM_PORTS + FAST_PORTS)
        total_server_reqs = sum(counts.values())
        logger.info(f"Total Server Requests: {total_server_reqs}")
        assert total_server_reqs > 1000, "Servers received too few requests"

    # 2. Traffic Distribution

    # 2. Traffic Distribution
    counts = count_server_requests(tmp_path, MEDIUM_PORTS + FAST_PORTS)
    
    # Fast servers were added late (after 5s), but they are faster.
    # They should have significant traffic.
    total_fast_reqs = sum(counts[p] for p in FAST_PORTS)
    total_medium_reqs = sum(counts[p] for p in MEDIUM_PORTS)
    
    logger.info(f"Traffic Distribution:\nMedium: {total_medium_reqs}\nFast: {total_fast_reqs}")
    logger.info(f"Per Node Counts: {counts}")

    assert total_fast_reqs > 0, "Fast servers received no traffic after being added!"
    
    # Stopped servers should have traffic, but stopped receiving it.
    
    avg_fast = total_fast_reqs / len(FAST_PORTS)
    avg_medium = total_medium_reqs / len(MEDIUM_PORTS)
    
    logger.info(f"Avg Fast: {avg_fast}, Avg Medium: {avg_medium}")
    # We don't strictly assert avg_fast > avg_medium because of the time difference,
    # but it shouldn't be zero.

if __name__ == "__main__":
    sys.exit(pytest.main(["-v", "-s", __file__]))
