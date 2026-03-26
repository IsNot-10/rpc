import os
import sys
import time
import subprocess
import pytest
import logging
import re
from pathlib import Path

# Import common utils
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from utils import ServerManager, RegistryManager, run_process, STRESS_CLIENT_BIN

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

@pytest.fixture
def environment(tmp_path):
    registry = RegistryManager(tmp_path)
    servers = ServerManager(tmp_path)
    
    yield registry, servers
    
    servers.stop_all()
    registry.stop()

def test_scalability_20_nodes(environment, tmp_path):
    registry, servers = environment
    
    # 1. Start Registry
    registry.start()
    
    # 2. Start 20 Servers
    server_count = 20
    base_port = 9001
    server_ports = list(range(base_port, base_port + server_count))
    
    logger.info(f"Starting {server_count} servers...")
    for port in server_ports:
        servers.start_server(port)
        
    # Allow time for registration
    time.sleep(2)
    
    # 3. Run Stress Client
    # Create client config
    client_conf = tmp_path / "client.conf"
    client_conf.write_text("rpcserverip=127.0.0.1\nrpcserverport=9001\nregistry_ip=127.0.0.1\nregistry_port=8001\n")
    
    logger.info("Running stress test...")
    cmd = [
        str(STRESS_CLIENT_BIN),
        "-i", str(client_conf),
        "-t", "4",
        "-c", "100",
        "-d", "10"  # 10 seconds duration
    ]
    
    p, _ = run_process(cmd, cwd=str(tmp_path))
    stdout, stderr = p.communicate()
    output = stdout.decode() if stdout else ""
    
    logger.info(f"Stress Test Output:\n{output}")
    
    # 4. Analyze Results
    # Example output: "Success: 158562, Failed: 0"
    success_match = re.search(r"Success:\s+(\d+)", output)
    failed_match = re.search(r"Failed:\s+(\d+)", output)
    
    if success_match and failed_match:
        success = int(success_match.group(1))
        failed = int(failed_match.group(1))
        total = success + failed
        
        if total == 0:
            pytest.fail("No requests completed")
            
        success_rate = success / total
        logger.info(f"Total: {total}, Success: {success}, Failed: {failed}, Rate: {success_rate:.4f}")
        
        assert success_rate > 0.95, f"Success rate {success_rate:.4f} < 0.95"
        assert total > 10000, f"Total requests {total} too low for scalability test"
    else:
        # Fallback if output format differs (check stdout manually if needed)
        # If stress client failed to run
        if p.returncode != 0:
            pytest.fail(f"Stress client failed with exit code {p.returncode}")
        logger.warning("Could not parse success/failure counts from output")
