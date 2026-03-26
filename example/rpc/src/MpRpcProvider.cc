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
#include "json.h"
#include "metrics/Metrics.h"
#include "Monitor.h"
#include "MonitorAssets.h"
#include "tracing/TraceContext.h"

using json = nlohmann::json;

// =================================================================================
// Helper Classes (Moved from Header)
// =================================================================================

class MpRpcProvider::TokenBucket {
public:
    TokenBucket(int rate, int capacity) 
        : rate_(rate), capacity_(capacity), tokens_(capacity), last_refill_time_(std::chrono::steady_clock::now()) {}

    bool consume(int n = 1) {
        if (rate_ <= 0) return true;

        std::lock_guard<std::mutex> lock(mutex_);
        refill();
        if (tokens_ >= n) {
            tokens_ -= n;
            return true;
        }
        return false;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refill_time_).count();
        if (duration > 0) {
            int new_tokens = (duration * rate_) / 1000;
            if (new_tokens > 0) {
                tokens_ = std::min(capacity_, tokens_ + new_tokens);
                last_refill_time_ = now;
            }
        }
    }

    int rate_;
    int capacity_;
    int tokens_;
    std::chrono::steady_clock::time_point last_refill_time_;
    std::mutex mutex_;
};

class MpRpcProvider::AutoConcurrencyLimiter {
public:
    AutoConcurrencyLimiter(int initial_max_concurrency = 40)
        : max_concurrency_(initial_max_concurrency)
        , active_req_(0)
        , min_latency_us_(-1)
        , ema_max_qps_(-1)
        , last_sampling_time_us_(0)
        , next_reset_time_us_(0)
        , samples_count_(0)
        , samples_latency_sum_(0)
    {
         last_sampling_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
         next_reset_time_us_ = last_sampling_time_us_ + 10 * 1000 * 1000;
    }

    bool acquire() {
        int current = active_req_.fetch_add(1, std::memory_order_relaxed);
        int max_c = max_concurrency_.load(std::memory_order_relaxed);
        if (current >= max_c) {
            active_req_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void release(int64_t latency_us) {
        active_req_.fetch_sub(1, std::memory_order_relaxed);
        updateStats(latency_us);
    }

    int maxConcurrency() const { return max_concurrency_; }

private:
    void updateStats(int64_t latency_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (latency_us <= 0) return;

        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (min_latency_us_ == -1 || latency_us < min_latency_us_) {
            min_latency_us_ = latency_us;
        }

        samples_count_++;
        samples_latency_sum_ += latency_us;

        if (now_us - last_sampling_time_us_ >= 500 * 1000) {
            double interval_s = (now_us - last_sampling_time_us_) / 1000000.0;
            double current_qps = samples_count_ / interval_s;

            if (ema_max_qps_ == -1) {
                ema_max_qps_ = current_qps;
            } else if (current_qps > ema_max_qps_) {
                ema_max_qps_ = current_qps;
            } else {
                ema_max_qps_ = ema_max_qps_ * 0.9 + current_qps * 0.1;
            }

            if (min_latency_us_ > 0) {
                double min_latency_s = min_latency_us_ / 1000000.0;
                double new_limit = ema_max_qps_ * min_latency_s * 1.5;
                
                new_limit = std::max(5.0, new_limit);
                
                int old_limit = max_concurrency_.load();
                int next_limit = (int)(old_limit * 0.7 + new_limit * 0.3);
                
                max_concurrency_.store(next_limit);
            }

            samples_count_ = 0;
            samples_latency_sum_ = 0;
            last_sampling_time_us_ = now_us;
        }

        if (now_us > next_reset_time_us_) {
            int current_limit = max_concurrency_.load();
            max_concurrency_.store(std::max(5, current_limit / 2));
            min_latency_us_ = -1; 
            next_reset_time_us_ = now_us + 10 * 1000 * 1000;
        }
    }

    std::atomic<int> max_concurrency_;
    std::atomic<int> active_req_;
    
    int64_t min_latency_us_;
    double ema_max_qps_;
    
    int64_t last_sampling_time_us_;
    int64_t next_reset_time_us_;
    
    int samples_count_ = 0;
    int64_t samples_latency_sum_ = 0;
    
    std::mutex mutex_;
};

// =================================================================================
// MpRpcProvider Implementation
// =================================================================================

class RpcClosure : public google::protobuf::Closure {
public:
    RpcClosure(std::function<void()> cb) : cb_(cb) {}
    
    void Run() override {
        if(cb_) cb_();
        delete this;
    }
private:
    std::function<void()> cb_;
};

MpRpcProvider::MpRpcProvider() = default;
MpRpcProvider::~MpRpcProvider() = default;

void MpRpcProvider::notifyService(google::protobuf::Service* service)
{
    ServiceInfo info;
    info.service_ = service;

    const auto* serviceDesc = service->GetDescriptor();
    std::string serviceName = serviceDesc->name();
   
    LOG_INFO << "Registering Service: " << serviceName;

    int methodCnt = serviceDesc->method_count();
    for(int i = 0; i < methodCnt; i++)
    {
        const auto* methodDesc = serviceDesc->method(i);
        std::string methodName = methodDesc->name();
        
        auto hist = metrics::MetricsRegistry::instance().GetHistogram(
            "rpc_server_latency_seconds", 
            "RPC Server Latency", 
            {{"service", serviceName}, {"method", methodName}}
        );
        
        info.methodMap_.emplace(methodName, MethodContext{methodDesc, hist});
        LOG_INFO << " -> Method: " << methodName;
    }

    serviceMap_.emplace(serviceName, info);
}

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

    // Retry connection logic
    int retries = 0;
    while (::connect(clientfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        if (retries >= 10) {
             fprintf(stderr, "MpRpcProvider: Connect registry error after 10 retries\n");
             LOG_ERROR << "Connect registry error after 10 retries";
             ::close(clientfd);
             return;
        }
        fprintf(stderr, "MpRpcProvider: Connect registry failed, retrying in 0.5s... (Attempt %d)\n", retries+1);
        LOG_WARN << "Connect registry failed, retrying in 0.5s...";
        usleep(500000);
        retries++;
    }
    
    fprintf(stderr, "MpRpcProvider: Connected to registry at %s:%d\n", registryIp.c_str(), registryPort);
    std::string ipPort = addr.getIpPort();
    
    int weight = 1;
    std::string weightStr = MpRpcApplication::getInstance().Load("rpc_server_weight");
    if (!weightStr.empty()) {
        weight = atoi(weightStr.c_str());
    }

    for(const auto& [serviceName, info] : serviceMap_)
    {
        for(const auto& [methodName, methodDesc] : info.methodMap_)
        {
            std::string msg = "REG|" + serviceName + "|" + methodName + "|" + ipPort + "|" + std::to_string(weight) + "|\n";
            if (::send(clientfd, msg.data(), msg.size(), 0) == -1) {
                fprintf(stderr, "MpRpcProvider: Failed to send registration for %s/%s\n", serviceName.c_str(), methodName.c_str());
            } else {
                fprintf(stderr, "MpRpcProvider: Sent registration for %s/%s\n", serviceName.c_str(), methodName.c_str());
            }
            usleep(1000);
        }
    }
    
    std::thread([clientfd, ipPort](){
        while(true) {
            sleep(5);
            std::string hb = "HB|" + ipPort + "|\n";
            if (::send(clientfd, hb.data(), hb.size(), 0) == -1) {
                LOG_ERROR << "Heartbeat send failed";
                break; 
            }
        }
    }).detach();
}

void MpRpcProvider::run()
{
    startTime_ = std::chrono::system_clock::now();
    
    if (getenv("ENABLE_ACCESS_LOG")) {
        enableAccessLog_ = true;
    }

    auto& app = MpRpcApplication::getInstance();
    std::string ip = app.Load("rpcserverip");
    uint16_t port = atoi(app.Load("rpcserverport").c_str());

    int rate_limit = -1;
    std::string rateStr = app.Load("rate_limit");
    if (!rateStr.empty()) {
        rate_limit = atoi(rateStr.c_str());
    }
    LOG_INFO << "Rate limit config: " << rate_limit;
    
    int capacity = (rate_limit > 0) ? (rate_limit * 2) : 0;
    tokenBucket_ = std::make_unique<TokenBucket>(rate_limit, capacity);
    
    int max_concurrency = 1000;
    std::string maxConnStr = app.Load("max_concurrency");
    if (!maxConnStr.empty()) {
        max_concurrency = atoi(maxConnStr.c_str());
    }
    LOG_INFO << "Max Concurrency config: " << max_concurrency;
    concurrencyLimiter_ = std::make_unique<AutoConcurrencyLimiter>(max_concurrency);

    InetAddress addr{ip, port};
    EventLoop loop;
    TcpServer server{&loop, addr, "RpcProvider", TcpServer::Option::kReusePort};
    
    server.setThreadNum(4);
    
    server.setConnectionCallback([this](const TcpConnectionPtr& conn) { onConnection(conn); });
    
    server.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp) {
        onMessage(conn, buffer, timeStamp);
    });

    metrics::MetricsRegistry::instance().StartTicker();

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

        if (path == "/status") { handleStatus(conn); context->reset(); return; }
        if (path == "/metrics") { handleMetrics(conn); context->reset(); return; }
        if (path == "/metrics_json") { handleMetricsJson(conn); context->reset(); return; }
        if (path == "/dashboard" || path == "/monitor") { handleDashboard(conn); context->reset(); return; }
        if (path == "/js/jquery_min") { handleJquery(conn); context->reset(); return; }
        if (path == "/js/flot_min") { handleFlot(conn); context->reset(); return; }
        if (path == "/metrics_series") { handleMetricsSeries(conn, req); context->reset(); return; }
        
        handleRpcOverHttp(conn, req, context);
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

void MpRpcProvider::handleStatus(const TcpConnectionPtr& conn)
{
    json j;
    j["status"] = "ok";
    j["version"] = "1.0.0";
    j["start_time"] = std::chrono::duration_cast<std::chrono::seconds>(startTime_.time_since_epoch()).count();
    
    j["connection_count"] = metrics::MetricsRegistry::instance().GetGauge("rpc_connection_count", "Current active connections", {})->Value();
    
    if (concurrencyLimiter_) {
        j["max_concurrency"] = concurrencyLimiter_->maxConcurrency();
    }
    
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
}

void MpRpcProvider::handleMetrics(const TcpConnectionPtr& conn)
{
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
}

void MpRpcProvider::handleMetricsJson(const TcpConnectionPtr& conn)
{
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
}

void MpRpcProvider::handleDashboard(const TcpConnectionPtr& conn)
{
    HttpResponse resp(false);
    resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
    resp.setContentType("text/html");
    resp.setBody(kMonitorHtml);
    resp.setCloseConnection(true);
    
    Buffer buf;
    resp.appendToBuffer(&buf);
    conn->send(&buf);
    conn->shutdown();
}

void MpRpcProvider::handleJquery(const TcpConnectionPtr& conn)
{
    HttpResponse resp(false);
    resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
    resp.setContentType("application/javascript");
    resp.setBody(kJqueryMin);
    resp.setCloseConnection(true);
    
    Buffer buf;
    resp.appendToBuffer(&buf);
    conn->send(&buf);
    conn->shutdown();
}

void MpRpcProvider::handleFlot(const TcpConnectionPtr& conn)
{
    HttpResponse resp(false);
    resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
    resp.setContentType("application/javascript");
    resp.setBody(kFlotMin);
    resp.setCloseConnection(true);
    
    Buffer buf;
    resp.appendToBuffer(&buf);
    conn->send(&buf);
    conn->shutdown();
}

void MpRpcProvider::handleMetricsSeries(const TcpConnectionPtr& conn, const HttpRequest& req)
{
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
}

void MpRpcProvider::handleRpcOverHttp(const TcpConnectionPtr& conn, const HttpRequest& req, HttpContext* context)
{
    std::string path = req.getPath();
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
    auto hist = methodCtx.latencyHistogram;

    std::shared_ptr<mprpc::tracing::Span> span;
    std::string traceId = req.getHeader("trace_id");
    std::string spanId = req.getHeader("span_id");
    
    if (!traceId.empty()) {
         span = mprpc::tracing::Span::CreateServerSpan(
             traceId,
             "", 
             spanId, 
             serviceName + "." + methodName
         );
    } else {
         span = mprpc::tracing::Span::CreateServerSpan(
             "", "", "",
             serviceName + "." + methodName
         );
    }
    
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
        span->SetRequestSize(req.getBody().size());
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
                span->SetResponseSize(buf.readableBytes());
                span->End();
            }
        }
    );
    
    MpRpcController controller;
    service->CallMethod(methodDesc, &controller, reqPtr.get(), respPtr.get(), done);
    
    context->reset();
}

void MpRpcProvider::handleRpcRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp)
{
    while (buffer->readableBytes() > 0) {
        RequestInfo reqInfo;
        if (!parseRequest(buffer, &reqInfo)) {
            break;
        }

        if (tokenBucket_ && !tokenBucket_->consume(1)) {
            LOG_WARN << "Rate limit exceeded from " << conn->getPeerAddr().getIpPort();
            
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
                            resultRefl->SetInt32(resultMsg, errcodeField, 429);
                            resultRefl->SetString(resultMsg, errmsgField, "Rate limit exceeded");
                        }
                    }
                    
                    sendRpcResponse(conn, response);
                    delete response;
                }
             }
             
             continue; 
        }

        if (concurrencyLimiter_ && !concurrencyLimiter_->acquire()) {
             LOG_WARN << "Max concurrency exceeded";
             conn->shutdown();
             return;
        }
        
        auto start_time = std::chrono::steady_clock::now();

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

        google::protobuf::Message* request = service->GetRequestPrototype(methodDesc).New();
        if(!request->ParseFromString(reqInfo.args_str))
        {
            LOG_WARN << "Request parse error";
            if(concurrencyLimiter_) concurrencyLimiter_->release(0);
            delete request;
            continue;
        }

        google::protobuf::Message* response = service->GetResponsePrototype(methodDesc).New();

        std::string service_name = reqInfo.service_name;
        std::string method_name = reqInfo.method_name;
        
        if (enableAccessLog_) {
            LOG_INFO << "doing local service: " << method_name;
            fflush(stdout);
        }

        auto hist = methodCtx.latencyHistogram;
        
        std::shared_ptr<mprpc::tracing::Span> span;
        if (reqInfo.meta_data.count("trace_id")) {
             span = mprpc::tracing::Span::CreateServerSpan(
                 reqInfo.meta_data["trace_id"],
                 "", 
                 reqInfo.meta_data["span_id"], 
                 reqInfo.service_name + "." + reqInfo.method_name
             );
             LOG_INFO << "Received Request: " << reqInfo.service_name << "." << reqInfo.method_name 
                      << " from " << conn->getPeerAddr().getIpPort() 
                      << " [TraceID: " << reqInfo.meta_data["trace_id"] << "]";
        } else {
             span = mprpc::tracing::Span::CreateServerSpan(
                 "", "", "",
                 reqInfo.service_name + "." + reqInfo.method_name
             );
             LOG_INFO << "Received Request: " << reqInfo.service_name << "." << reqInfo.method_name 
                      << " from " << conn->getPeerAddr().getIpPort() 
                      << " [No TraceID]";
        }
        
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
        
        MpRpcController controller;
        for(const auto& pair : reqInfo.meta_data) {
            controller.SetMetadata(pair.first, pair.second);
        }

        service->CallMethod(methodDesc, &controller, request, response, done);
    }
}

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
        return false;
    }
    
    uint32_t args_size = header.args_size();
    if (buffer->readableBytes() < 4 + header_size + args_size) return false;
    
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
}
