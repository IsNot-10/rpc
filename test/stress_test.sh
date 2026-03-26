#!/bin/bash
set -e

# Loop 10 times
for i in {1..5}; do
    echo "=================================================="
    echo "Run #$i"
    echo "=================================================="
    
    echo "Running test_tracing.py (Distributed Tracing)..."
    pytest -vs ./test_tracing.py

    echo "Running test_scalability.py..."
    pytest -vs ./test_scalability.py
    
    echo "Running test_reliability.py..."
    pytest -vs ./test_reliability.py
    
    echo "Running test_lalb_cluster.py..."
    pytest -vs ./test_lalb_cluster.py

    echo "Running test_lalb_dynamic.py..."
    pytest -vs ./test_lalb_dynamic.py

    echo "Running test_ha_integration.py..."
    pytest -vs ./test_ha_integration.py
    
    echo "Run #$i Completed Successfully"
done
