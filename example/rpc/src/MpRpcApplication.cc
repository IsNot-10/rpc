#include "MpRpcApplication.h"
#include "Logging.h"
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <cstdlib>
#include <algorithm>

static void ShowArgsHelp()
{
    std::cout << "format: command -i <configfile>" << std::endl;
}

MpRpcApplication& MpRpcApplication::getInstance()
{
    static MpRpcApplication app;
    return app;
}

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
        if(line.empty() || line[0] == '#') continue;
        
        auto it = line.find('=');
        if(it != std::string::npos)
        {
            std::string key = line.substr(0, it);
            std::string value = line.substr(it + 1);
            
            if (!value.empty() && value.back() == '\r') value.pop_back();
            
            configMap_.emplace(key, value);
        }
    }
}

std::string MpRpcApplication::Load(const std::string& key)
{
    auto it = configMap_.find(key);
    return it != configMap_.end() ? it->second : "";
}
