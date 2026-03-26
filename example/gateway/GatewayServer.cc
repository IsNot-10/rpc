#include "HttpServer.h"
#include "HttpResponse.h"
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "json.h"
#include "Logging.h"
#include "GatewayComponents.h" // Includes ThreadPool, TokenBucket, CircuitBreaker, RpcUtils

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <iostream>
#include <string>
#include <memory>

using json = nlohmann::json;

// --- 网关服务器 ---
class GatewayServer {
public:
    GatewayServer(EventLoop* loop, const InetAddress& addr)
        : server_(loop, addr, "GatewayServer", TcpServer::Option::kReusePort), pool_(4), rateLimiter_(5000, 10000) { // Increase threads to 50 for blocking RPCs, Increase RateLimit to 5000 QPS
        
        // 注册 HTTP 路由通用路由
        server_.POST("/api/:service/:method", [this](const HttpRequest& req, const TcpConnectionPtr& conn) {
            this->handleGenericRpc(req, conn);
        });
    }

    void start() {
        server_.start();
    }

private:
    void handleGenericRpc(const HttpRequest& req, const TcpConnectionPtr& conn) {
        // 限流检查
        if (!rateLimiter_.consume()) {
            LOG_WARN << "Rate Limit Exceeded for " << req.getPath();
            sendError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Rate Limit Exceeded (Gateway)", req);
            return;
        }

        // 1. 从路径中提取服务和方法 (由 Router 注入)
        std::string serviceName = req.getPathParam("service");
        std::string methodName = req.getPathParam("method");
        
        if (serviceName.empty() || methodName.empty()) {
            sendError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Invalid Service/Method", req);
            return;
        }

        // 尝试添加前缀以匹配熔断器 Key
        std::string fullServiceName = serviceName;
        if (serviceName.find('.') == std::string::npos) {
            fullServiceName = "fixbug." + serviceName;
        }

        // 熔断检查
        if (!circuitBreaker_.allowRequest(fullServiceName)) {
             LOG_WARN << "Circuit Breaker Blocked Request for " << fullServiceName;
             sendError(conn, HttpResponse::HttpStatusCode::k503ServiceUnavailable, "Service Unavailable (Circuit Breaker)", req);
             return;
        }
        
        LOG_INFO << "Received Generic RPC Request: " << req.getPath();
        
        if (req.getMethod() != HttpRequest::Method::kPost) {
            sendError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Only POST allowed", req);
            return;
        }

        // 2. 读取 Body
        std::string requestBody = req.getBody();
        
        // 3. 提取 Headers (用于透传)
        std::map<std::string, std::string> headers;
        headers["Authorization"] = req.getHeader("Authorization");
        headers["X-Request-ID"] = req.getHeader("X-Request-ID");

        // 4. 提交到线程池以避免阻塞 EventLoop
        pool_.addTask([this, conn, fullServiceName, methodName, requestBody, headers, req_ver=req.getVersion(), req_conn=req.getHeader("Connection")]() {
             this->doGenericRpc(conn, fullServiceName, methodName, requestBody, headers, req_ver, req_conn);
        });
    }

    void doGenericRpc(const TcpConnectionPtr& conn, 
                      std::string serviceName, 
                      std::string methodName, 
                      std::string jsonBody,
                      std::map<std::string, std::string> headers,
                      HttpRequest::Version version,
                      std::string connectionHeader) {
        
        // 5. 查找描述符
        const google::protobuf::MethodDescriptor* methodDesc = RpcUtils::GetMethodDescriptor(serviceName, methodName);
        if (!methodDesc) {
            if (serviceName.find('.') == std::string::npos) {
                serviceName = "fixbug." + serviceName;
                methodDesc = RpcUtils::GetMethodDescriptor(serviceName, methodName);
            }
            
            if (!methodDesc) {
                 sendError(conn, HttpResponse::HttpStatusCode::k404NotFound, "Service/Method not found", HttpRequest(), version, connectionHeader);
                 return;
            }
        }

        // 6. 动态创建请求/响应消息
        google::protobuf::DynamicMessageFactory messageFactory;
        const google::protobuf::Message* requestPrototype = messageFactory.GetPrototype(methodDesc->input_type());
        const google::protobuf::Message* responsePrototype = messageFactory.GetPrototype(methodDesc->output_type());
        
        std::unique_ptr<google::protobuf::Message> request(requestPrototype->New());
        std::unique_ptr<google::protobuf::Message> response(responsePrototype->New());

        // 7. JSON -> Protobuf
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true;
        auto status = google::protobuf::util::JsonStringToMessage(jsonBody, request.get(), options);
        if (!status.ok()) {
            LOG_ERROR << "JsonStringToMessage failed: " << status.ToString();
            sendError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Invalid JSON Body", HttpRequest(), version, connectionHeader);
            return;
        }

        // 8. 发起 RPC 调用
        MpRpcChannel channel;
        MpRpcController controller;
        
        // 设置透传的 Headers
        for (const auto& h : headers) {
            if (!h.second.empty()) {
                controller.SetMetadata(h.first, h.second);
            }
        }

        channel.CallMethod(methodDesc, &controller, request.get(), response.get(), nullptr);
        
        if (controller.Failed()) {
            LOG_ERROR << "RPC Failed: " << controller.ErrorText();
            // 记录失败 (熔断器)
            circuitBreaker_.recordFailure(methodDesc->service()->name());
            sendError(conn, HttpResponse::HttpStatusCode::k500InternalServerError, "RPC Failed: " + controller.ErrorText(), HttpRequest(), version, connectionHeader);
        } else {
            // 记录成功
            circuitBreaker_.recordSuccess(methodDesc->service()->name());
            
            // 9. Protobuf -> JSON
            std::string responseJson;
            google::protobuf::util::JsonPrintOptions printOptions;
            printOptions.add_whitespace = true;
            printOptions.always_print_primitive_fields = true;
            status = google::protobuf::util::MessageToJsonString(*response, &responseJson, printOptions);
            
            if (!status.ok()) {
                LOG_ERROR << "MessageToJsonString failed: " << status.ToString();
                sendError(conn, HttpResponse::HttpStatusCode::k500InternalServerError, "Response Serialization Failed", HttpRequest(), version, connectionHeader);
                return;
            }
            
            LOG_INFO << "RPC Success. Response: " << response->ShortDebugString();

            HttpResponse resp(true);
            resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
            resp.setStatusMessage("OK");
            resp.setContentType("application/json");
            resp.setBody(responseJson);
            // resp.setVersion(version);
            
            if (connectionHeader == "close" || version == HttpRequest::Version::kHttp10) {
                 resp.setCloseConnection(true);
            }

            Buffer buffer;
            resp.appendToBuffer(&buffer);
            conn->send(&buffer);
        }
    }

    void sendError(const TcpConnectionPtr& conn, HttpResponse::HttpStatusCode code, const std::string& message, const HttpRequest& req, 
                   HttpRequest::Version version = HttpRequest::Version::kHttp11, std::string connectionHeader = "") {
        HttpResponse resp(false);
        resp.setStatusCode(code);
        resp.setStatusMessage("Error");
        resp.setContentType("application/json");
        
        json j;
        j["error"] = message;
        resp.setBody(j.dump());
        
        // resp.setVersion(version);
        if (connectionHeader == "close" || version == HttpRequest::Version::kHttp10) {
             resp.setCloseConnection(true);
        }

        Buffer buffer;
        resp.appendToBuffer(&buffer);
        conn->send(&buffer);
        
        // 错误时强制断开连接? 不一定，取决于 Keep-Alive
        if (resp.closeConnection()) {
             conn->shutdown();
        }
    }

    HttpServer server_;
    ThreadPool pool_;
    TokenBucket rateLimiter_;
    CircuitBreaker circuitBreaker_;
};

int main(int argc, char* argv[]) {
    // Init RPC Application
    MpRpcApplication::getInstance().Init(argc, argv);
    
    // 初始化 Proto 导入器 (指向存放 .proto 文件的目录)
    const char* proto_path_env = std::getenv("PROTO_PATH");
    std::string proto_path = proto_path_env ? proto_path_env : "/home/abab/muduo-x/rpc/example/rpc/protos";
    RpcUtils::Init(proto_path);

    std::string portStr = MpRpcApplication::getInstance().Load("gateway_port");
    uint16_t port = portStr.empty() ? 8080 : std::stoi(portStr);

    EventLoop loop;
    InetAddress addr("0.0.0.0", port);
    GatewayServer gateway(&loop, addr);

    LOG_INFO << "Gateway Server starting on 0.0.0.0:" << port;
    gateway.start();
    loop.loop();
    return 0;
}
