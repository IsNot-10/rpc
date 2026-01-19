#pragma once

#include <string>
#include <memory>
#include <unordered_map>

/**
 * @brief Rpc框架的基础应用类 (单例模式)
 * 
 * 职责：
 * 1. 初始化框架环境 (Init)
 * 2. 加载配置文件 (Load)
 * 3. 提供全局配置访问
 */
class MpRpcApplication
{
public:
    ~MpRpcApplication()=default; 
    
    // 初始化函数，解析命令行参数并加载配置文件
    void Init(int argc, char** argv);
    
    // 获取单例对象的引用
    static MpRpcApplication& getInstance();

    // 读取配置项
    std::string Load(const std::string& key);

    // 禁用拷贝构造和赋值操作
    MpRpcApplication(const MpRpcApplication&)=delete;
    MpRpcApplication& operator=(const MpRpcApplication&)=delete;
    MpRpcApplication(MpRpcApplication&&)=delete;
    MpRpcApplication& operator=(MpRpcApplication&&)=delete;

private:
    MpRpcApplication()=default;
    
    // 内部函数：加载配置文件
    void LoadConfigFile(const char* file);

private:
    // 存储配置信息的哈希表 (Key=Value)
    std::unordered_map<std::string,std::string> configMap_;
};
