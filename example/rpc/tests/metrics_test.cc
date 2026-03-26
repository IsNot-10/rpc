#include "metrics/Metrics.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>

// Simple test framework
#define EXPECT_TRUE(a) \
    do { \
        if (!(a)) { \
            std::cerr << "FAILED: " << #a << " is false at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while(0)

void test_Concurrency() {
    std::cout << "Testing Metrics Concurrency..." << std::endl;
    auto& registry = metrics::MetricsRegistry::instance();
    auto c = registry.GetCounter("concurrent_counter", "Test Concurrent", {{"t", "1"}});
    
    int num_threads = 10;
    int ops_per_thread = 1000;
    
    std::vector<std::thread> threads;
    for(int i=0; i<num_threads; ++i) {
        threads.emplace_back([c, ops_per_thread]() {
            for(int j=0; j<ops_per_thread; ++j) {
                c->Inc();
            }
        });
    }
    
    for(auto& t : threads) {
        t.join();
    }
    
    std::string output = registry.ToPrometheus();
    // Expected value: num_threads * ops_per_thread
    std::string expected = std::to_string(num_threads * ops_per_thread);
    // Look for "concurrent_counter{t="1"} 10000" (maybe with .0)
    std::cout << "  Searching for " << expected << " in output..." << std::endl;
    
    bool found = false;
    if (output.find("concurrent_counter{t=\"1\"} " + expected) != std::string::npos) found = true;
    if (output.find("concurrent_counter{t=\"1\"} " + expected + ".0") != std::string::npos) found = true;
    
    if (!found) {
        std::cout << "Output snippet: " << output.substr(0, 500) << "..." << std::endl;
    }
    EXPECT_TRUE(found);
}

void test_Concurrency_Registration() {
    std::cout << "Testing Metrics Registration Concurrency..." << std::endl;
    auto& registry = metrics::MetricsRegistry::instance();
    
    int num_threads = 20;
    int metrics_per_thread = 100;
    std::atomic<bool> stop(false);
    
    std::vector<std::thread> threads;
    for(int i=0; i<num_threads; ++i) {
        threads.emplace_back([&, i]() {
            for(int j=0; j<metrics_per_thread; ++j) {
                // Mix of getting existing and new metrics
                std::string name = "dynamic_counter_" + std::to_string(j % 10); // shared
                if (i % 2 == 0) name += "_" + std::to_string(i); // unique per thread
                
                auto c = registry.GetCounter(name, "Dynamic desc", {{"thread", std::to_string(i)}});
                c->Inc();
            }
        });
    }
    
    for(auto& t : threads) t.join();
    
    // Verify registry didn't crash and has entries
    std::string output = registry.ToPrometheus();
    EXPECT_TRUE(output.length() > 0);
    std::cout << "  Registration Concurrency Test Passed (No Crash)" << std::endl;
}

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
    
    bool counter_found = output.find("test_counter{label=\"val\"} 3.5") != std::string::npos;
    bool gauge_found = output.find("test_gauge{host=\"localhost\"} 9") != std::string::npos;
    bool hist_sum_found = output.find("test_hist_sum{req=\"1\"} 2.1") != std::string::npos;
    
    EXPECT_TRUE(counter_found);
    EXPECT_TRUE(gauge_found);
    EXPECT_TRUE(hist_sum_found);
    
    test_Concurrency();
    test_Concurrency_Registration();
    
    std::cout << "All Metrics tests passed!" << std::endl;

    return 0;
}
