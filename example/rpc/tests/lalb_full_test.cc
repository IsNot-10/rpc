#include "lb/lalb_manager.h"
#include "lb/lb_common.h"
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <iostream>
#include <cmath>
#include <random>
#include <chrono>
#include <cassert>
#include <map>
#include <set>
#include <mutex>

// Helper macros for simple testing
#define EXPECT_TRUE(condition) \
    if (!(condition)) { \
        std::cerr << "EXPECT_TRUE failed: " << #condition << std::endl; \
        exit(1); \
    }

#define EXPECT_FALSE(condition) \
    if ((condition)) { \
        std::cerr << "EXPECT_FALSE failed: " << #condition << std::endl; \
        exit(1); \
    }

#define EXPECT_GT(a, b) \
    if (!((a) > (b))) { \
        std::cerr << "EXPECT_GT failed: " << #a << " > " << #b << " (" << (a) << " <= " << (b) << ")" << std::endl; \
        exit(1); \
    }

// Helper to generate fake hosts
std::vector<std::string> GenerateHosts(int n) {
    std::vector<std::string> hosts;
    for (int i = 0; i < n; ++i) {
        hosts.push_back("127.0.0.1:" + std::to_string(9000 + i));
    }
    return hosts;
}

void test_HostManagement() {
    std::cout << "Running HostManagement..." << std::endl;
    LalbManager manager;
    auto hosts = GenerateHosts(5);
    
    manager.EnsureHosts(hosts);
    
    std::set<std::string> excluded;
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
        
    std::string selected = manager.Select(excluded, now);
    EXPECT_FALSE(selected.empty());
    
    bool found = false;
    for (const auto& h : hosts) {
        if (h == selected) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

void test_PreferFastNodes() {
    std::cout << "Running PreferFastNodes..." << std::endl;
    LalbManager manager;
    std::vector<std::string> hosts = {"127.0.0.1:9001", "127.0.0.1:9002"};
    manager.EnsureHosts(hosts);
    
    std::string fast_node = "127.0.0.1:9001";
    std::string slow_node = "127.0.0.1:9002";
    
    int64_t now = 1000000;
    
    // Training Phase
    for (int i = 0; i < 100; ++i) {
        manager.Feedback(fast_node, true, now, now + 10000, 0, 0); // 10ms
        now += 1000;
        if (i % 10 == 0) {
            manager.Feedback(slow_node, true, now, now + 100000, 0, 0); // 100ms
        }
    }
    
    // Selection Phase
    std::map<std::string, int> counts;
    std::set<std::string> excluded;
    
    for (int i = 0; i < 1000; ++i) {
        std::string s = manager.Select(excluded, now);
        if (!s.empty()) {
            counts[s]++;
            if (s == fast_node) {
                manager.Feedback(s, true, now, now + 10000, 0, 0);
            } else {
                manager.Feedback(s, true, now, now + 100000, 0, 0);
            }
            now += 500;
        }
    }
    
    std::cout << "  Fast Node: " << counts[fast_node] << ", Slow Node: " << counts[slow_node] << std::endl;
    EXPECT_GT(counts[fast_node], counts[slow_node] * 5);
}

void test_Failover() {
    std::cout << "Running Failover..." << std::endl;
    LalbManager manager;
    std::vector<std::string> hosts = {"127.0.0.1:9001", "127.0.0.1:9002"};
    manager.EnsureHosts(hosts);
    
    std::string node1 = "127.0.0.1:9001";
    std::string node2 = "127.0.0.1:9002";
    
    int64_t now = 1000000;
    
    // Warmup
    for (int i = 0; i < 50; ++i) {
        manager.Feedback(node1, true, now, now + 10000, 0, 0);
        manager.Feedback(node2, true, now, now + 10000, 0, 0);
        now += 1000;
    }
    
    // Fail Node 1
    for (int i = 0; i < 150; ++i) {
        manager.Feedback(node1, false, now, now + 1000, 0, 0);
        now += 100;
    }
    
    std::map<std::string, int> counts;
    std::set<std::string> excluded;
    
    for (int i = 0; i < 200; ++i) {
        std::string s = manager.Select(excluded, now);
        counts[s]++;
        if (s == node1) {
             manager.Feedback(s, false, now, now + 1000, 0, 0);
        } else {
             manager.Feedback(s, true, now, now + 10000, 0, 0);
        }
        now += 1000;
    }
    
    std::cout << "  Node 1 (Failed): " << counts[node1] << ", Node 2 (Healthy): " << counts[node2] << std::endl;
    EXPECT_GT(counts[node2], counts[node1] * 10);
}

void test_InflightPunishment() {
    std::cout << "Running InflightPunishment..." << std::endl;
    LalbManager manager;
    std::vector<std::string> hosts = {"127.0.0.1:9001", "127.0.0.1:9002"};
    manager.EnsureHosts(hosts);
    
    std::string node1 = "127.0.0.1:9001";
    std::string node2 = "127.0.0.1:9002";
    
    int64_t now = 1000000;
    
    // Warmup
    for (int i = 0; i < 50; ++i) {
        manager.Feedback(node1, true, now, now + 10000, 0, 0);
        manager.Feedback(node2, true, now, now + 10000, 0, 0);
        now += 1000;
    }
    
    // Inflight Pileup
    std::set<std::string> excluded;
    for (int i = 0; i < 100; ++i) {
        std::string s = manager.Select(excluded, now);
        if (s == node2) {
            manager.Feedback(s, true, now, now + 10000, 0, 0);
        }
        now += 500; 
    }
    
    int node2_selections = 0;
    int node1_late_selections = 0;
    
    for (int i = 0; i < 100; ++i) {
        std::string s = manager.Select(excluded, now);
        if (s == node2) node2_selections++;
        if (s == node1) node1_late_selections++;
        now += 500;
    }
    
    std::cout << "  Late selections - Node 1: " << node1_late_selections << ", Node 2: " << node2_selections << std::endl;
    EXPECT_GT(node2_selections, node1_late_selections);
}

void test_Concurrency_Lalb() {
    std::cout << "Running Concurrency Test for LALB..." << std::endl;
    LalbManager manager;
    auto hosts = GenerateHosts(10);
    manager.EnsureHosts(hosts);
    
    std::atomic<bool> stop(false);
    std::atomic<long> selections(0);
    
    // 8 threads selecting and feeding back
    std::vector<std::thread> threads;
    for(int i=0; i<8; ++i) {
        threads.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<> lat_dist(5000, 15000); // 5-15ms
            std::set<std::string> excluded;
            
            while(!stop) {
                int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                    
                std::string s = manager.Select(excluded, now);
                if (!s.empty()) {
                    selections++;
                    // Simulate latency
                    int64_t latency = lat_dist(gen);
                    // Add some noise/failures
                    bool success = true;
                    if (latency > 14000) success = false; // Occasional failure
                    
                    manager.Feedback(s, success, now, now + latency, 0, 0);
                }
                // Yield to reduce CPU burn
                if (i == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    // Updater thread to churn hosts
    std::thread updater([&]() {
        while(!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // Rotate hosts
            std::string removed = hosts.back();
            hosts.pop_back();
            hosts.insert(hosts.begin(), removed);
            manager.EnsureHosts(hosts);
        }
    });
    
    std::cout << "  Running LALB stress for 2s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true;
    
    for(auto& t : threads) t.join();
    updater.join();
    
    std::cout << "  Total selections: " << selections << std::endl;
    EXPECT_TRUE(selections > 0);
    std::cout << "  LALB Concurrency Test Passed" << std::endl;
}

int main() {
    test_HostManagement();
    test_PreferFastNodes();
    test_Failover();
    test_InflightPunishment();
    test_Concurrency_Lalb();
    std::cout << "All full tests passed!" << std::endl;
    return 0;
}
