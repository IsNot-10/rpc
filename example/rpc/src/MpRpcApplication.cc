#include "MpRpcApplication.h"
#include "Logging.h"
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <cstdlib>
#include <algorithm>

// 显示命令行帮助信息
static void ShowArgsHelp()
{
    std::cout << "format: command -i <configfile>" << std::endl;
}

// Meyers Singleton 实现：线程安全且懒加载
MpRpcApplication& MpRpcApplication::getInstance()
{
    static MpRpcApplication app;
    return app;
}

// 初始化：解析命令行参数 -i <config_file>
void MpRpcApplication::Init(int argc, char** argv)
{
    if(argc < 2)
    {
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
    int c = 0;
    std::string file;
    while((c = ::getopt(argc, argv, "i:")) != -1)
    {
        switch(c)
        {
        case 'i':
            file = optarg;
            break;
        case '?':
        case ':':
            ShowArgsHelp();
            exit(EXIT_FAILURE);
        default:
            break;
        }
    }
    LoadConfigFile(file.c_str());

    // Check LOG_LEVEL environment variable
    char* log_level_env = std::getenv("LOG_LEVEL");
    if (log_level_env) {
        std::string level_str = log_level_env;
        std::transform(level_str.begin(), level_str.end(), level_str.begin(), ::toupper);
        if (level_str == "TRACE") Logger::setLogLevel(Logger::LogLevel::TRACE);
        else if (level_str == "DEBUG") Logger::setLogLevel(Logger::LogLevel::DEBUG);
        else if (level_str == "INFO") Logger::setLogLevel(Logger::LogLevel::INFO);
        else if (level_str == "WARN") Logger::setLogLevel(Logger::LogLevel::WARN);
        else if (level_str == "ERROR") Logger::setLogLevel(Logger::LogLevel::ERROR);
        else if (level_str == "FATAL") Logger::setLogLevel(Logger::LogLevel::FATAL);
    }
}

// 加载配置文件内容到内存哈希表
void MpRpcApplication::LoadConfigFile(const char* file)
{
    std::ifstream ifs(file);
    if(!ifs.is_open())
    {
        LOG_ERROR << "Can not open config file: " << file;
        exit(EXIT_FAILURE);
    }
    
    std::string line;
    while(std::getline(ifs, line))
    {
        // 忽略注释
        if(line.empty() || line[0] == '#') continue;
        
        // 解析 key=value
        auto it = line.find('=');
        if(it != std::string::npos)  
        {
            std::string key = line.substr(0, it);
            std::string value = line.substr(it + 1);
            
            // 去除可能的换行符（兼容Windows换行）
            if (!value.empty() && value.back() == '\r') value.pop_back();
            
            configMap_.emplace(key, value);
        }
    }
}

// 获取配置项
std::string MpRpcApplication::Load(const std::string& key)
{
    auto it = configMap_.find(key);
    return it != configMap_.end() ? it->second : "";
}
