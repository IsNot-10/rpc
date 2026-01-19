#include <iostream>
#include <string>
#include <vector>
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "user.pb.h"
#include "Logging.h"

int main(int argc, char** argv) {
    std::string config_file;
    int count = 100;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "-n" && i + 1 < argc) {
            count = std::stoi(argv[++i]);
        }
    }

    if (config_file.empty()) {
        std::cout << "Usage: " << argv[0] << " -i <config_file> [-n count]" << std::endl;
        return 1;
    }

    char* new_argv[] = { argv[0], (char*)"-i", (char*)config_file.c_str(), nullptr };
    MpRpcApplication::getInstance().Init(3, new_argv);
    
    // 设置日志级别
    const char* log_level_env = std::getenv("LOG_LEVEL");
    if (log_level_env) {
        std::string level(log_level_env);
        if (level == "TRACE") Logger::setLogLevel(Logger::LogLevel::TRACE);
        else if (level == "DEBUG") Logger::setLogLevel(Logger::LogLevel::DEBUG);
        else if (level == "INFO") Logger::setLogLevel(Logger::LogLevel::INFO);
        else if (level == "WARN") Logger::setLogLevel(Logger::LogLevel::WARN);
        else if (level == "ERROR") Logger::setLogLevel(Logger::LogLevel::ERROR);
        else if (level == "FATAL") Logger::setLogLevel(Logger::LogLevel::FATAL);
    }

    fixbug::UserServiceRpc_Stub stub(new MpRpcChannel());
    
    int success = 0;
    for (int i = 0; i < count; ++i) {
        fixbug::LoginRequest request;
        std::string name = "user_" + std::to_string(i);
        request.set_name(name);
        request.set_pwd("123456");
        
        fixbug::LoginResponse response;
        MpRpcController controller;
        
        stub.Login(&controller, &request, &response, nullptr);
        
        if (!controller.Failed() && response.result().errcode() == 0) {
            success++;
            // Format: "User: user_123 => Route: 127.0.0.1:9001"
            std::cout << "User: " << name << " => Route: " << controller.GetRemoteAddr() << std::endl;
        } else {
            std::cout << "Sent " << name << " Failed: " << controller.ErrorText() << std::endl;
        }
    }
    
    std::cout << "Verification Client Finished. Success: " << success << "/" << count << std::endl;
    return 0;
}
