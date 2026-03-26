#include "lb/consistent_hash_lb.h"
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <chrono>

// Simple test framework
#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "FAILED: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while(0)

#define EXPECT_TRUE(a) \
    do { \
        if (!(a)) { \
            std::cerr << "FAILED: " << #a << " is false at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while(0)

void test_Basic() {
    std::cout << "Testing ConsistentHash Basic Logic..." << std::endl;

    ConsistentHashLB lb;
    std::vector<std::string> hosts = {
        "192.168.1.1:8080",
        "192.168.1.2:8080",
        "192.168.1.3:8080",
        "192.168.1.4:8080",
        "192.168.1.5:8080"
    };

    // 1. Consistency Check
    SelectIn in;
    in.hosts = hosts;
    in.service_key = "TestService";
    in.request_key = "user_12345";

    std::string host1 = lb.select(in);
    std::string host2 = lb.select(in);
    EXPECT_EQ(host1, host2);
    std::cout << "  Consistency Check Passed: " << host1 << std::endl;

    // 2. Distribution Check
    std::map<std::string, int> counts;
    int total_reqs = 10000;
    for (int i = 0; i < total_reqs; ++i) {
        in.request_key = "user_" + std::to_string(i);
        std::string h = lb.select(in);
        counts[h]++;
    }

    double expected = total_reqs / hosts.size();
    double sum_sq_diff = 0;
    std::cout << "  Distribution:" << std::endl;
    for (const auto& h : hosts) {
        int c = counts[h];
        // std::cout << "    " << h << ": " << c << std::endl;
        EXPECT_TRUE(c > 0); 
        sum_sq_diff += std::pow(c - expected, 2);
    }
    
    double std_dev = std::sqrt(sum_sq_diff / hosts.size());
    double cv = std_dev / expected; 
    std::cout << "  CV: " << cv << std::endl;
    EXPECT_TRUE(cv < 0.3);

    // 3. Monotonicity (Add/Remove Node)
    std::vector<std::string> hosts_more = hosts;
    hosts_more.push_back("192.168.1.6:8080");
    
    std::map<std::string, std::string> key_to_host;
    in.hosts = hosts;
    for (int i = 0; i < 1000; ++i) {
        std::string k = "u" + std::to_string(i);
        in.request_key = k;
        key_to_host[k] = lb.select(in);
    }
    
    in.hosts = hosts_more;
    int moved_to_new = 0;
    int moved_to_others = 0;
    std::string new_node = "192.168.1.6:8080";
    
    for (int i = 0; i < 1000; ++i) {
        std::string k = "u" + std::to_string(i);
        in.request_key = k;
        std::string h = lb.select(in);
        
        if (h != key_to_host[k]) {
            if (h == new_node) {
                moved_to_new++;
            } else {
                moved_to_others++;
            }
        }
    }
    
    std::cout << "  Moved to new node: " << moved_to_new << std::endl;
    std::cout << "  Moved to others: " << moved_to_others << std::endl;
    
    EXPECT_EQ(moved_to_others, 0);
    EXPECT_TRUE(moved_to_new > 0);
}

void test_Concurrency_UpdateWhileSelection() {
    std::cout << "Testing Concurrency (Update while Selection)..." << std::endl;
    ConsistentHashLB lb;
    
    std::vector<std::string> hosts;
    for(int i=0; i<20; ++i) hosts.push_back("192.168.1." + std::to_string(i) + ":8080");
    
    std::mutex hosts_mutex;
    std::atomic<bool> stop(false);
    std::atomic<long> total_selects(0);
    
    // Worker threads (Readers)
    std::vector<std::thread> threads;
    for(int i=0; i<8; ++i) {
        threads.emplace_back([&]() {
            std::mt19937 gen(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            std::uniform_int_distribution<> dis(0, 1000000);
            
            while(!stop) {
                SelectIn in;
                in.service_key = "ConcurrentService";
                in.request_key = "user_" + std::to_string(dis(gen));
                
                {
                    std::lock_guard<std::mutex> lock(hosts_mutex);
                    in.hosts = hosts;
                }
                
                std::string s = lb.select(in);
                if(!s.empty()) total_selects++;
                
                // std::this_thread::yield();
            }
        });
    }
    
    // Updater thread (Writer)
    std::thread updater([&]() {
        std::mt19937 gen(std::random_device{}());
        while(!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> lock(hosts_mutex);
            
            // Randomly add or remove a host to simulate churn
            // Keep size between 10 and 30
            if (hosts.size() > 10 && std::uniform_int_distribution<>(0, 1)(gen) == 0) {
                hosts.pop_back();
            } else if (hosts.size() < 30) {
                hosts.push_back("192.168.1." + std::to_string(hosts.size()) + ":8080");
            }
        }
    });
    
    // Run stress test for 2 seconds
    // This is sufficient to catch basic race conditions (e.g. crash in EnsureHosts)
    std::cout << "  Running stress test for 2s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true;
    
    for(auto& t : threads) t.join();
    updater.join();
    
    std::cout << "  Total selections: " << total_selects << std::endl;
    EXPECT_TRUE(total_selects > 0);
    std::cout << "  Concurrency Test Passed (No Crash)" << std::endl;
}

int main() {
    test_Basic();
    test_Concurrency_UpdateWhileSelection();
    std::cout << "All ConsistentHash tests passed!" << std::endl;
    return 0;
}
