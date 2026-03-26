#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <getopt.h>
#include <iomanip>
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "user.pb.h"

// Global statistics
std::atomic<long> g_requests{0};
std::atomic<long> g_success{0};
std::atomic<long> g_failed{0};
std::atomic<bool> g_running{true};

struct Config {
    int threads = 1;
    int connections = 1; // Not fully utilized in blocking mode, but we'll try to honor it if possible or just use as thread cap
    int duration = 10; // seconds
    int requests = 0; // If > 0, run for this many requests instead of duration
    std::string user = "stress_user"; // User name for requests
    std::string config_file = "rpc_config.xml"; // Default config file
    int pause_seconds = 0; // Pause before starting stress
};

void stress_worker(int id, const Config& config) {
    // Each thread creates its own Stub, but they share the underlying ConnectionPool (singleton)
    // In blocking mode, each thread holds at most 1 active connection at a time.
    // So 'connections' parameter is effectively limited by 'threads' count in this implementation.
    
    fixbug::UserServiceRpc_Stub stub(new MpRpcChannel());
    
    // Pre-allocate request objects to reduce overhead? 
    // Actually in a stress test we might want to test object creation too, but reusing is more efficient for network stress.
    fixbug::LoginRequest request;
    request.set_name(config.user);
    request.set_pwd("123456");
    
    while (g_running) {
        if (config.requests > 0 && g_requests >= config.requests) {
            break;
        }

        fixbug::LoginResponse response;
        MpRpcController controller;
        
        stub.Login(&controller, &request, &response, nullptr);
        
        g_requests++;
        if (controller.Failed()) {
            g_failed++;
            // Optional: print error occasionally
            // std::cerr << "Request failed: " << controller.ErrorText() << std::endl;
        } else {
            if (response.result().errcode() == 0) {
                g_success++;
            } else {
                g_failed++;
            }
        }
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -t, --threads <N>      Number of threads (default: 1)\n"
              << "  -c, --connections <N>  Number of connections (default: 1)\n"
              << "  -d, --duration <S>     Duration in seconds (default: 10)\n"
              << "  -r, --requests <N>     Total requests to send (overrides duration)\n"
              << "  -u, --user <NAME>      User name for requests (default: stress_user)\n"
              << "  -i, --config <FILE>    Config file path (default: rpc_config.xml)\n"
              << "  --pause <S>            Pause in seconds before starting stress\n"
              << "  -h, --help             Show this help message\n";
}

int main(int argc, char** argv) {
    Config config;
    
    struct option long_options[] = {
        {"threads", required_argument, 0, 't'},
        {"connections", required_argument, 0, 'c'},
        {"duration", required_argument, 0, 'd'},
        {"requests", required_argument, 0, 'r'},
        {"user", required_argument, 0, 'u'},
        {"config", required_argument, 0, 'i'},
        {"pause", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "t:c:d:r:u:i:p:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 't':
                config.threads = std::stoi(optarg);
                break;
            case 'c':
                config.connections = std::stoi(optarg);
                break;
            case 'd':
                config.duration = std::stoi(optarg);
                break;
            case 'r':
                config.requests = std::stoi(optarg);
                break;
            case 'u':
                config.user = optarg;
                break;
            case 'i':
                config.config_file = optarg;
                break;
            case 'p': 
                config.pause_seconds = std::stoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (config.threads <= 0) config.threads = 1;
    if (config.duration <= 0) config.duration = 1;
    
    // In blocking mode, we need 1 thread per connection to achieve concurrency
    if (config.connections > config.threads) {
        if (config.connections <= 1000) {
             std::cout << "Adjusting threads to " << config.connections << " to match requested connections (Blocking Mode)" << std::endl;
             config.threads = config.connections;
        } else {
             std::cout << "Warning: Connection count " << config.connections << " is too high for blocking threads. Capping at 1000." << std::endl;
             config.threads = 1000;
             config.connections = 1000;
        }
    }

    // Initialize RPC Application
    // We need to pass a fake argc/argv to Init if we want to pass the config file
    optind = 1;
    char* init_argv[] = { argv[0], (char*)"-i", (char*)config.config_file.c_str(), nullptr };
    MpRpcApplication::getInstance().Init(3, init_argv);
    
    if (config.pause_seconds > 0) {
        std::cout << "Pausing for " << config.pause_seconds << " seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(config.pause_seconds));
    }

    if (config.requests > 0) {
        std::cout << "Running request-based test @ Protobuf RPC" << std::endl;
        std::cout << "  Target: " << config.requests << " requests" << std::endl;
    } else {
        std::cout << "Running " << config.duration << "s test @ Protobuf RPC" << std::endl;
    }
    std::cout << "  " << config.threads << " threads and " << config.connections << " connections" << std::endl;

    std::vector<std::thread> threads;
    auto start_time = std::chrono::steady_clock::now();
    
    for (int i = 0; i < config.threads; ++i) {
        threads.emplace_back(stress_worker, i, config);
    }
    
    // Run loop
    if (config.requests > 0) {
        // Wait until requests are done
        while (g_running) {
            if (g_requests >= config.requests) {
                g_running = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        // Run for duration
        std::this_thread::sleep_for(std::chrono::seconds(config.duration));
        g_running = false;
    }
    
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto actual_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double seconds = actual_duration / 1000000.0;
    
    long total_reqs = g_requests;
    long total_success = g_success;
    long total_failed = g_failed;
    
    std::cout << "------------------------------------------------" << std::endl;
    std::cout << "  Thread Stats   Avg      Stdev     Max   +/- Stdev" << std::endl;
    std::cout << "    (Not implemented per-thread stats yet)" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;
    std::cout << "  " << total_reqs << " requests in " << std::fixed << std::setprecision(2) << seconds << "s, " 
              << (total_reqs / seconds) << " req/s" << std::endl;
    std::cout << "  Success: " << total_success << ", Failed: " << total_failed << std::endl;
    std::cout << "------------------------------------------------" << std::endl;

    return 0;
}
