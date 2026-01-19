#pragma once

#include "Callbacks.h"
#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <set>
#include <string>
#include <vector>
#include <memory>
#include "lb/load_balancer.h"
#include "lb/consistent_hash_lb.h"

// 状态码定义
enum class CHANNEL_CODE
{
    SUCCESS,
    PACKAGE_ERR,
    SEND_ERR,
    RECEIVE_ERR,
    getServiceAddr_ERR
};

/**
 * @brief Rpc客户端通道
 * 
 * 职责：
 * 1. 序列化请求参数
 * 2. 服务发现与负载均衡 (从注册中心获取地址)
 * 3. 熔断保护 (Circuit Breaker)
 * 4. 网络传输 (Socket Send/Recv)
 * 5. 故障重试与容错
 */
class MpRpcChannel : public google::protobuf::RpcChannel
{
public:
    using AddrInfo = std::pair<std::string, uint16_t>;

    // 核心接口：发起一次 Rpc 调用
    void CallMethod(const google::protobuf::MethodDescriptor* methodDesc,
            google::protobuf::RpcController* controller,
            const google::protobuf::Message* request,
            google::protobuf::Message* response,
            google::protobuf::Closure* done) override;
    
private:
    // 1. 组装协议包
    CHANNEL_CODE packageRpcRequest(std::string* send_str,
            const google::protobuf::MethodDescriptor* methodDesc,
            google::protobuf::RpcController* controller,
            const google::protobuf::Message* request);
    
    // 3. 接收响应
    CHANNEL_CODE receiveRpcResponse(const int connfd,
            google::protobuf::Message* response,
            google::protobuf::RpcController* controller,
            int timeout_ms = 5000);

    // 4. 获取服务列表 (服务发现)
    CHANNEL_CODE getHosts(const std::string& service_name, 
        const std::string& method_name,
        std::vector<std::string>& hosts,
        std::shared_ptr<ConsistentHashRing>& cached_ring,
        google::protobuf::RpcController* controller);
};
