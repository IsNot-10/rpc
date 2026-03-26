#include <iostream>
#include <string>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>

#include "echo.pb.h"
#include "MpRpcApplication.h"
#include "MpRpcProvider.h"
#include "MpRpcController.h"
#include "Logging.h"
#include "AsyncLogging.h"

using namespace demo;

std::unique_ptr<AsyncLogging> g_asyncLog;

void asyncOutput(const char* msg, int len) {
    g_asyncLog->append(msg, len);
}

int g_intrinsic_delay_ms = 0;
int g_error_rate = 0; // 0-100 percentage

class EchoServiceImpl : public EchoService {
public:
    void Echo(google::protobuf::RpcController* controller,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) {
        // 模拟随机错误
        if (g_error_rate > 0) {
            int r = rand() % 100;
            if (r < g_error_rate) {
                std::cout << "[EchoService] Simulating Error (rate=" << g_error_rate << "%, rand=" << r << ") - DROP REQUEST" << std::endl;
                // 不调用 done->Run()，也不发送响应，让客户端超时
                return;
            }
        }

        // 模拟延迟
        if (request->sleep_us() > 0) {
            usleep(request->sleep_us());
        }
        if (g_intrinsic_delay_ms > 0) {
            usleep(g_intrinsic_delay_ms * 1000);
        }

        response->set_message("Echo: " + request->message());
        
        // 打印日志方便观察负载均衡 (Disabled to reduce noise, client summary provides distribution)
        // std::cout << "[EchoService] Received: " << request->message() 
        //           << ", Sleep: " << request->sleep_us() << "us" 
        //           << " [processed by port " << MpRpcApplication::getInstance().Load("rpcserverport") << "]"
        //           << std::endl;

        if (done) {
            done->Run();
        }
    }
};

void showUsage(const char* prog) {
    std::cout << "Usage: " << prog << " -p <port> [-l <rate_limit>] [-c <max_concurrency>] [-d <delay_ms>] [-e <error_rate_percent>]" << std::endl;
}

int main(int argc, char* argv[]) {
    // Enable Async Logging to file
    off_t rollSize = 500 * 1000 * 1000;
    g_asyncLog.reset(new AsyncLogging("server", rollSize));
    g_asyncLog->start();
    Logger::setOutput(asyncOutput);

    // 设置日志级别为 INFO，以便在文件中看到更多细节
    Logger::setLogLevel(Logger::LogLevel::INFO);
    
    srand(time(nullptr));
    int port = 8000;
    int rate_limit = -1;
    int max_concurrency = 1000;
    int intrinsic_delay_ms = 0;
    int error_rate = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:l:c:d:e:")) != -1) {
        switch (opt) {
            case 'p': port = std::stoi(optarg); break;
            case 'l': rate_limit = std::stoi(optarg); break;
            case 'c': max_concurrency = std::stoi(optarg); break;
            case 'd': intrinsic_delay_ms = std::stoi(optarg); break;
            case 'e': error_rate = std::stoi(optarg); break;
            default: showUsage(argv[0]); return 1;
        }
    }
    g_intrinsic_delay_ms = intrinsic_delay_ms;
    g_error_rate = error_rate;

    // 生成临时配置文件
    std::string config_file = "server_" + std::to_string(port) + ".conf";
    std::ofstream out(config_file);
    out << "rpcserverip=127.0.0.1" << std::endl;
    out << "rpcserverport=" << port << std::endl;
    out << "registry_ip=127.0.0.1" << std::endl;
    out << "registry_port=8001" << std::endl;
    if (rate_limit > 0) {
        out << "rate_limit=" << rate_limit << std::endl;
    }
    if (max_concurrency > 0) {
        out << "max_concurrency=" << max_concurrency << std::endl;
    }
    out.close();

    // 重置 getopt 状态，以便 MpRpcApplication::Init 再次解析参数
    optind = 1;

    // 初始化框架
    char* fake_argv[] = { argv[0], (char*)"-i", (char*)config_file.c_str() };
    int fake_argc = 3;
    MpRpcApplication::getInstance().Init(fake_argc, fake_argv);

    // 注册服务并启动
    MpRpcProvider provider;
    provider.notifyService(new EchoServiceImpl());
    provider.run();
    
    if (g_asyncLog) {
        g_asyncLog->stop();
    }

    return 0;
}
