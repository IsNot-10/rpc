#pragma once

#include <string>
#include <memory>
#include <unordered_map>

class MpRpcApplication
{
public:
    ~MpRpcApplication()=default; 
    
    void Init(int argc, char** argv);
    
    static MpRpcApplication& getInstance();

    std::string Load(const std::string& key);

    MpRpcApplication(const MpRpcApplication&)=delete;
    MpRpcApplication& operator=(const MpRpcApplication&)=delete;
    MpRpcApplication(MpRpcApplication&&)=delete;
    MpRpcApplication& operator=(MpRpcApplication&&)=delete;

private:
    MpRpcApplication()=default;
    
    void LoadConfigFile(const char* file);

    std::unordered_map<std::string, std::string> configMap_;
};
