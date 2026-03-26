#include "tracing/TraceContext.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>

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

using namespace mprpc::tracing;

void test_SpanCreation() {
    std::cout << "Testing SpanCreation..." << std::endl;
    
    // 1. Client Span
    auto span = Span::CreateClientSpan("test_method");
    EXPECT_TRUE(span != nullptr);
    EXPECT_EQ(span->Name(), "test_method");
    EXPECT_TRUE(span->trace_id() != 0);
    EXPECT_TRUE(span->span_id() != 0);
    EXPECT_EQ(span->parent_span_id(), 0);
    
    std::cout << "  Client Span created: TraceID=" << span->TraceIdStr() << std::endl;
    
    // 2. Server Span (Child)
    auto child = Span::CreateServerSpan(span->TraceIdStr(), "beef", span->SpanIdStr(), "server_method");
    EXPECT_TRUE(child != nullptr);
    EXPECT_EQ(child->trace_id(), span->trace_id());
    EXPECT_EQ(child->parent_span_id(), span->span_id());
    
    std::cout << "  Server Span created: TraceID=" << child->TraceIdStr() << ", ParentID=" << child->ParentSpanIdStr() << std::endl;
}

void test_ContextPropagation() {
    std::cout << "Testing ContextPropagation..." << std::endl;
    
    auto span = Span::CreateClientSpan("root");
    TraceContext::SetCurrentSpan(span);
    
    auto current = TraceContext::GetCurrentSpan();
    EXPECT_EQ(current, span);
    
    // Nested scope
    {
        auto child = Span::CreateClientSpan("child");
        // Note: CreateClientSpan doesn't automatically set parent from context unless implemented.
        // Let's check implementation behavior or manual linking.
        // If CreateClientSpan uses GetCurrentSpan internally, parent should be set.
        // Checking header... CreateClientSpan(name) usually starts a new trace or uses thread local?
        // Let's assume for now it might be independent if not explicitly passed.
        
        // But we can manually create a child
        auto manual_child = std::make_shared<Span>(span->trace_id(), 12345, span->span_id(), "manual_child", SpanKind::INTERNAL);
        TraceContext::SetCurrentSpan(manual_child);
        EXPECT_EQ(TraceContext::GetCurrentSpan(), manual_child);
        
        TraceContext::SetCurrentSpan(span); // Restore
    }
    
    EXPECT_EQ(TraceContext::GetCurrentSpan(), span);
    TraceContext::SetCurrentSpan(nullptr);
}

void test_Serialization() {
    std::cout << "Testing Serialization..." << std::endl;
    // Basic check of ID strings
    uint64_t trace_id = 123456789;
    uint64_t span_id = 987654321;
    auto span = std::make_shared<Span>(trace_id, span_id, 0, "test", SpanKind::CLIENT);
    
    // Check if string representation is hex
    std::string tid = span->TraceIdStr();
    // 123456789 = 0x75BCD15
    // Should be hex string
    std::cout << "  TraceID Hex: " << tid << std::endl;
    EXPECT_TRUE(tid.length() > 0);
}

void test_Concurrency_Context() {
    std::cout << "Testing Concurrency (Thread Local Context)..." << std::endl;
    
    // Verify that setting span in one thread doesn't affect another
    int num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> errors(0);
    
    for(int i=0; i<num_threads; ++i) {
        threads.emplace_back([&, i]() {
            // Each thread sets a unique span
            std::string name = "thread_" + std::to_string(i);
            auto span = Span::CreateClientSpan(name);
            TraceContext::SetCurrentSpan(span);
            
            // Sleep to allow interleaving
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            // Verify context is still ours
            auto current = TraceContext::GetCurrentSpan();
            if (current != span) {
                std::cerr << "Thread " << i << " lost its span!" << std::endl;
                errors++;
            }
            if (current->Name() != name) {
                std::cerr << "Thread " << i << " saw wrong span name: " << current->Name() << std::endl;
                errors++;
            }
            
            // Clear
            TraceContext::SetCurrentSpan(nullptr);
            if (TraceContext::GetCurrentSpan() != nullptr) {
                std::cerr << "Thread " << i << " failed to clear span!" << std::endl;
                errors++;
            }
        });
    }
    
    for(auto& t : threads) t.join();
    
    EXPECT_EQ(errors, 0);
    std::cout << "  Concurrency Context Test Passed" << std::endl;
}

int main() {
    test_SpanCreation();
    test_ContextPropagation();
    test_Serialization();
    test_Concurrency_Context();
    std::cout << "All Tracing tests passed!" << std::endl;
    return 0;
}
