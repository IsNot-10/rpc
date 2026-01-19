#pragma once

#include "TcpServer.h"
#include "EventLoop.h"
#include "TimeStamp.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <vector>

// 服务节点信息结构体
struct ServiceNode {
    std::string ipPort;         // 服务地址 "IP:Port"
    TimeStamp lastHeartbeat;    // 最后一次心跳时间
    int weight = 1;             // 权重，用于负载均衡
};

/**
 * @brief 分布式服务注册中心服务端
 * 
 * 功能：
 * 1. 接收服务提供者(RpcProvider)的注册请求 (REG)
 * 2. 接收服务消费者(RpcChannel)的服务发现请求 (DIS)
 * 3. 接收服务提供者的心跳保活 (HB)
 * 4. 定时剔除超时未发送心跳的节点
 */
class RegistryServer
{
public:
    RegistryServer(EventLoop* loop, const InetAddress& listenAddr);
    
    // 启动注册中心服务
    void start();

private:
    // 处理新连接建立/断开的回调
    void onConnection(const TcpConnectionPtr& conn);

    // 处理读写事件的回调（解析协议并处理请求）
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp time);

    // 处理服务注册请求
    void handleRegister(const std::string& key, const std::string& method, const std::string& ipPort, int weight);
    
    // 处理服务发现请求
    void handleDiscovery(const TcpConnectionPtr& conn, const std::string& service, const std::string& method);
    
    // 处理心跳请求
    void handleHeartbeat(const std::string& ipPort);

    // 定时任务：清理过期节点
    void cleanUpExpiredNodes();

    EventLoop* loop_;           // 事件循环（Reactor）
    TcpServer server_;          // Muduo TcpServer
    
    // 服务注册表
    // Key: "ServiceName/MethodName"
    // Value: 提供该服务方法的节点列表
    std::unordered_map<std::string, std::vector<ServiceNode>> serviceMap_;
    
    // 反向索引: IP:Port -> set<ServiceKey>
    std::unordered_map<std::string, std::unordered_set<std::string>> nodeServices_;

    std::mutex mutex_;          // 保护 serviceMap_ 的互斥锁
};
