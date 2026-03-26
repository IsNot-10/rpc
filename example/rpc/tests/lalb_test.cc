#include "lb/lalb_weight.h"
#include "lb/lalb_manager.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>

// using namespace lb; // lb namespace contains helper functions, not the main classes

#define EXPECT_EQ(a, b) \
    if ((a) != (b)) { \
        std::cerr << "EXPECT_EQ failed: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")" << std::endl; \
        exit(1); \
    }

#define EXPECT_NEAR(a, b, tolerance) \
    if (std::abs((a) - (b)) > (tolerance)) { \
        std::cerr << "EXPECT_NEAR failed: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")" << std::endl; \
        exit(1); \
    }

#define EXPECT_LT(a, b) \
    if (!((a) < (b))) { \
        std::cerr << "EXPECT_LT failed: " << #a << " < " << #b << " (" << (a) << " >= " << (b) << ")" << std::endl; \
        exit(1); \
    }

#define EXPECT_GT(a, b) \
    if (!((a) > (b))) { \
        std::cerr << "EXPECT_GT failed: " << #a << " > " << #b << " (" << (a) << " <= " << (b) << ")" << std::endl; \
        exit(1); \
    }

#define TEST(suite, name) void test_##suite##_##name()

TEST(LalbWeightTest, InitialWeight) {
    std::cout << "Running InitialWeight..." << std::endl;
    LalbWeight w(LalbManager::WEIGHT_SCALE);
    EXPECT_EQ(w.Value(), LalbManager::WEIGHT_SCALE);
}

TEST(LalbWeightTest, UpdateSuccess) {
    std::cout << "Running UpdateSuccess..." << std::endl;
    LalbWeight w(LalbManager::WEIGHT_SCALE);
    
    // Simulate a successful request with 10ms latency
    int64_t now = 1000000;
    int64_t latency = 10000; // 10ms
    w.Update(true, now, now + latency, 0, 0, 0);
    
    // Weight should be roughly WEIGHT_SCALE / 10000
    // But QPS scaling applies? 
    // Initial QPS is DEFAULT_QPS=1 -> scaled_qps = WEIGHT_SCALE
    // So weight = WEIGHT_SCALE / 10000
    
    int64_t expected = LalbManager::WEIGHT_SCALE / 10000;
    // Allow some margin if integer division or min weight affects it
    
    EXPECT_NEAR(w.Value(), expected, expected * 0.1);
}

TEST(LalbWeightTest, UpdateFailurePenalty) {
    std::cout << "Running UpdateFailurePenalty..." << std::endl;
    LalbWeight w(LalbManager::WEIGHT_SCALE);
    
    int64_t now = 1000000;
    // Success first to establish baseline
    w.Update(true, now, now + 10000, 0, 0, 0);
    int64_t weight_success = w.Value();
    
    // Failure with small latency (e.g. 1ms connection refused)
    // Should be punished to ~200ms effective latency
    w.Update(false, now + 20000, now + 21000, 0, 0, 0);
    
    int64_t weight_fail = w.Value();
    
    std::cout << "Weight success: " << weight_success << ", Weight fail: " << weight_fail << std::endl;
    
    EXPECT_LT(weight_fail, weight_success);
    // Should be roughly 20x smaller (200ms vs 10ms)
    EXPECT_LT(weight_fail, weight_success / 10);
}

TEST(LalbWeightTest, QPS_Scaling) {
    std::cout << "Running QPS_Scaling..." << std::endl;
    LalbWeight w(LalbManager::WEIGHT_SCALE);
    int64_t now = 1000000;
    
    // Fill queue with high QPS requests (e.g. 1ms interval, 10ms latency)
    // Queue size is 128.
    // 128 requests in 128ms -> ~1000 QPS.
    // scaled_qps should be ~ 1000 * WEIGHT_SCALE
    // avg_latency = 10ms
    // weight = 1000 * WEIGHT_SCALE / 10000 = WEIGHT_SCALE / 10
    
    for (int i = 0; i < 130; ++i) {
        w.Update(true, now, now + 10000, 0, 0, 0);
        now += 1000; // 1ms interval -> 1000 QPS
    }
    
    int64_t high_qps_weight = w.Value();
    
    LalbWeight w2(LalbManager::WEIGHT_SCALE);
    now = 1000000;
    for (int i = 0; i < 5; ++i) {
        w2.Update(true, now, now + 10000, 0, 0, 0);
        now += 1000000; // 1s interval -> 1 QPS
    }
    int64_t low_qps_weight = w2.Value();
    
    std::cout << "High QPS Weight: " << high_qps_weight << std::endl;
    std::cout << "Low QPS Weight: " << low_qps_weight << std::endl;
    
    // High QPS weight should be significantly higher
    EXPECT_GT(high_qps_weight, low_qps_weight * 10); 
}

int main() {
    test_LalbWeightTest_InitialWeight();
    test_LalbWeightTest_UpdateSuccess();
    test_LalbWeightTest_UpdateFailurePenalty();
    test_LalbWeightTest_QPS_Scaling();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
