import sys
import time
import pytest
import logging
from utils import (
    ServerManager, RegistryManager, 
    create_client_config, run_stress_client, 
    wait_for_port, kill_process_tree, run_process,
    get_env
)
import subprocess
import re

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

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

def test_timeout_and_retry(registry, server_manager, tmp_path):
    """Test integration of Timeout and Retry logic."""
    logger.info("TEST: Timeout and Retry Integration")
    
    # 1. Start one slow server (2s delay) and one fast server (0ms)
    server_manager.start_server(9001, delay_ms=2000)
    server_manager.start_server(9002, delay_ms=0)
    
    time.sleep(2) # Wait for registration
    
    # 2. Configure Client: Timeout=3000ms (Global), Retries=3
    # Use LALB to handle latency differences intelligently (ConsistentHash sticks to slow node)
    conf_file = create_client_config(tmp_path, lb_algo="lalb", timeout_ms=3000, retries=3)
    
    # 3. Run Client
    # 20 requests. LALB should favor 9002 (Fast) over 9001 (Slow).
    success, failed, _ = run_stress_client(conf_file, threads=2, requests=20, user="random_user")
    
    logger.info(f"Timeout/Retry Result: Success={success}, Failed={failed}")
    
    assert success > 15, f"Too many failures with valid backup node. Success: {success}/20"

def test_concurrency_limiter_and_degradation(registry, server_manager, tmp_path):
    """Test Concurrency Limiter and Degradation (Server returning 503)."""
    logger.info("TEST: Concurrency Limiter & Degradation")
    
    # 1. Start server with very low rate limit (e.g., 5 QPS)
    server_manager.start_server(9003, rate_limit=5)
    
    time.sleep(2)
    
    conf_file = create_client_config(tmp_path, lb_algo="random")
    
    # 2. Flood with requests (50 concurrent threads)
    # Should trigger server-side rate limiting (Degradation)
    success, failed, output = run_stress_client(conf_file, threads=10, requests=20, user="flood")
    
    logger.info(f"Limiter Result: Success={success}, Failed={failed}")
    
    # We expect failures due to rate limiting
    assert failed > 0, "Expected some requests to be rate limited (failed), but all succeeded!"
    assert success > 0, "Expected some requests to succeed!"
    
    # Also verify the client didn't crash
    assert "Segmentation fault" not in output

def test_circuit_breaker_integration(registry, server_manager, tmp_path):
    """Test Circuit Breaker Integration: Stop sending to dead node."""
    logger.info("TEST: Circuit Breaker Integration")
    
    server_manager.start_server(9004)
    server_manager.start_server(9005)
    
    time.sleep(2)
    conf_file = create_client_config(tmp_path, lb_algo="random")
    
    # 1. Warm up
    run_stress_client(conf_file, threads=2, requests=10)
    
    # 2. Kill 9004
    server_manager.stop_server(9004)
    
    # 3. Send many requests
    # CB should trip and direct all traffic to 9005
    # Increase requests to ensure CB trips (Window=1500)
    success, failed, _ = run_stress_client(conf_file, threads=5, requests=2000)
    
    logger.info(f"CB Result: Success={success}, Failed={failed}")
    
    # With random LB, 50% would go to 9004.
    # Without CB, 50% would fail.
    # With CB, failures should be small (initial failures until trip).
    # Expected success: > 1800
    assert success > 1800, f"Circuit Breaker didn't protect enough. Success: {success}/2000"

def test_backup_request(registry, server_manager, tmp_path):
    """
    Test Backup Request (Hedged Request):
    1. Server 1: 1000ms delay.
    2. Server 2: 0ms delay.
    3. Client: backup_request_ms=200.
    4. Send request that routes to Server 1.
    5. Verify it completes in ~200-300ms (not 1000ms).
    """
    logger.info("TEST: Backup Request (Hedged Request)")
    
    # Start Servers
    server_manager.start_server(9001, delay_ms=1000) # Slow
    server_manager.start_server(9002, delay_ms=0)    # Fast
    
    time.sleep(2)
    
    conf_file = create_client_config(tmp_path, lb_algo="random", backup_ms=200)
    
    # Use stress client to measure average latency implicitly?
    # Stress client doesn't output average latency directly in stdout format I parsed,
    # but run_stress_client returns success/failed.
    # I can modify run_stress_client to return duration?
    # Or just check success count (if timeout was set, slow would fail).
    # But here we don't set timeout, we set backup request.
    # If backup works, it should be fast.
    # If I want to verify latency, I need a client that reports it or time the execution.
    
    start_time = time.time()
    success, failed, output = run_stress_client(conf_file, threads=1, requests=10, user="random")
    total_duration = time.time() - start_time
    
    logger.info(f"Output:\n{output}")
    
    assert success >= 9 # Allow 1 failure
    
    avg_latency = total_duration / 10.0
    logger.info(f"Average Latency: {avg_latency*1000:.2f}ms")
    
    # If backup request works, avg latency should be ~200ms + overhead.
    # If it didn't work, half requests would take 1000ms. Avg would be ~500ms.
    assert avg_latency < 0.4, f"Average latency too high ({avg_latency:.2f}s). Backup request might not be working."

if __name__ == "__main__":
    sys.exit(pytest.main(["-v", __file__]))
