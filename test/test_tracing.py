import sys
import time
import pytest
import logging
import json
import re
from pathlib import Path
from utils import (
    ServerManager, RegistryManager, 
    run_process, VERIFICATION_CLIENT_BIN,
    create_client_config
)

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger(__name__)

@pytest.fixture(scope="module")
def registry(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp("tracing_registry")
    manager = RegistryManager(tmp_path)
    manager.start()
    yield manager
    manager.stop()

@pytest.fixture(scope="function")
def server_manager(tmp_path):
    manager = ServerManager(tmp_path)
    yield manager
    manager.stop_all()

def extract_spans(log_path):
    spans = []
    if not log_path.exists():
        logger.warning(f"Log file not found: {log_path}")
        return spans
        
    logger.info(f"Reading log: {log_path}")
    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()
        logger.info(f"Log Size: {len(content)}")
        for line in content.splitlines():
            if "[TRACE]" in line:
                try:
                    parts = line.split("[TRACE] ")
                    if len(parts) < 2: continue
                    
                    json_candidate = parts[1]
                    first_brace = json_candidate.find('{')
                    if first_brace == -1: continue
                    
                    json_candidate = json_candidate[first_brace:]
                    last_brace = json_candidate.rfind('}')
                    if last_brace != -1:
                        json_str = json_candidate[:last_brace+1]
                        span = json.loads(json_str)
                        spans.append(span)
                except Exception as e:
                    pass
    return spans

def verify_spans(client_spans, server_spans):
    found_match = False
    for c_span in client_spans:
        trace_id = c_span.get('trace_id')
        span_id = c_span.get('span_id')
        
        for s_span in server_spans:
            if s_span.get('trace_id') == trace_id and s_span.get('parent_span_id') == span_id:
                found_match = True
                
                assert s_span['kind'] == 'SERVER'
                assert c_span['kind'] == 'CLIENT'
                assert s_span['name'] == c_span['name']
                
                if 'start_time_us' in s_span and 'start_time_us' in c_span:
                     assert s_span['start_time_us'] >= c_span['start_time_us']
                
                logger.info(f"Verified Trace: {trace_id}")
                break
        if found_match: break
    
    assert found_match, "Could not find a matching Client-Server span pair"

def test_distributed_tracing(registry, server_manager, tmp_path):
    # Log paths
    client_log_path = tmp_path / "client_trace.log"
    
    # 1. Start Server with TRACE level
    server_port = 9999
    server_manager.start_server(server_port, extra_env={"LOG_LEVEL": "TRACE"})
    server_log_path = tmp_path / f"server_{server_port}.log"
    
    time.sleep(2)
    
    # 2. Configure Client
    conf_file = create_client_config(tmp_path, registry_port=registry.port, lb_algo="random")
    
    # 3. Run Client
    logger.info("Starting Client...")
    client_cmd = [
        str(VERIFICATION_CLIENT_BIN), 
        "-i", str(conf_file),
        "-m", "Login",
        "-d", '{"name":"trace_user", "pwd":"123"}'
    ]

    p, log_f = run_process(
        client_cmd, 
        env={"LOG_LEVEL": "TRACE"}, 
        log_path=client_log_path
    )
    
    try:
        p.wait(timeout=10)
    except Exception:
        p.kill()
        
    if log_f: log_f.close()
    
    time.sleep(1)
    
    # 4. Parse Logs
    client_spans = extract_spans(client_log_path)
    server_spans = extract_spans(server_log_path)
    
    logger.info(f"Captured {len(client_spans)} client spans and {len(server_spans)} server spans")
    
    assert len(client_spans) > 0, "No client spans captured"
    assert len(server_spans) > 0, "No server spans captured"
    
    # 5. Verification
    verify_spans(client_spans, server_spans)

if __name__ == "__main__":
    sys.exit(pytest.main(["-v", __file__]))
