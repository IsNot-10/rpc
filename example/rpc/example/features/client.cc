#include <iostream>
#include <string>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <mutex>

#include "echo.pb.h"
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "tracing/TraceContext.h"
#include "metrics/Metrics.h"
#include "Logging.h"
#include "AsyncLogging.h"

using namespace demo;
using namespace mprpc::tracing;

std::unique_ptr<AsyncLogging> g_asyncLog;

void asyncOutput(const char* msg, int len) {
    g_asyncLog->append(msg, len);
}

std::atomic<int> g_success_count(0);
std::atomic<int> g_fail_count(0);
std::atomic<long long> g_total_latency(0);
bool g_verbose = false;
std::mutex g_mutex;
std::map<std::string, int> g_server_stats;

void showUsage(const char* prog) {
    std::cout << "Usage: " << prog << " -t <threads> -n <requests_per_thread> [-d <delay_us>] [-lb <algo>] [-b <backup_ms>] [-v]" << std::endl;
    std::cout << "  -lb: random, rr, ch, lalb" << std::endl;
    std::cout << "  -v: verbose output" << std::endl;
}

void worker(int id, int requests, int delay_us) {
    EchoService_Stub stub(new MpRpcChannel());
    
    // Create a trace span for this worker thread
    auto span = Span::CreateClientSpan("worker_" + std::to_string(id));
    TraceContext::SetCurrentSpan(span);

    for (int i = 0; i < requests; ++i) {
        EchoRequest request;
        request.set_message("Hello from thread " + std::to_string(id) + " req " + std::to_string(i));
        request.set_sleep_us(delay_us);
        
        EchoResponse response;
        MpRpcController controller;
        
        auto start = std::chrono::steady_clock::now();
        stub.Echo(&controller, &request, &response, nullptr);
        auto end = std::chrono::steady_clock::now();
        
        if (controller.Failed()) {
            g_fail_count++;
            if (g_verbose) {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::cerr << "[Thread " << id << "] RPC Failed: " << controller.ErrorText() << std::endl;
            }
        } else if (response.result().errcode() != 0) {
            g_fail_count++;
            if (g_verbose) {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::cerr << "[Thread " << id << "] App Error: " << response.result().errcode() << " - " << response.result().errmsg() << std::endl;
            }
        } else {
            g_success_count++;
            long long latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            g_total_latency += latency;
            
            std::string remote = controller.GetRemoteAddr().empty() ? "Unknown" : controller.GetRemoteAddr();

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_server_stats[remote]++;
                
                if (g_verbose) {
                    std::cout << "[Thread " << id << "] Response: " << response.message() 
                              << " Latency: " << latency << "us" 
                              << " From: " << remote
                              << " TraceID: " << span->TraceIdStr() << std::endl;
                }
            }
        }
        
        // Record metrics manually for demo purposes (framework does this too internally)
        metrics::MetricsRegistry::instance().GetCounter("client_requests_total", "Total client requests", {{"thread", std::to_string(id)}})->Inc();

        // 稍微休眠一下避免发太快
        usleep(1000); 
    }
    
    TraceContext::SetCurrentSpan(nullptr);
}

int main(int argc, char* argv[]) {
    // Enable Async Logging to file
    off_t rollSize = 500 * 1000 * 1000;
    g_asyncLog.reset(new AsyncLogging("client", rollSize));
    g_asyncLog->start();
    Logger::setOutput(asyncOutput);

    // 设置日志级别为 INFO
    Logger::setLogLevel(Logger::LogLevel::INFO);

    int threads = 1;
    int requests = 10;
    int delay_us = 0;
    std::string lb_algo = "random";
    int backup_ms = 1000; // 默认很大，不触发
    int timeout_ms = 5000;

    int opt;
    while ((opt = getopt(argc, argv, "t:n:d:b:l:vo:")) != -1) {
        switch (opt) {
            case 't': threads = std::stoi(optarg); break;
            case 'n': requests = std::stoi(optarg); break;
            case 'd': delay_us = std::stoi(optarg); break;
            case 'b': backup_ms = std::stoi(optarg); break;
            case 'l': lb_algo = optarg; break; // -l for load balancer because -lb is not standard getopt
            case 'v': g_verbose = true; break;
            case 'o': timeout_ms = std::stoi(optarg); break;
            default: showUsage(argv[0]); return 1;
        }
    }

    // 生成临时配置文件
    std::string config_file = "client.conf";
    std::ofstream out(config_file);
    out << "rpcserverip=127.0.0.1" << std::endl;
    out << "rpcserverport=9999" << std::endl;
    out << "registry_ip=127.0.0.1" << std::endl;
    out << "registry_port=8001" << std::endl;
    out << "load_balancer=" << lb_algo << std::endl;
    out << "backup_request_ms=" << backup_ms << std::endl;
    out << "rpc_timeout_ms=" << timeout_ms << std::endl;
    out.close();

    // 重置 getopt 状态
    optind = 1;

    // 初始化框架
    char* fake_argv[] = { argv[0], (char*)"-i", (char*)config_file.c_str() };
    int fake_argc = 3;
    MpRpcApplication::getInstance().Init(fake_argc, fake_argv);

    std::cout << "Starting " << threads << " threads, " << requests << " requests each..." << std::endl;
    std::cout << "Load Balancer: " << lb_algo << ", Backup Request: " << backup_ms << "ms" << std::endl;

    auto start_all = std::chrono::steady_clock::now();

    std::vector<std::thread> thread_pool;
    for (int i = 0; i < threads; ++i) {
        thread_pool.emplace_back(worker, i, requests, delay_us);
    }

    for (auto& t : thread_pool) {
        t.join();
    }
    
    LOG_INFO << "Client finishing...";
    if (g_asyncLog) {
        g_asyncLog->stop();
    }

    auto end_all = std::chrono::steady_clock::now();
    double total_time_sec = std::chrono::duration_cast<std::chrono::milliseconds>(end_all - start_all).count() / 1000.0;

    int total_reqs = g_success_count + g_fail_count;
    double qps = (total_time_sec > 0) ? total_reqs / total_time_sec : 0;
    double avg_latency = (g_success_count > 0) ? (double)g_total_latency / g_success_count / 1000.0 : 0;

    std::cout << "========================================" << std::endl;
    std::cout << "Total Requests: " << total_reqs << std::endl;
    std::cout << "Success: " << g_success_count << std::endl;
    std::cout << "Failed: " << g_fail_count << std::endl;
    std::cout << "Total Time: " << total_time_sec << "s" << std::endl;
    std::cout << "QPS: " << qps << std::endl;
    std::cout << "Avg Latency: " << avg_latency << "ms" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Server Distribution:" << std::endl;
    for (const auto& pair : g_server_stats) {
        std::cout << "  " << pair.first << ": " << pair.second 
                  << " (" << (pair.second * 100.0 / g_success_count) << "%)" << std::endl;
    }
    std::cout << "========================================" << std::endl;
    
    if (g_verbose) {
        std::cout << "\n[Metrics Dump]" << std::endl;
        std::cout << metrics::MetricsRegistry::instance().ToPrometheus() << std::endl;
    }

    return 0;
}
