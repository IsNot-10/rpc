#include "metrics/Metrics.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>

int main() {
    auto& registry = metrics::MetricsRegistry::instance();
    
    std::cout << "Testing Metrics..." << std::endl;

    // Counter
    auto c = registry.GetCounter("test_counter", "Test Counter", {{"label", "val"}});
    c->Inc();
    c->Inc(2.5);
    
    // Gauge
    auto g = registry.GetGauge("test_gauge", "Test Gauge", {{"host", "localhost"}});
    g->Set(10);
    g->Inc();
    g->Dec(2);
    
    // Histogram
    auto h = registry.GetHistogram("test_hist", "Test Hist", {{"req", "1"}});
    h->Observe(0.1);
    h->Observe(0.5);
    h->Observe(1.5);
    
    std::string output = registry.ToPrometheus();
    // std::cout << output << std::endl;
    
    bool counter_found = output.find("test_counter{label=\"val\"} 3.5") != std::string::npos;
    bool gauge_found = output.find("test_gauge{host=\"localhost\"} 9") != std::string::npos;
    bool hist_sum_found = output.find("test_hist_sum{req=\"1\"} 2.1") != std::string::npos;
    bool hist_bucket_found = output.find("test_hist_bucket{req=\"1\",le=\"0.5\"} 2") != std::string::npos;
    bool hist_qps_found = output.find("test_hist_qps{req=\"1\"}") != std::string::npos;
    bool hist_max_found = output.find("test_hist_max_latency_seconds{req=\"1\"}") != std::string::npos;
    
    if (counter_found && gauge_found && hist_sum_found && hist_bucket_found && hist_qps_found && hist_max_found) {
        std::cout << "SUCCESS: All metrics found and correct." << std::endl;
    } else {
        std::cout << "FAILURE: Missing metrics." << std::endl;
        std::cout << "Output:\n" << output << std::endl;
        return 1;
    }

    return 0;
}
