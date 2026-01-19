#include "MpRpcProvider.h"
#include "MpRpcApplication.h"
#include "MpRpcController.h"
#include "RpcHeader.pb.h"
#include "TcpServer.h"
#include "Logging.h"
#include <unistd.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/dynamic_message.h>
#include <functional>
#include <memory>
#include <any>
#include "http/HttpContext.h"
#include "http/HttpResponse.h"
#include "json.h" // nlohmann::json
#include "metrics/Metrics.h"
#include "Monitor.h"
#include "MonitorAssets.h"
#include "tracing/TraceContext.h"

using json = nlohmann::json;

// 辅助 Closure 类，用于将 lambda 或其他回调适配为 Protobuf Closure
class RpcClosure : public google::protobuf::Closure {
public:
    RpcClosure(std::function<void()> cb) : cb_(cb) {}
    void Run() override {
        if(cb_) cb_();
        delete this; // 自杀，因为 CallMethod 期望 Closure 在运行后自行清理
    }
private:
    std::function<void()> cb_;
};

// 注册服务：将 Service 对象及其方法描述符存入 map
void MpRpcProvider::notifyService(google::protobuf::Service* service)
{
    ServiceInfo info;
    info.service_ = service;

    const auto* serviceDesc = service->GetDescriptor();
    std::string serviceName = serviceDesc->name();
   
    LOG_INFO << "=========================================";
    LOG_INFO << "Registering Service: " << serviceName;

    int methodCnt = serviceDesc->method_count();
    for(int i = 0; i < methodCnt; i++)
    {
        const auto* methodDesc = serviceDesc->method(i);
        std::string methodName = methodDesc->name();
        auto hist = metrics::MetricsRegistry::instance().GetHistogram("rpc_server_latency_seconds", "RPC Server Latency", {{"service", serviceName}, {"method", methodName}});
        info.methodMap_.emplace(methodName, MethodContext{methodDesc, hist});
        LOG_INFO << " -> Method: " << methodName;
    }

    serviceMap_.emplace(serviceName, info);
    LOG_INFO << "=========================================";
}

// 注册到注册中心 (Registry)
void MpRpcProvider::registerRegistry(const InetAddress& addr)
{
    int clientfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientfd == -1)
    {
        LOG_ERROR << "Create socket error";
        return;
    }

    std::string registryIp = MpRpcApplication::getInstance().Load("registry_ip");
    uint16_t registryPort = atoi(MpRpcApplication::getInstance().Load("registry_port").c_str());
    if (registryIp.empty() || registryPort == 0)
    {
         registryIp = "127.0.0.1";
         registryPort = 8001;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(registryPort);
    serverAddr.sin_addr.s_addr = inet_addr(registryIp.c_str());

    if (::connect(clientfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        LOG_ERROR << "Connect registry error";
        ::close(clientfd);
        return;
    }
    
    std::string ipPort = addr.getIpPort();
    
    // 获取权重配置
    int weight = 1;
    std::string weightStr = MpRpcApplication::getInstance().Load("rpc_server_weight");
    if (!weightStr.empty()) {
        weight = atoi(weightStr.c_str());
    }

    // 发送注册消息: REG|Service|Method|IP:Port|Weight
    for(const auto& [serviceName, info] : serviceMap_)
    {
        for(const auto& [methodName, methodDesc] : info.methodMap_)
        {
            std::string msg = "REG|" + serviceName + "|" + methodName + "|" + ipPort + "|" + std::to_string(weight) + "|"; 
            ::send(clientfd, msg.data(), msg.size(), 0);
            usleep(1000); // 简单防止粘包
        }
    }
    
    // 启动心跳线程: HB|IP:Port
    std::thread([clientfd, ipPort](){
        while(true) {
            sleep(5);
            std::string hb = "HB|" + ipPort + "|";
            if (::send(clientfd, hb.data(), hb.size(), 0) == -1) {
                LOG_ERROR << "Heartbeat send failed";
                break; 
            }
        }
    }).detach();
}

// 启动服务器
void MpRpcProvider::run()
{
    startTime_ = std::chrono::system_clock::now();
    if (getenv("ENABLE_ACCESS_LOG")) {
        enableAccessLog_ = true;
    }

    auto& app = MpRpcApplication::getInstance();
    std::string ip = app.Load("rpcserverip");
    uint16_t port = atoi(app.Load("rpcserverport").c_str());

    // 初始化限流器
    int rate_limit = -1;
    std::string rateStr = app.Load("rate_limit");
    if (!rateStr.empty()) {
        rate_limit = atoi(rateStr.c_str());
    }
    LOG_INFO << "Rate limit config: " << rate_limit;
    int capacity = (rate_limit > 0) ? (rate_limit * 2) : 0;
    tokenBucket_ = std::make_unique<TokenBucket>(rate_limit, capacity);
    
    // 初始化并发限制器
    int max_concurrency = 1000;
    std::string maxConnStr = app.Load("max_concurrency");
    if (!maxConnStr.empty()) {
        max_concurrency = atoi(maxConnStr.c_str());
    }
    LOG_INFO << "Max Concurrency config: " << max_concurrency;
    concurrencyLimiter_ = std::make_unique<AutoConcurrencyLimiter>(max_concurrency);

    // 配置并启动 TcpServer
    InetAddress addr{ip, port};
    EventLoop loop;
    TcpServer server{&loop, addr, "RpcProvider", TcpServer::Option::kReusePort};
    
    server.setThreadNum(4); // 设置IO线程数
    server.setConnectionCallback([this](const TcpConnectionPtr& conn) { onConnection(conn); });
    server.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp) {
        onMessage(conn, buffer, timeStamp);
    });

    // 启动指标采集定时器 (Metrics Ticker)
    metrics::MetricsRegistry::instance().StartTicker();

    // 先启动服务器监听端口，再进行注册中心注册，避免注册阻塞导致端口未及时绑定
    server.start();
    LOG_INFO << "RpcProvider start service at ip:" << ip << " port:" << port;
    registerRegistry(addr);
    loop.loop();
}

void MpRpcProvider::onConnection(const TcpConnectionPtr& conn)
{
    if(!conn->connected()) {
        conn->shutdown();
        metrics::MetricsRegistry::instance().GetGauge("rpc_connection_count", "Current active connections", {})->Dec();
    } else {
        metrics::MetricsRegistry::instance().GetGauge("rpc_connection_count", "Current active connections", {})->Inc();
    }
}

// 核心业务处理流程
void MpRpcProvider::onMessage(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp)
{
    if (!conn->getContext().has_value()) {
        if (buffer->readableBytes() < 4) {
            return; 
        }
        
        const char* buf = buffer->peek();
        std::string method(buf, std::min(buffer->readableBytes(), (size_t)8));
        
        bool isHttp = false;
        if (method.rfind("POST", 0) == 0 || method.rfind("GET", 0) == 0 || 
            method.rfind("HEAD", 0) == 0 || method.rfind("PUT", 0) == 0 || 
            method.rfind("DELETE", 0) == 0 || method.rfind("OPTIONS", 0) == 0) {
            isHttp = true;
        }
        
        if (isHttp) {
            conn->setContext(HttpContext());
        } else {
            conn->setContext(std::string("RPC"));
        }
    }
    
    if (conn->getContext().type() == typeid(HttpContext)) {
        handleHttpRequest(conn, buffer, timeStamp);
    } else {
        handleRpcRequest(conn, buffer, timeStamp);
    }
}

void MpRpcProvider::handleHttpRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp)
{
    HttpContext* context = std::any_cast<HttpContext>(conn->getMutableContext());
    
    if (!context->parseRequest(buffer, timeStamp)) {
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();
        return;
    }

    if (context->gotAll()) {
        const HttpRequest& req = context->getRequest();
        
        std::string path = req.getPath();
        if (path == "/") {
             sendHttpError(conn, HttpResponse::HttpStatusCode::k200Ok, "Mymuduo RPC Server");
             context->reset();
             return;
        }

        if (path == "/status") {
             json j;
             j["status"] = "ok";
             j["version"] = "1.0.0";
             j["start_time"] = std::chrono::duration_cast<std::chrono::seconds>(startTime_.time_since_epoch()).count();
             
            // 连接数
            j["connection_count"] = metrics::MetricsRegistry::instance().GetGauge("rpc_connection_count", "Current active connections", {})->Value();
             
             if (concurrencyLimiter_) {
                 j["max_concurrency"] = concurrencyLimiter_->maxConcurrency();
             }
             
            // 服务列表
            j["services"] = json::array();
             for(const auto& [serviceName, info] : serviceMap_) {
                 json svc;
                 svc["name"] = serviceName;
                 svc["methods"] = json::array();
                 for(const auto& [methodName, desc] : info.methodMap_) {
                     svc["methods"].push_back(methodName);
                 }
                 j["services"].push_back(svc);
             }
             
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("application/json");
             resp.setBody(j.dump());
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/metrics") {
             std::string metrics = metrics::MetricsRegistry::instance().ToPrometheus();
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("text/plain; version=0.0.4");
             resp.setBody(metrics);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/metrics_json") {
             std::string metrics = metrics::MetricsRegistry::instance().ToJson();
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("application/json");
             resp.setBody(metrics);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/dashboard" || path == "/monitor") {
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("text/html");
             resp.setBody(kMonitorHtml);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/js/jquery_min") {
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("application/javascript");
             resp.setBody(kJqueryMin);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/js/flot_min") {
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("application/javascript");
             resp.setBody(kFlotMin);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }

        if (path == "/metrics_series") {
             std::string name = req.getQueryParam("name");
             std::string series = metrics::MetricsRegistry::instance().GetSeriesJson(name);
             HttpResponse resp(false);
             resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
             resp.setContentType("application/json");
             resp.setBody(series);
             resp.setCloseConnection(true);
             
             Buffer buf;
             resp.appendToBuffer(&buf);
             conn->send(&buf);
             conn->shutdown();

             context->reset();
             return;
        }
        
        size_t firstSlash = path.find('/', 1);
        if (firstSlash == std::string::npos) {
             sendHttpError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Invalid path. Expected /Service/Method");
             context->reset();
             return;
        }
        
        std::string serviceName = path.substr(1, firstSlash - 1);
        std::string methodName = path.substr(firstSlash + 1);
        
        auto sit = serviceMap_.find(serviceName);
        if(sit == serviceMap_.end()) {
            sendHttpError(conn, HttpResponse::HttpStatusCode::k404NotFound, "Service not found: " + serviceName);
            context->reset();
            return;
        }
        
        auto mit = sit->second.methodMap_.find(methodName);
        if(mit == sit->second.methodMap_.end()) {
            sendHttpError(conn, HttpResponse::HttpStatusCode::k404NotFound, "Method not found: " + methodName);
            context->reset();
            return;
        }
        
        google::protobuf::Service* service = sit->second.service_;
        const auto& methodCtx = mit->second;
        const auto* methodDesc = methodCtx.descriptor;
        
        google::protobuf::Message* request = service->GetRequestPrototype(methodDesc).New();
        google::protobuf::Message* response = service->GetResponsePrototype(methodDesc).New();
        
        std::string jsonBody = req.getBody().empty() ? "{}" : req.getBody();
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true;
        auto status = google::protobuf::util::JsonStringToMessage(jsonBody, request, options);
        if (!status.ok()) {
            sendHttpError(conn, HttpResponse::HttpStatusCode::k400BadRequest, "Invalid JSON: " + status.ToString());
            delete request;
            delete response;
            context->reset();
            return;
        }
        
        std::shared_ptr<google::protobuf::Message> reqPtr(request);
        std::shared_ptr<google::protobuf::Message> respPtr(response);
        
        bool close = true;
        std::string connection = req.getHeader("Connection");
        if (connection == "Keep-Alive" || connection == "keep-alive" || 
           (req.getVersion() == HttpRequest::Version::kHttp11 && connection != "close")) {
            close = false;
        }

        auto start_time = std::chrono::steady_clock::now();
        // 捕获方法上下文 (MethodContext) 的指标对象
        // ServiceInfo 生命周期足够长，直接持有其 Histogram 的指针或共享指针是安全的
        auto hist = methodCtx.latencyHistogram;

        // 分布式追踪 (Distributed Tracing)
        std::shared_ptr<mprpc::tracing::Span> span;
        std::string traceId = req.getHeader("trace_id");
        std::string spanId = req.getHeader("span_id");
        
        if (!traceId.empty()) {
             // 客户端传递了 trace_id，说明是调用链的一部分
             span = mprpc::tracing::Span::CreateServerSpan(
                 traceId,
                 "", // 我们不指定当前 span_id，由系统生成
                 spanId, 
                 serviceName + "." + methodName
             );
        } else {
             span = mprpc::tracing::Span::CreateServerSpan(
                 "", "", "",
                 serviceName + "." + methodName
             );
        }
        
        // RAII helper
        struct ScopedSpan {
            std::shared_ptr<mprpc::tracing::Span> span_;
            std::shared_ptr<mprpc::tracing::Span> prev_span_;
            ScopedSpan(std::shared_ptr<mprpc::tracing::Span> s) : span_(s) {
                prev_span_ = mprpc::tracing::TraceContext::GetCurrentSpan();
                mprpc::tracing::TraceContext::SetCurrentSpan(span_);
            }
            ~ScopedSpan() {
                mprpc::tracing::TraceContext::SetCurrentSpan(prev_span_);
            }
        };
        ScopedSpan scoped_span(span);

        if (span) {
            span->SetRequestSize(req.getBody().size()); // Approx payload size
            if (conn->connected()) {
                 span->SetRemoteSide(conn->getPeerAddr().getIpPort());
            }
        }
        
        google::protobuf::Closure* done = new RpcClosure(
            [conn, respPtr, reqPtr, close, start_time, hist, span]() {
                auto end_time = std::chrono::steady_clock::now();
                int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
                hist->Observe(latency_us / 1000000.0);

                std::string responseJson;
                google::protobuf::util::JsonPrintOptions printOptions;
                printOptions.add_whitespace = true;
                printOptions.always_print_primitive_fields = true;
                auto status = google::protobuf::util::MessageToJsonString(*respPtr, &responseJson, printOptions);
                
                HttpResponse resp(true);
                if (!status.ok()) {
                    resp.setStatusCode(HttpResponse::HttpStatusCode::k500InternalServerError);
                    resp.setBody("{\"error\": \"Response serialization failed\"}");
                } else {
                    resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
                    resp.setContentType("application/json");
                    resp.setBody(responseJson);
                }
                
                if (close) resp.setCloseConnection(true);
                
                Buffer buf;
                resp.appendToBuffer(&buf);
                conn->send(&buf);
                if (resp.closeConnection()) conn->shutdown();
                
                if (span) {
                    // HTTP response size approx
                    span->SetResponseSize(buf.readableBytes());
                    span->End();
                }
            }
        );
        
        MpRpcController controller;
        service->CallMethod(methodDesc, &controller, reqPtr.get(), respPtr.get(), done);
        
        context->reset();
    }
}

void MpRpcProvider::sendHttpError(const TcpConnectionPtr& conn, HttpResponse::HttpStatusCode code, const std::string& message)
{
    HttpResponse resp(false);
    resp.setStatusCode(code);
    resp.setStatusMessage("Error");
    resp.setContentType("application/json");
    
    json j;
    j["error"] = message;
    resp.setBody(j.dump());
    resp.setCloseConnection(true);
    
    Buffer buffer;
    resp.appendToBuffer(&buffer);
    conn->send(&buffer);
    conn->shutdown();
}

void MpRpcProvider::handleRpcRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp)
{
    while (buffer->readableBytes() > 0) {
        // 1. 解析请求
        RequestInfo reqInfo;
        if (!parseRequest(buffer, &reqInfo)) {
            // 请求不完整或存在解析错误
            break;
        }

        // 2. 限流检查 (Token Bucket)
        if (tokenBucket_ && !tokenBucket_->consume(1)) {
            LOG_WARN << "Rate limit exceeded from " << conn->getPeerAddr().getIpPort();
            // ... (Error handling logic)
            // 达到限流阈值，拒绝请求
            // 为了简化实现，这里暂时不发送具体的错误响应，或者发送通用的错误响应
            // 实际生产中应该返回一个特定的错误码
            
            // 在此次重构中，我们尝试构建一个包含错误信息的响应并返回
             auto sit = serviceMap_.find(reqInfo.service_name);
             if(sit != serviceMap_.end()) {
                auto mit = sit->second.methodMap_.find(reqInfo.method_name);
                if(mit != sit->second.methodMap_.end()) {
                    google::protobuf::Service* service = sit->second.service_;
                    const auto& methodCtx = mit->second;
                    const auto* methodDesc = methodCtx.descriptor;
                    google::protobuf::Message* response = service->GetResponsePrototype(methodDesc).New();
                    
                    const google::protobuf::Reflection* reflection = response->GetReflection();
                    const google::protobuf::Descriptor* descriptor = response->GetDescriptor();
                    const google::protobuf::FieldDescriptor* resultField = descriptor->FindFieldByName("result");
                    
                    if (resultField) {
                        google::protobuf::Message* resultMsg = reflection->MutableMessage(response, resultField);
                        const google::protobuf::Reflection* resultRefl = resultMsg->GetReflection();
                        const google::protobuf::Descriptor* resultDesc = resultMsg->GetDescriptor();
                        
                        const google::protobuf::FieldDescriptor* errcodeField = resultDesc->FindFieldByName("errcode");
                        const google::protobuf::FieldDescriptor* errmsgField = resultDesc->FindFieldByName("errmsg");
                        
                        if (errcodeField && errmsgField) {
                            resultRefl->SetInt32(resultMsg, errcodeField, 429); // 429 Too Many Requests
                            resultRefl->SetString(resultMsg, errmsgField, "Rate limit exceeded");
                        }
                    }
                    
                    // Send
                    sendRpcResponse(conn, response);
                    delete response;
                }
             }
             
             // 即使无法构建完美响应，也应停止处理该请求
             continue; 
        }

        // 3. 最大并发检查
        if (concurrencyLimiter_ && !concurrencyLimiter_->acquire()) {
             LOG_WARN << "Max concurrency exceeded";
             // Send error or close
             // Similar to rate limit
             conn->shutdown();
             return;
        }
        
        auto start_time = std::chrono::steady_clock::now();

        // 4. 查找服务和方法
        auto sit = serviceMap_.find(reqInfo.service_name);
        if(sit == serviceMap_.end())
        {
            LOG_WARN << reqInfo.service_name << " does not exist!";
            if(concurrencyLimiter_) concurrencyLimiter_->release(0);
            continue;
        }
        auto mit = sit->second.methodMap_.find(reqInfo.method_name);
        if(mit == sit->second.methodMap_.end())
        {
            LOG_WARN << reqInfo.method_name << " does not exist!";
            if(concurrencyLimiter_) concurrencyLimiter_->release(0);
            continue;
        }

        google::protobuf::Service* service = sit->second.service_;
        const auto& methodCtx = mit->second;
        const auto* methodDesc = methodCtx.descriptor;

        // 5. 反序列化 Request
        google::protobuf::Message* request = service->GetRequestPrototype(methodDesc).New();
        if(!request->ParseFromString(reqInfo.args_str))
        {
            LOG_WARN << "Request parse error";
            if(concurrencyLimiter_) concurrencyLimiter_->release(0);
            delete request;
            continue;
        }

        // 6. 创建 Response 对象
        google::protobuf::Message* response = service->GetResponsePrototype(methodDesc).New();

        // 7. 绑定回调
        std::string service_name = reqInfo.service_name;
        std::string method_name = reqInfo.method_name;
        
        if (enableAccessLog_) {
            LOG_INFO << "doing local service: " << method_name;
            // 为依赖日志捕获的测试强制刷新缓冲区
            fflush(stdout);
        }

        auto hist = methodCtx.latencyHistogram;
        
        // 分布式追踪 (Distributed Tracing)
        std::shared_ptr<mprpc::tracing::Span> span;
        if (reqInfo.meta_data.count("trace_id")) {
             // 客户端传递了 trace_id，说明是调用链的一部分
             // 客户端的 span_id 即为服务端的 parent_span_id
             span = mprpc::tracing::Span::CreateServerSpan(
                 reqInfo.meta_data["trace_id"],
                 "", // 我们不指定当前 span_id，由系统生成
                 reqInfo.meta_data["span_id"], 
                 reqInfo.service_name + "." + reqInfo.method_name
             );
        } else {
             // 根 Span
             span = mprpc::tracing::Span::CreateServerSpan(
                 "", "", "",
                 reqInfo.service_name + "." + reqInfo.method_name
             );
        }
        
        // RAII helper
        struct ScopedSpan {
            std::shared_ptr<mprpc::tracing::Span> span_;
            std::shared_ptr<mprpc::tracing::Span> prev_span_;
            ScopedSpan(std::shared_ptr<mprpc::tracing::Span> s) : span_(s) {
                prev_span_ = mprpc::tracing::TraceContext::GetCurrentSpan();
                mprpc::tracing::TraceContext::SetCurrentSpan(span_);
            }
            ~ScopedSpan() {
                mprpc::tracing::TraceContext::SetCurrentSpan(prev_span_);
            }
        };
        ScopedSpan scoped_span(span);

        if (span) {
            span->SetRequestSize(reqInfo.args_size);
            if (conn->connected()) {
                 span->SetRemoteSide(conn->getPeerAddr().getIpPort());
            }
        }
        
        google::protobuf::Closure* done = new RpcClosure(
            [this, conn, response, start_time, hist, span]() {
                auto end_time = std::chrono::steady_clock::now();
                int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
                hist->Observe(latency_us / 1000000.0);

                this->sendRpcResponseAndRelease(conn, response, start_time);
                if (span) {
                    span->SetResponseSize(response->ByteSizeLong());
                    span->End();
                }
            }
        );      
        
        // 8. 执行
        MpRpcController controller;
        for(const auto& pair : reqInfo.meta_data) {
            controller.SetMetadata(pair.first, pair.second);
        }

        service->CallMethod(methodDesc, &controller, request, response, done);
        
        // Context is cleared by ScopedSpan destructor
    }
}

// 协议解析: HeadSize(4字节) + Header(Protobuf) + Args(Protobuf)
bool MpRpcProvider::parseRequest(Buffer* buffer, RequestInfo* reqInfo)
{
    if (buffer->readableBytes() < 4) return false;
    
    uint32_t header_size = 0;
    const char* data = buffer->peek();
    memcpy(&header_size, data, 4);
    
    if (buffer->readableBytes() < 4 + header_size) return false;
    
    std::string header_str(data + 4, header_size);
    mprpc::RpcHeader header;
    if(!header.ParseFromString(header_str))
    {
        LOG_WARN << "rpc_header_str parse error!";
        buffer->retrieveAll();
        // 不宜直接在此处关闭连接；返回 false 通常表示“需要更多数据”
        // 这里消费掉错误数据以避免阻塞
        return false;
    }
    
    uint32_t args_size = header.args_size();
    if (buffer->readableBytes() < 4 + header_size + args_size) return false;
    
    // 已接收完整数据包
    buffer->retrieve(4 + header_size);
    reqInfo->args_str = buffer->retrieveAsString(args_size);
    
    reqInfo->service_name = header.service_name();
    reqInfo->method_name = header.method_name();
    reqInfo->args_size = args_size;
    for(auto& pair : header.meta_data()) {
        reqInfo->meta_data[pair.first] = pair.second;
    }
    return true;
}

// 发送响应并清理资源
void MpRpcProvider::sendRpcResponseAndRelease(const TcpConnectionPtr& conn, google::protobuf::Message* response, std::chrono::steady_clock::time_point start_time)
{
    sendRpcResponse(conn, response);
    
    if (concurrencyLimiter_) {
        auto end_time = std::chrono::steady_clock::now();
        int64_t latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        concurrencyLimiter_->release(latency);
    }
}

void MpRpcProvider::sendRpcResponse(const TcpConnectionPtr& conn, google::protobuf::Message* response)
{
    std::string response_str;
    if(response->SerializeToString(&response_str))
    {
        // 添加长度前缀 (帧头)
        uint32_t len = response_str.size();
        std::string send_str;
        send_str.insert(0, std::string((char*)&len, 4));
        send_str += response_str;
        conn->send(send_str);
    }
    else
    {
        LOG_WARN << "serialize response_str error!";
    }
    // 保持连接存活，不主动关闭
}
