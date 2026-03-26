#include "MpRpcChannel.h"
#include "ServiceDiscovery.h"
#include "ha/circuit_breaker.h"
#include "ha/concurrency_limiter.h"
#include "RpcHeader.pb.h"
#include "MpRpcApplication.h"
#include "MpRpcController.h"
#include "Logging.h"
#include "ConnectionPool.h"
#include "tracing/TraceContext.h"
#include "lb/load_balancer.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

namespace {

struct RequestContext {
    int fd = -1;
    std::string host_key;
    std::string ip;
    uint16_t port;
    bool sent = false;
};

bool readN(int fd, char* buf, int n) {
    int total = 0;
    while (total < n) {
        int ret;
        do {
            ret = ::recv(fd, buf + total, n - total, 0);
        } while (ret < 0 && errno == EINTR);
        
        if (ret <= 0) return false;
        total += ret;
    }
    return true;
}

std::string normalizeHostKey(const std::string& s) {
    size_t last = s.rfind(':');
    if (last == std::string::npos) return s;
    size_t prev = (last > 0) ? s.rfind(':', last - 1) : std::string::npos;
    if (prev != std::string::npos) {
        return s.substr(0, last);
    }
    return s;
}

void cleanupRequest(const RequestContext& ctx, bool success) {
    if (ctx.fd == -1) return;
    
    if (success) {
        ConnectionPool::instance().releaseConnection(ctx.ip, ctx.port, ctx.fd);
    } else {
        ConnectionPool::instance().closeConnection(ctx.fd);
    }
    
    if (ctx.sent) {
        ha::ConcurrencyLimiter::instance().dec(ctx.host_key);
    }
}

void reportStats(const std::shared_ptr<LoadBalancer>& lb, 
                const SelectIn& selectIn, 
                const RequestContext& ctx, 
                bool success, 
                int64_t latency_ms, 
                int rpc_timeout_ms,
                int attempt) 
{
    ha::CircuitBreaker::instance().report_status(ctx.host_key, success, success ? latency_ms : rpc_timeout_ms);
    
    CallInfo info;
    info.service_key = selectIn.service_key;
    info.host = ctx.host_key;
    info.success = success;
    info.begin_time_us = selectIn.begin_time_us;
    info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    info.timeout_ms = rpc_timeout_ms;
    info.retried_count = attempt;
    lb->feedback(info);
}

bool attemptSend(const std::shared_ptr<LoadBalancer>& lb,
                 const SelectIn& selectIn,
                 const std::string& send_str,
                 RequestContext& ctx,
                 std::set<std::string>& excluded_hosts,
                 int rpc_timeout_ms,
                 int attempt)
{
    std::string selected = lb->select(selectIn);
    if (selected.empty()) {
        LOG_WARN << "LB select failed (Attempt " << attempt << ")";
        return false;
    }
    LOG_INFO << "LB Selected: " << selected << " (Attempt " << attempt << ")";

    ctx.host_key = normalizeHostKey(selected);
    excluded_hosts.insert(selected);

    size_t p = ctx.host_key.find(':');
    if (p == std::string::npos) return false;
    ctx.ip = ctx.host_key.substr(0, p);
    ctx.port = atoi(ctx.host_key.substr(p + 1).c_str());

    ctx.fd = ConnectionPool::instance().getConnection(ctx.ip, ctx.port);
    if (ctx.fd == -1) {
        reportStats(lb, selectIn, ctx, false, 0, rpc_timeout_ms, attempt);
        return false;
    }

    ha::ConcurrencyLimiter::instance().inc(ctx.host_key);
    
    int send_ret;
    do {
        send_ret = ::send(ctx.fd, send_str.data(), send_str.size(), MSG_NOSIGNAL);
    } while (send_ret == -1 && errno == EINTR);

    if (send_ret != -1) {
        ctx.sent = true;
        return true;
    } else {
        cleanupRequest(ctx, false);
        reportStats(lb, selectIn, ctx, false, 0, rpc_timeout_ms, attempt);
        ctx.fd = -1; // Reset fd to indicate failure
        return false;
    }
}

} // namespace

void MpRpcChannel::CallMethod(const google::protobuf::MethodDescriptor* methodDesc,
                                google::protobuf::RpcController* controller,
                                const google::protobuf::Message* request,
                                google::protobuf::Message* response,
                                google::protobuf::Closure* done)
{
    const int MAX_RETRIES = 5;
    
    std::string backup_ms_str = MpRpcApplication::getInstance().Load("backup_request_ms");
    int backup_request_ms = backup_ms_str.empty() ? 10 : std::stoi(backup_ms_str);
    
    std::string timeout_str = MpRpcApplication::getInstance().Load("rpc_timeout_ms");
    int rpc_timeout_ms = timeout_str.empty() ? 5000 : std::stoi(timeout_str);

    std::vector<std::string> hosts;
    std::shared_ptr<LoadBalancer> lb;
    
    if (!ServiceDiscovery::instance().getHosts(methodDesc->service()->name(), methodDesc->name(), hosts, lb)) {
        if (controller->Failed()) return;
        controller->SetFailed("Service discovery failed");
        if (done) done->Run();
        return;
    }

    if (!lb) {
        controller->SetFailed("Load Balancer not initialized");
        if (done) done->Run();
        return;
    }

    auto span = mprpc::tracing::Span::CreateClientSpan(
        methodDesc->service()->name() + "." + methodDesc->name()
    );
    
    struct ScopedSpan {
        std::shared_ptr<mprpc::tracing::Span> span_;
        std::shared_ptr<mprpc::tracing::Span> prev_span_;
        ScopedSpan(std::shared_ptr<mprpc::tracing::Span> s) : span_(s) {
            prev_span_ = mprpc::tracing::TraceContext::GetCurrentSpan();
            mprpc::tracing::TraceContext::SetCurrentSpan(span_);
        }
        ~ScopedSpan() {
            if (span_) span_->End();
            mprpc::tracing::TraceContext::SetCurrentSpan(prev_span_);
        }
    };
    ScopedSpan scoped_span(span);

    auto* mprpcController = dynamic_cast<MpRpcController*>(controller);
    if (mprpcController) {
        mprpcController->SetMetadata("trace_id", span->TraceIdStr());
        mprpcController->SetMetadata("span_id", span->SpanIdStr());
        mprpcController->SetMetadata("parent_span_id", span->ParentSpanIdStr());
    }

    std::string serviceKey = methodDesc->service()->name() + ":" + methodDesc->name();
    std::string hash_key;
    
    if (mprpcController && mprpcController->HasHashKey()) {
        hash_key = std::to_string(mprpcController->GetHashKey());
    } else {
        hash_key = request->DebugString(); 
    }

    auto global_start = std::chrono::steady_clock::now();
    std::set<std::string> excluded_hosts;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - global_start).count();
        if (elapsed_ms >= rpc_timeout_ms) break;
        
        int remaining_ms = rpc_timeout_ms - elapsed_ms;

        if (controller->Failed()) controller->Reset();
        
        std::string send_str;
        if (packageRpcRequest(&send_str, methodDesc, controller, request) != CHANNEL_CODE::SUCCESS) {
            LOG_ERROR << "Package request failed";
            return; 
        }
        span->SetRequestSize(send_str.size());

        SelectIn selectIn;
        selectIn.hosts = hosts;
        selectIn.service_key = serviceKey;
        selectIn.request_key = hash_key;
        selectIn.excluded = &excluded_hosts;
        selectIn.begin_time_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        RequestContext ctx1;
        if (!attemptSend(lb, selectIn, send_str, ctx1, excluded_hosts, rpc_timeout_ms, attempt)) {
            continue;
        }

        struct pollfd fds[2];
        int nfds = 1;
        fds[0].fd = ctx1.fd;
        fds[0].events = POLLIN;

        int wait_ms = std::min(backup_request_ms, remaining_ms);
        int ret;
        do {
            ret = poll(fds, nfds, wait_ms);
        } while (ret < 0 && errno == EINTR);

        RequestContext ctx2;
        bool use_ctx2 = false;

        // Backup request logic
        if (ret == 0 && remaining_ms > backup_request_ms) {
            if (attemptSend(lb, selectIn, send_str, ctx2, excluded_hosts, rpc_timeout_ms, attempt)) {
                fds[1].fd = ctx2.fd;
                fds[1].events = POLLIN;
                nfds = 2;
                use_ctx2 = true;
            }

            int final_wait = remaining_ms - backup_request_ms;
            if (final_wait < 0) final_wait = 0;
            
            do {
                ret = poll(fds, nfds, final_wait);
            } while (ret < 0 && errno == EINTR);
        }

        RequestContext* target_ctx = nullptr;
        if (ret > 0) {
            if (fds[0].revents & POLLIN) {
                target_ctx = &ctx1;
            } else if (use_ctx2 && (fds[1].revents & POLLIN)) {
                target_ctx = &ctx2;
            }
        }

        if (target_ctx) {
            auto code = receiveRpcResponse(target_ctx->fd, response, controller, remaining_ms);
            auto end_time = std::chrono::steady_clock::now();
            int64_t latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now).count();
            
            if (code == CHANNEL_CODE::SUCCESS) {
                LOG_INFO << "RPC Success: " << target_ctx->host_key << " (Latency: " << latency << "ms)";
            } else {
                LOG_WARN << "RPC Failed: " << target_ctx->host_key << " (Code: " << (int)code << ")";
            }

            reportStats(lb, selectIn, *target_ctx, (code == CHANNEL_CODE::SUCCESS), latency, rpc_timeout_ms, attempt);
            cleanupRequest(*target_ctx, (code == CHANNEL_CODE::SUCCESS));
            
            // Clean up the other request if it exists
            if (use_ctx2) {
                if (target_ctx == &ctx1) cleanupRequest(ctx2, false);
                else cleanupRequest(ctx1, false);
            }

            if (code == CHANNEL_CODE::SUCCESS) {
                if (mprpcController) mprpcController->SetRemoteAddr(target_ctx->host_key);
                span->SetRemoteSide(target_ctx->host_key);
                span->SetResponseSize(response->ByteSizeLong());
                span->SetErrorCode(0);
                if (done) done->Run();
                return;
            }
        } else {
            // Timeout or error on both
            cleanupRequest(ctx1, false);
            reportStats(lb, selectIn, ctx1, false, 0, rpc_timeout_ms, attempt);
            if (use_ctx2) {
                cleanupRequest(ctx2, false);
                reportStats(lb, selectIn, ctx2, false, 0, rpc_timeout_ms, attempt);
            }
        }
        
        LOG_WARN << "Retrying CallMethod (" << attempt + 1 << "/" << MAX_RETRIES << ")";
    }

    LOG_ERROR << "All retries failed for " << serviceKey;
    controller->SetFailed("Degradation: Service Unavailable (Timeout/MaxRetries)");
    span->SetErrorCode(503);
    if (done) done->Run();
}

CHANNEL_CODE MpRpcChannel::packageRpcRequest(std::string* send_str,
        const google::protobuf::MethodDescriptor* methodDesc,
        google::protobuf::RpcController* controller,
        const google::protobuf::Message* request)
{
    std::string args_str;
    if(!request->SerializeToString(&args_str))
    {
        controller->SetFailed("serialize request error!");
        return CHANNEL_CODE::PACKAGE_ERR;
    }

    mprpc::RpcHeader header;
    header.set_service_name(methodDesc->service()->name());
    header.set_method_name(methodDesc->name());
    header.set_args_size(args_str.size());

    MpRpcController* mpController = dynamic_cast<MpRpcController*>(controller);
    if (mpController) {
        auto& metadataMap = *header.mutable_meta_data();
        for (const auto& pair : mpController->GetAllMetadata()) {
            metadataMap[pair.first] = pair.second;
        }
    }

    std::string header_str;
    if(!header.SerializeToString(&header_str))
    {
        controller->SetFailed("serialize header error!");
        return CHANNEL_CODE::PACKAGE_ERR;
    }

    uint32_t header_size = header_str.size();
    send_str->insert(0, std::string((char*)&header_size, 4));
    *send_str += header_str;
    *send_str += args_str;
    
    return CHANNEL_CODE::SUCCESS;
}

CHANNEL_CODE MpRpcChannel::receiveRpcResponse(const int connfd,
        google::protobuf::Message* response,
        google::protobuf::RpcController* controller,
        int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char len_buf[4];
    if (!readN(connfd, len_buf, 4)) {
        controller->SetFailed("recv frame length error or timeout!");
        return CHANNEL_CODE::RECEIVE_ERR;
    }
    
    uint32_t len = 0;
    memcpy(&len, len_buf, 4);
    
    if (len == 0) return CHANNEL_CODE::SUCCESS;
    
    if (len > 10 * 1024 * 1024) { 
        controller->SetFailed("response too large!");
        return CHANNEL_CODE::RECEIVE_ERR;
    }

    std::vector<char> buf(len);
    if (!readN(connfd, buf.data(), len)) {
        controller->SetFailed("recv body error!");
        return CHANNEL_CODE::RECEIVE_ERR;
    }

    if(!response->ParseFromArray(buf.data(), len))
    {
        controller->SetFailed("parse error!");
        return CHANNEL_CODE::RECEIVE_ERR;
    }
    
    return CHANNEL_CODE::SUCCESS;
}
