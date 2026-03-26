#pragma once

#include "Callbacks.h"
#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <map>
#include <chrono>
#include "http/HttpContext.h"
#include "http/HttpResponse.h"
#include "TcpServer.h"
#include "metrics/Metrics.h"

class InetAddress;

/**
 * @brief Rpc服务提供者
 */
class MpRpcProvider
{
public:
    MpRpcProvider();
    ~MpRpcProvider();

    void notifyService(google::protobuf::Service* service);
    void run();

private:
    struct RequestInfo
    {
        std::string service_name;
        std::string method_name;
        uint32_t args_size;
        std::string args_str;
        std::map<std::string, std::string> meta_data;
    };

    struct MethodContext {
        const google::protobuf::MethodDescriptor* descriptor;
        std::shared_ptr<metrics::Histogram> latencyHistogram;
    };

    struct ServiceInfo
    {
        google::protobuf::Service* service_;
        std::unordered_map<std::string, MethodContext> methodMap_;
    };

    class TokenBucket;
    class AutoConcurrencyLimiter;

    std::unique_ptr<TokenBucket> tokenBucket_;
    std::unique_ptr<AutoConcurrencyLimiter> concurrencyLimiter_;
    bool enableAccessLog_ = false;

    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);

    // HTTP Logic
    void handleHttpRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);
    void sendHttpError(const TcpConnectionPtr& conn, HttpResponse::HttpStatusCode code, const std::string& message);
    void handleStatus(const TcpConnectionPtr& conn);
    void handleMetrics(const TcpConnectionPtr& conn);
    void handleMetricsJson(const TcpConnectionPtr& conn);
    void handleDashboard(const TcpConnectionPtr& conn);
    void handleJquery(const TcpConnectionPtr& conn);
    void handleFlot(const TcpConnectionPtr& conn);
    void handleMetricsSeries(const TcpConnectionPtr& conn, const HttpRequest& req);
    void handleRpcOverHttp(const TcpConnectionPtr& conn, const HttpRequest& req, HttpContext* context);

    // RPC Logic
    void handleRpcRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);
    bool parseRequest(Buffer* buffer, RequestInfo* reqInfo);
    void sendRpcResponse(const TcpConnectionPtr& conn, google::protobuf::Message* response);
    void sendRpcResponseAndRelease(const TcpConnectionPtr& conn, google::protobuf::Message* response, std::chrono::steady_clock::time_point start_time);
    
    void registerRegistry(const InetAddress& addr);

    std::unordered_map<std::string, ServiceInfo> serviceMap_;
    std::chrono::system_clock::time_point startTime_;
};
