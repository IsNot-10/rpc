#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "user.pb.h"

std::atomic<int> g_success_count{0};
std::atomic<int> g_fail_count{0};

void thread_func(int id) {
    fixbug::UserServiceRpc_Stub stub(new MpRpcChannel());
    
    // 持续发送，模拟压力
    for (int i = 0; i < 20; ++i) { 
        fixbug::LoginRequest request;
        request.set_name("delay"); // 触发 200ms 延迟
        request.set_pwd("123456");
        
        fixbug::LoginResponse response;
        MpRpcController controller;
        
        // 设置较短的超时，以便快速失败
        // controller.SetTimeout(1000); 
        
        stub.Login(&controller, &request, &response, nullptr);
        
        if (controller.Failed()) {
            std::cout << "[Thread " << id << "] Request Failed: " << controller.ErrorText() << std::endl;
            g_fail_count++;
        } else {
            if (response.result().errcode() != 0) {
                 // 业务错误 (可能被限流返回 503)
                 g_fail_count++;
            } else {
                 g_success_count++;
            }
        }
        // std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    int threads_num = 20;
    int requests_per_thread = 20;
    std::string config_file;
    std::string user_name = "test"; // 默认为 test，避免 delay
    int pause_duration = 0;
    bool random_user = false;

    // Manual parsing to avoid getopt conflict
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "-t" && i + 1 < argc) {
            threads_num = std::stoi(argv[++i]);
        } else if (arg == "-r" && i + 1 < argc) {
            requests_per_thread = std::stoi(argv[++i]);
        } else if (arg == "-u" && i + 1 < argc) {
            user_name = argv[++i];
        } else if (arg == "--pause" && i + 1 < argc) {
            pause_duration = std::stoi(argv[++i]);
        } else if (arg == "--random-user") {
            random_user = true;
        }
    }

    if (config_file.empty()) {
        std::cout << "Usage: " << argv[0] << " -i <config_file> [-t threads] [-r requests] [-u user_name] [--pause seconds] [--random-user]" << std::endl;
        return 1;
    }

    // Construct args for MpRpcApplication
    char* new_argv[] = { argv[0], (char*)"-i", (char*)config_file.c_str(), nullptr };
    MpRpcApplication::getInstance().Init(3, new_argv);
    
    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();
    
    std::cout << "Starting stress test with " << threads_num << " threads, " << requests_per_thread << " requests each, user=" << user_name << "..." << std::endl;

    // 启动多线程并发冲击
    for (int i = 0; i < threads_num; ++i) {
        threads.emplace_back([i, requests_per_thread, user_name, pause_duration, random_user]() {
             // 每个线程独立的 Application 实例可能开销太大或不支持，但每个线程必须有自己的 Channel
             // MpRpcChannel 内部如果只复用连接，这里创建多个 Channel 应该会建立多条连接
             // 前提是 Channel 内部不共享全局连接池，或者连接池允许建立新连接
             
             // 为了确保多连接，我们显式地让 Channel 建立连接。
             // 目前 MpRpcChannel 构造函数并不建立连接，而是在 CallMethod 时懒加载或者从池中取。
             // 检查 MpRpcChannel 实现，如果是短连接或连接复用逻辑。
             // 假设默认实现是懒加载且复用的。为了压力测试，我们在每个线程里创建 Stub 和 Channel。
             
            fixbug::UserServiceRpc_Stub stub(new MpRpcChannel());
            
            // Phase 1: Half requests
            int half_requests = requests_per_thread / 2;
            if (pause_duration > 0 && half_requests == 0) half_requests = 1;

            auto send_once = [&](int j) {
                // If pause is requested, pause after half requests
                if (pause_duration > 0 && j == half_requests) {
                    // std::cout << "Thread " << i << " pausing for " << pause_duration << "s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(pause_duration));
                }
                
                fixbug::LoginRequest request;
                if (random_user) {
                    request.set_name("user" + std::to_string(i) + "_" + std::to_string(j));
                } else {
                    request.set_name(user_name); 
                }
                request.set_pwd("123456");
                
                fixbug::LoginResponse response;
                MpRpcController controller;
                
                stub.Login(&controller, &request, &response, nullptr);
                
                if (controller.Failed()) {
                    std::cout << "[Thread " << i << "] Request Failed: " << controller.ErrorText() << std::endl;
                    g_fail_count++;
                } else {
                    if (response.result().errcode() != 0) {
                         g_fail_count++;
                    } else {
                         g_success_count++;
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            };

            for (int j = 0; j < requests_per_thread; ++j) {
                send_once(j);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "========================================" << std::endl;
    std::cout << "Stress Test Finished in " << duration << " ms" << std::endl;
    std::cout << "Total Requests: " << (g_success_count + g_fail_count) << std::endl;
    std::cout << "Success: " << g_success_count << std::endl;
    std::cout << "Failed: " << g_fail_count << std::endl;
    std::cout << "========================================" << std::endl;
              
    return 0;
}
