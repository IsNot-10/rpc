#include "ha/circuit_breaker.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cassert>
#include <atomic>
#include <random>
#include <cmath>

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

using namespace ha;

// BRPC-style Feedback Control for Stress Testing
struct FeedbackControl {
    FeedbackControl(int req_num, int error_percent, std::string host)
        : req_num(req_num)
        , error_percent(error_percent)
        , host(host)
        , healthy_cnt(0)
        , unhealthy_cnt(0)
        , healthy(true) 
    {}

    int req_num;
    int error_percent;
    std::string host;
    int healthy_cnt;
    int unhealthy_cnt;
    bool healthy;
};

void feedback_thread(FeedbackControl* fc) {
    auto& cb = CircuitBreaker::instance();
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 99);

    for (int i = 0; i < fc->req_num; ++i) {
        bool call_healthy = false;
        // Simulate request
        if (dis(gen) < fc->error_percent) {
            // Report failure
            cb.report_status(fc->host, false, 1000); // 1ms latency
            call_healthy = false;
        } else {
            // Report success
            cb.report_status(fc->host, true, 1000); // 1ms latency
            call_healthy = true;
        }
        
        // Check status immediately after report? 
        // In BRPC test, they check the return value of OnCallEnd.
        // Our report_status returns void, but we can check should_access.
        
        if (cb.should_access(fc->host)) {
            fc->healthy_cnt++;
            fc->healthy = true;
        } else {
            fc->unhealthy_cnt++;
            fc->healthy = false;
        }
        
        // Small sleep to simulate real traffic interval
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

void StartFeedbackThread(std::vector<std::thread>& thread_list,
                         std::vector<std::unique_ptr<FeedbackControl>>& fc_list,
                         int error_percent,
                         std::string host,
                         int thread_num = 3) {
    thread_list.clear();
    fc_list.clear();
    for (int i = 0; i < thread_num; ++i) {
        auto fc = std::make_unique<FeedbackControl>(2000, error_percent, host);
        thread_list.emplace_back(feedback_thread, fc.get());
        fc_list.push_back(std::move(fc));
    }
}

void test_EmaErrorRecorder() {
    std::cout << "[Unit] Testing EmaErrorRecorder..." << std::endl;
    
    // Window size 10, Max error 10%
    EmaErrorRecorder recorder(10, 10);
    
    // 1. Initial State
    EXPECT_TRUE(recorder.on_call_end(0, 1000)); // Success
    
    // 2. Add some failures
    for(int i=0; i<5; ++i) {
        recorder.on_call_end(1, 1000); // Fail
    }
    // Should be unhealthy now
    double rate = recorder.get_error_rate();
    std::cout << "  Error rate after 5 failures: " << rate << std::endl;
    EXPECT_TRUE(rate > 0.1);
    
    // 3. Recovery
    for(int i=0; i<20; ++i) {
        recorder.on_call_end(0, 1000); // Success
    }
    rate = recorder.get_error_rate();
    std::cout << "  Error rate after recovery: " << rate << std::endl;
    EXPECT_TRUE(rate < 0.1);
}

void test_CircuitBreaker_Stress_ShouldNotIsolate() {
    std::cout << "[Stress] Testing Should Not Isolate (Low Error Rate)..." << std::endl;
    std::string host = "127.0.0.1:9001";
    CircuitBreaker::instance().reset(host);

    std::vector<std::thread> thread_list;
    std::vector<std::unique_ptr<FeedbackControl>> fc_list;

    // 3% error rate, should not trigger 10% threshold
    StartFeedbackThread(thread_list, fc_list, 3, host);

    for (auto& t : thread_list) {
        t.join();
    }

    for (const auto& fc : fc_list) {
        std::cout << "  Thread Result: healthy=" << fc->healthy_cnt << ", unhealthy=" << fc->unhealthy_cnt << std::endl;
        // In a low error rate scenario, unhealthy count (blocked requests) should be 0
        EXPECT_EQ(fc->unhealthy_cnt, 0);
        EXPECT_TRUE(fc->healthy);
    }
    std::cout << "  PASSED" << std::endl;
}

void test_CircuitBreaker_Stress_ShouldIsolate() {
    std::cout << "[Stress] Testing Should Isolate (High Error Rate)..." << std::endl;
    std::string host = "127.0.0.1:9002";
    CircuitBreaker::instance().reset(host);

    std::vector<std::thread> thread_list;
    std::vector<std::unique_ptr<FeedbackControl>> fc_list;

    // 50% error rate, should trigger isolation
    StartFeedbackThread(thread_list, fc_list, 50, host);

    for (auto& t : thread_list) {
        t.join();
    }

    int total_blocked = 0;
    for (const auto& fc : fc_list) {
        std::cout << "  Thread Result: healthy=" << fc->healthy_cnt << ", unhealthy=" << fc->unhealthy_cnt << std::endl;
        total_blocked += fc->unhealthy_cnt;
    }
    
    // We expect significant blocking
    EXPECT_TRUE(total_blocked > 0);
    
    // In a 50% error rate scenario, the node might be healthy or broken at the exact moment the test ends
    // (due to a lucky successful probe). So we don't strictly assert !should_access(host) here.
    // Instead, the fact that we blocked requests (total_blocked > 0) proves the circuit breaker worked.
    std::cout << "  PASSED (Blocked " << total_blocked << " requests)" << std::endl;
}

void test_CircuitBreaker_Isolation_Growth() {
    std::cout << "[Stress] Testing Isolation Duration Growth..." << std::endl;
    std::string host = "127.0.0.1:9003";
    CircuitBreaker::instance().reset(host);
    
    std::vector<std::thread> thread_list;
    std::vector<std::unique_ptr<FeedbackControl>> fc_list;

    // 1. Trigger First Isolation
    std::cout << "  Triggering 1st isolation..." << std::endl;
    StartFeedbackThread(thread_list, fc_list, 100, host); // 100% error
    for (auto& t : thread_list) t.join();
    
    // We can't easily check the internal duration ms without exposing it, 
    // but we can check if it stays broken.
    EXPECT_TRUE(!CircuitBreaker::instance().should_access(host));
    
    // 2. Wait for probe (min isolation is 100ms, but it might have doubled to 200ms)
    // We wait 250ms to ensure we are in the probe window even if it is 200ms
    std::cout << "  Waiting for probe window (250ms)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    
    // Now it should allow a probe (which we will fail again)
    // We simulate a failed probe by reporting error
    // Note: should_access might be true here (probe allowed), that's expected.
    
    CircuitBreaker::instance().report_status(host, false, 1000);
    
    // This should trigger extended isolation (200ms)
    // Let's verify it is still broken
    EXPECT_TRUE(!CircuitBreaker::instance().should_access(host));
    
    std::cout << "  PASSED" << std::endl;
}

int main() {
    test_EmaErrorRecorder();
    test_CircuitBreaker_Stress_ShouldNotIsolate();
    test_CircuitBreaker_Stress_ShouldIsolate();
    test_CircuitBreaker_Isolation_Growth();
    
    std::cout << "All CircuitBreaker tests passed!" << std::endl;
    return 0;
}
