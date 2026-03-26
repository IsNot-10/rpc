import os
import sys
import time
import subprocess
import socket
import logging
import signal
from pathlib import Path

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

# Paths
# Assuming this file is in muduo-x/rpc/test
TEST_DIR = Path(os.path.dirname(os.path.abspath(__file__)))
ROOT_DIR = TEST_DIR.parent
BUILD_DIR = ROOT_DIR / "build"
BIN_DIR = ROOT_DIR / "example/rpc/bin"
LIB_DIR = ROOT_DIR / "lib"
EXAMPLE_LIB_DIR = ROOT_DIR / "example/rpc/lib"

REGISTRY_BIN = BUILD_DIR / "example/registry/registry_server"
SERVER_BIN = BIN_DIR / "server"
STRESS_CLIENT_BIN = BIN_DIR / "stress_client"
VERIFICATION_CLIENT_BIN = BIN_DIR / "verification_client"
GATEWAY_BIN = BUILD_DIR / "example/gateway/GatewayServer"

def get_env():
    """Returns environment variables with correct LD_LIBRARY_PATH."""
    env = os.environ.copy()
    lib_path = f"{EXAMPLE_LIB_DIR}:{LIB_DIR}"
    if "LD_LIBRARY_PATH" in env:
        env["LD_LIBRARY_PATH"] = f"{lib_path}:{env['LD_LIBRARY_PATH']}"
    else:
        env["LD_LIBRARY_PATH"] = lib_path
    return env

def wait_for_port(port, host='127.0.0.1', timeout=10.0):
    """Waits for a port to be open."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except (socket.timeout, ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False

def find_free_port():
    """Finds a free port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        return s.getsockname()[1]

def run_process(cmd, env=None, log_path=None, cwd=None):
    """Runs a subprocess with proper environment and logging."""
    full_env = get_env()
    if env:
        full_env.update(env)
    
    stdout_dest = subprocess.DEVNULL
    stderr_dest = subprocess.DEVNULL
    log_file = None

    if log_path:
        log_file = open(log_path, "w")
        stdout_dest = log_file
        # stderr_dest = log_file 
        # Let stderr go to console for debugging
        stderr_dest = None 
        
    try:
        p = subprocess.Popen(
            cmd, 
            env=full_env,
            cwd=cwd,
            stdout=stdout_dest,
            stderr=stderr_dest,
            preexec_fn=os.setsid # Create new process group
        )
        return p, log_file
    except Exception as e:
        if log_file:
            log_file.close()
        raise e

def force_kill_port(port):
    """Force kills any process listening on the given port."""
    try:
        # Use fuser to kill process on port (TCP)
        subprocess.run(["fuser", "-k", "-n", "tcp", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
    except Exception:
        pass

def wait_for_port_closed(port, host='127.0.0.1', timeout=5.0):
    """Waits for a port to be closed."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                time.sleep(0.1)
        except (socket.timeout, ConnectionRefusedError, OSError):
            return True
    return False

def kill_process_tree(pid):
    """Kills a process tree safely."""
    try:
        os.killpg(os.getpgid(pid), signal.SIGTERM)
        # Give it a moment to terminate gracefully
        time.sleep(0.1)
        # Force kill if needed (not implemented to avoid zombie/orphans, but OS cleans up usually)
    except ProcessLookupError:
        pass
    except Exception as e:
        logger.warning(f"Error killing process group {pid}: {e}")

class ServerManager:
    """Manages RPC Server processes."""
    def __init__(self, tmp_path):
        self.processes = {}  # port -> Popen
        self.log_files = {}
        self.tmp_path = Path(tmp_path)

    def start_server(self, port, registry_port=8001, delay_ms=0, rate_limit=-1, weight=1, extra_env=None):
        if port in self.processes:
            return

        conf_content = f"""rpcserverip=127.0.0.1
rpcserverport={port}
registry_ip=127.0.0.1
registry_port={registry_port}
rpc_server_weight={weight}
rate_limit={rate_limit}
max_concurrency=1000
"""
        conf_file = self.tmp_path / f"server_{port}.conf"
        conf_file.write_text(conf_content)
        
        log_path = self.tmp_path / f"server_{port}.log"
        
        env = {"SERVER_DELAY_MS": str(delay_ms), "ENABLE_ACCESS_LOG": "1"}
            
        if extra_env:
            env.update(extra_env)

        p, log_f = run_process(
            [str(SERVER_BIN), "-i", str(conf_file)],
            env=env,
            log_path=log_path
        )
        
        self.processes[port] = p
        self.log_files[port] = log_f
        
        if not wait_for_port(port):
            self.stop_server(port)
            raise RuntimeError(f"Server on port {port} failed to start")

    def stop_server(self, port):
        if port in self.processes:
            p = self.processes[port]
            kill_process_tree(p.pid)
            p.wait()
            
            if port in self.log_files:
                self.log_files[port].close()
                del self.log_files[port]
            
            del self.processes[port]

    def stop_all(self):
        for port in list(self.processes.keys()):
            self.stop_server(port)

class RegistryManager:
    """Manages Registry Server process."""
    def __init__(self, tmp_path):
        self.process = None
        self.log_file = None
        self.tmp_path = Path(tmp_path)
        self.port = 8001

    def start(self, port=8001):
        self.port = port
        log_path = self.tmp_path / "registry.log"
        
        # Ensure no existing registry on this port
        force_kill_port(port)
        
        # Ensure no existing registry
        # subprocess.run(["pkill", "-f", "registry_server"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # time.sleep(0.5)

        p, log_f = run_process([str(REGISTRY_BIN)], log_path=log_path)
        self.process = p
        self.log_file = log_f
        
        if not wait_for_port(port):
            self.stop()
            raise RuntimeError("Registry server failed to start")

    def stop(self):
        if self.process:
            kill_process_tree(self.process.pid)
            self.process.wait()
            self.process = None
        
        if self.log_file:
            self.log_file.close()
            self.log_file = None

    def query(self, service_name, method_name):
        """Query the registry for a service."""
        try:
            with socket.create_connection(('127.0.0.1', self.port), timeout=1) as sock:
                msg = f"DIS|{service_name}|{method_name}|\n"
                sock.sendall(msg.encode())
                response = sock.recv(4096).decode()
                
                if response.startswith("RES"):
                    parts = response.split('|')
                    hosts = [p.strip() for p in parts if p.strip() and p != "RES"]
                    logger.info(f"Query {service_name}:{method_name} -> {hosts}")
                    return hosts
        except Exception as e:
            logger.warning(f"Query failed: {e}")
            pass
        return []

    def wait_for_service(self, service_name, method_name, min_nodes=1, timeout=20):
        """Wait for a service to be registered with minimum number of nodes."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            hosts = self.query(service_name, method_name)
            if len(hosts) >= min_nodes:
                return True
            time.sleep(0.5)
        return False

class GatewayManager:
    """Manages Gateway Server process."""
    def __init__(self, tmp_path):
        self.process = None
        self.log_file = None
        self.tmp_path = Path(tmp_path)
        self.port = 8080

    def start(self, registry_port=8001, port=8080, lb_algo="random"):
        self.port = port
        log_path = self.tmp_path / "gateway.log"
        conf_file = self.tmp_path / "gateway.conf"
        
        conf_content = f"""registry_ip=127.0.0.1
registry_port={registry_port}
load_balancer={lb_algo}
# server_port might be hardcoded in GatewayServer to 8080 or configurable?
# Assuming defaults.
"""
        conf_file.write_text(conf_content)

        p, log_f = run_process([str(GATEWAY_BIN), "-i", str(conf_file)], log_path=log_path)
        self.process = p
        self.log_file = log_f
        
        if not wait_for_port(port):
            self.stop()
            raise RuntimeError(f"Gateway server failed to start on {port}")

    def stop(self):
        if self.process:
            kill_process_tree(self.process.pid)
            self.process.wait()
            self.process = None
        
        if self.log_file:
            self.log_file.close()
            self.log_file = None

def create_client_config(work_dir, registry_port=8001, lb_algo="random", timeout_ms=None, retries=None, backup_ms=None):
    """Creates a client configuration file."""
    conf_name = Path(work_dir) / f"client_{lb_algo}_{int(time.time()*1000)}.conf"
    content = [
        "rpcserverip=127.0.0.1",
        "rpcserverport=8080",  # Fake port
        "registry_ip=127.0.0.1",
        f"registry_port={registry_port}",
        f"load_balancer={lb_algo}"
    ]
    
    if timeout_ms is not None:
        content.append(f"rpc_timeout_ms={timeout_ms}")
    if retries is not None:
        content.append(f"rpc_max_retries={retries}")
    if backup_ms is not None:
        content.append(f"backup_request_ms={backup_ms}")
        
    conf_name.write_text("\n".join(content) + "\n")
    return str(conf_name)

def run_stress_client(conf_file, threads=1, requests=100, user="delay", pause=0):
    """Runs the stress client and returns results."""
    cmd = [
        str(STRESS_CLIENT_BIN), 
        "-i", str(conf_file), 
        "-t", str(threads), 
        "-r", str(requests), 
        "-u", user
    ]
    
    if pause > 0:
        cmd.extend(["--pause", str(pause)])
        
        # If pausing, we might want to return the process to let caller handle it?
        # But existing usage in test_reliability waits for it or kills registry in between.
        # For simple usage, we can just run it.
        # However, test_reliability's test_registry_restart_resilience uses pause to kill registry *during* run.
        # That test manages subprocess directly.
        # Let's support basic run here.
    
    import re
    
    # We use subprocess.run for synchronous execution
    env = get_env()
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    
    match_success = re.search(r"Success: (\d+)", proc.stdout)
    success = int(match_success.group(1)) if match_success else 0
    
    match_failed = re.search(r"Failed: (\d+)", proc.stdout)
    failed = int(match_failed.group(1)) if match_failed else 0
    
    return success, failed, proc.stdout

def count_server_requests(tmp_path, ports):
    """Count requests handled by each server by reading their logs."""
    counts = {}
    for port in ports:
        log_path = Path(tmp_path) / f"server_{port}.log"
        count = 0
        if log_path.exists():
            try:
                content = log_path.read_text(encoding="utf-8", errors="ignore")
                # Count "Login" or similar keywords indicating a request was handled
                # This depends on server implementation. 
                # Assuming "doing local service" or just counting method calls if logged.
                # In our server.cc: LOG_INFO << "doing local service: " << request->name();
                count = content.count("doing local service")
            except Exception:
                pass
        counts[port] = count
    return counts
