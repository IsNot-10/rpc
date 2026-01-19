#include "MpRpcChannel.h"
#include "ha/circuit_breaker.h"
#include "ha/concurrency_limiter.h"
#include "lb/lb_factory.h"
#include "RpcHeader.pb.h"
#include "MpRpcApplication.h"
#include "MpRpcController.h"
#include "Logging.h"
#include "ConnectionPool.h"
#include "tracing/TraceContext.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <vector>
#include <numeric>
#include <random>
#include <deque>
#include <cmath>
#include <sstream>
#include <chrono>
#include <memory>
#include <atomic>

#include <sys/stat.h>
#include <fstream>

// 辅助函数：带超时的连接
static int connectWithTimeout(int sockfd, const struct sockaddr* addr, socklen_t addrlen, int timeout_ms) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sockfd, addr, addrlen);
    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sockfd, &wset);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            res = select(sockfd + 1, nullptr, &wset, nullptr, &tv);
            if (res > 0) {
                int so_error;
                socklen_t len = sizeof(so_error);
                getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    res = 0; // 连接成功
                } else {
                    res = -1; // 连接错误
                }
            } else {
                res = -1; // 超时或错误
            }
        }
    }
    
    fcntl(sockfd, F_SETFL, flags);
    return res;
}

// -----------------------------------------------------------------------

struct ServiceCacheItem {
    std::vector<std::string> hosts;          // 服务提供者列表
    time_t expire_time;                      // 过期时间
    std::shared_ptr<ConsistentHashRing> ch_ring; // 一致性哈希环
    time_t next_retry_time = 0;              // 冷却时间/锁
};

static std::unordered_map<std::string, ServiceCacheItem> g_serviceCache;
static std::shared_mutex g_cacheMutex; // 使用读写锁
static const int CACHE_TTL_SEC = 30;

// -----------------------------------------------------------------------

static bool readN(int fd, char* buf, int n) {
    int total = 0;
    while (total < n) {
        int ret = ::recv(fd, buf + total, n - total, 0);
        if (ret <= 0) return false;
        total += ret;
    }
    return true;
}

static std::string normalizeHostKey(const std::string& s) {
    size_t last = s.rfind(':');
    if (last == std::string::npos) return s;
    size_t prev = (last > 0) ? s.rfind(':', last - 1) : std::string::npos;
    if (prev != std::string::npos) {
        return s.substr(0, last);
    }
    return s;
}

void MpRpcChannel::CallMethod(const google::protobuf::MethodDescriptor* methodDesc,
                                google::protobuf::RpcController* controller,
                                const google::protobuf::Message* request,
                                google::protobuf::Message* response,
                                google::protobuf::Closure* done)
{
    // 配置加载
    const int MAX_RETRIES = 5;
    std::string backup_ms_str = MpRpcApplication::getInstance().Load("backup_request_ms");
    int backup_request_ms = backup_ms_str.empty() ? 10 : std::stoi(backup_ms_str);
    
    std::string timeout_str = MpRpcApplication::getInstance().Load("rpc_timeout_ms");
    int rpc_timeout_ms = timeout_str.empty() ? 5000 : std::stoi(timeout_str);

    std::string lb_algo = MpRpcApplication::getInstance().Load("load_balancer");

    // 服务发现：获取服务主机列表
    std::vector<std::string> hosts;
    std::shared_ptr<ConsistentHashRing> cached_ring;
    if (getHosts(methodDesc->service()->name(), methodDesc->name(), hosts, cached_ring, controller) != CHANNEL_CODE::SUCCESS) {
        if (controller->Failed()) return;
        controller->SetFailed("Service discovery failed");
        if (done) done->Run();
        return;
    }

    // 负载均衡器初始化
    auto lb = CreateLoadBalancer(lb_algo);
    if (lb_algo == "consistent_hash" && cached_ring) {
        auto* chlb = dynamic_cast<ConsistentHashLB*>(lb.get());
        if (chlb) {
            chlb->SetRing(cached_ring);
        }
    }

    // 分布式追踪 (Distributed Tracing)
    auto span = mprpc::tracing::Span::CreateClientSpan(
        methodDesc->service()->name() + "." + methodDesc->name()
    );
    
    // RAII helper to manage span lifecycle and context switching
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

    // 将追踪信息注入到 Controller (随后会打包进 RpcHeader)
    auto* mprpcController = dynamic_cast<MpRpcController*>(controller);
    if (mprpcController) {
        mprpcController->SetMetadata("trace_id", span->TraceIdStr());
        mprpcController->SetMetadata("span_id", span->SpanIdStr());
        mprpcController->SetMetadata("parent_span_id", span->ParentSpanIdStr());
    }

    std::string serviceKey = methodDesc->service()->name() + ":" + methodDesc->name();
    std::string hash_key;
    // auto* mprpcController = dynamic_cast<MpRpcController*>(controller); // Moved up
    if (mprpcController && mprpcController->HasHashKey()) {
        hash_key = std::to_string(mprpcController->GetHashKey());
    } else {
        hash_key = request->DebugString(); 
    }

    auto global_start = std::chrono::steady_clock::now();

    // 重试循环
    std::set<std::string> excluded_hosts;
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        // 检查全局超时
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - global_start).count();
        if (elapsed_ms >= rpc_timeout_ms) {
            break; // Timeout (超时)
        }
        int remaining_ms = rpc_timeout_ms - elapsed_ms;

        if (controller->Failed()) controller->Reset();
        
        std::string send_str;
        if (packageRpcRequest(&send_str, methodDesc, controller, request) != CHANNEL_CODE::SUCCESS) {
            LOG_ERROR << "Package request failed";
            return; 
        }
        span->SetRequestSize(send_str.size());

        // ================== 备份请求 (对冲请求) 逻辑 ==================
        SelectIn selectIn;
        selectIn.hosts = hosts;
        selectIn.service_key = serviceKey;
        selectIn.request_key = hash_key;
        selectIn.excluded = &excluded_hosts;
        selectIn.begin_time_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        // 1. 选择主节点 (Primary)
        std::string selected1 = lb->select(selectIn);
        if (selected1.empty()) {
             LOG_WARN << "LB select failed (Attempt " << attempt << ")";
             continue; 
        }
        
        std::string host_key1 = normalizeHostKey(selected1);
        excluded_hosts.insert(selected1); 
        
        size_t p1 = host_key1.find(':');
        if (p1 == std::string::npos) continue;
        std::string ip1 = host_key1.substr(0, p1);
        uint16_t port1 = atoi(host_key1.substr(p1 + 1).c_str());

        // 2. 发送请求给主节点
        int clientfd1 = ConnectionPool::instance().getConnection(ip1, port1);
        if (clientfd1 == -1) {
             ha::CircuitBreaker::instance().report_status(host_key1, false, 5000);
             
             CallInfo info;
             info.service_key = serviceKey;
             info.host = host_key1;
             info.success = false;
             info.begin_time_us = selectIn.begin_time_us;
             info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
             info.timeout_ms = rpc_timeout_ms;
             info.retried_count = attempt;
             lb->feedback(info);
             
             continue;
        }
        
        ha::ConcurrencyLimiter::instance().inc(host_key1);
        if (::send(clientfd1, send_str.data(), send_str.size(), MSG_NOSIGNAL) != -1) {
        } else {
             ConnectionPool::instance().closeConnection(clientfd1);
             ha::ConcurrencyLimiter::instance().dec(host_key1);
             ha::CircuitBreaker::instance().report_status(host_key1, false, rpc_timeout_ms);
             
             CallInfo info;
             info.service_key = serviceKey;
             info.host = host_key1;
             info.success = false;
             info.begin_time_us = selectIn.begin_time_us;
             info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
             info.timeout_ms = rpc_timeout_ms;
             info.retried_count = attempt;
             lb->feedback(info);
             
             continue; // 重试
        }

        // 3. Backup Request Wait
        // 3. 等待备份请求触发时间 (Backup Request Wait)
        struct pollfd fds[2];
        int nfds = 1;
        fds[0].fd = clientfd1;
        fds[0].events = POLLIN;

        int wait_ms = std::min(backup_request_ms, remaining_ms);
        int ret = poll(fds, nfds, wait_ms);

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            // 主节点成功返回
            auto code = receiveRpcResponse(clientfd1, response, controller, remaining_ms);
            ha::ConcurrencyLimiter::instance().dec(host_key1);
            
            auto end_time = std::chrono::steady_clock::now();
            int64_t latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now).count();
            ha::CircuitBreaker::instance().report_status(host_key1, (code == CHANNEL_CODE::SUCCESS), latency);
            
            CallInfo info;
            info.service_key = serviceKey;
            info.host = host_key1;
            info.success = (code == CHANNEL_CODE::SUCCESS);
            info.begin_time_us = selectIn.begin_time_us;
            info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time.time_since_epoch()).count();
            info.timeout_ms = rpc_timeout_ms;
            info.retried_count = attempt;
            lb->feedback(info);
            
            if (code == CHANNEL_CODE::SUCCESS) {
                ConnectionPool::instance().releaseConnection(ip1, port1, clientfd1);
                if (mprpcController) mprpcController->SetRemoteAddr(host_key1);
                
                // Tracing updates
                span->SetRemoteSide(host_key1);
                span->SetResponseSize(response->ByteSizeLong()); 
                span->SetErrorCode(0);

                if (done) done->Run();
                return;
            } else {
                ConnectionPool::instance().closeConnection(clientfd1);
                // 继续进入重试循环
            }
        } 
        else if (ret == 0 && remaining_ms > backup_request_ms) {
            // 超时 (达到 Backup Request 阈值) -> 发送备份请求
            std::string selected2 = lb->select(selectIn);
            int clientfd2 = -1;
            std::string host_key2;
            bool req2_sent = false;

            if (!selected2.empty()) {
                host_key2 = normalizeHostKey(selected2);
                size_t p2 = host_key2.find(':');
                std::string ip2 = host_key2.substr(0, p2);
                uint16_t port2 = atoi(host_key2.substr(p2 + 1).c_str());

                clientfd2 = ConnectionPool::instance().getConnection(ip2, port2);
                if (clientfd2 != -1) {
                    ha::ConcurrencyLimiter::instance().inc(host_key2);
                    if (::send(clientfd2, send_str.data(), send_str.size(), 0) != -1) {
                        req2_sent = true;
                        fds[1].fd = clientfd2;
                        fds[1].events = POLLIN;
                        nfds = 2;
                    } else {
                        ConnectionPool::instance().closeConnection(clientfd2);
                        ha::ConcurrencyLimiter::instance().dec(host_key2);
                        
                        // 备份发送失败的反馈：并非严格必要，因为尚未真正进入“已尝试”的状态
                        clientfd2 = -1;
                    }
                }
            }

            // 等待任一连接就绪 (主或备份)
            int final_wait = remaining_ms - backup_request_ms;
            if (final_wait < 0) final_wait = 0;
            ret = poll(fds, nfds, final_wait);

            int target_fd = -1;
            std::string target_host;
            bool is_fd1 = false;

            if (ret > 0) {
                if (fds[0].revents & POLLIN) {
                    target_fd = clientfd1;
                    target_host = host_key1;
                    is_fd1 = true;
                } else if (req2_sent && (fds[1].revents & POLLIN)) {
                    target_fd = clientfd2;
                    target_host = host_key2;
                }
            }

            if (target_fd != -1) {
                auto code = receiveRpcResponse(target_fd, response, controller, final_wait);
                
                // 清理主连接 (Primary)
                if (is_fd1) {
                    if (code == CHANNEL_CODE::SUCCESS) ConnectionPool::instance().releaseConnection(ip1, port1, clientfd1);
                    else ConnectionPool::instance().closeConnection(clientfd1);
                } else {
                    ConnectionPool::instance().closeConnection(clientfd1);
                }
                ha::ConcurrencyLimiter::instance().dec(host_key1);

                // 清理备连接 (Backup)
                if (req2_sent) {
                    if (!is_fd1 && code == CHANNEL_CODE::SUCCESS) {
                         size_t p = host_key2.find(':');
                         ConnectionPool::instance().releaseConnection(host_key2.substr(0, p), atoi(host_key2.substr(p+1).c_str()), clientfd2);
                    } else {
                        ConnectionPool::instance().closeConnection(clientfd2);
                    }
                    ha::ConcurrencyLimiter::instance().dec(host_key2);
                }

                auto end_time = std::chrono::steady_clock::now();
                int64_t latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now).count();
                ha::CircuitBreaker::instance().report_status(target_host, (code == CHANNEL_CODE::SUCCESS), latency);

                CallInfo info;
                info.service_key = serviceKey;
                info.host = target_host;
                info.success = (code == CHANNEL_CODE::SUCCESS);
                info.begin_time_us = selectIn.begin_time_us;
                info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time.time_since_epoch()).count();
                info.timeout_ms = rpc_timeout_ms;
                info.retried_count = attempt;
                lb->feedback(info);

                if (code == CHANNEL_CODE::SUCCESS) {
                    if (mprpcController) mprpcController->SetRemoteAddr(target_host);
                    
                    // Tracing updates
                    span->SetRemoteSide(target_host);
                    span->SetResponseSize(response->ByteSizeLong()); 
                    span->SetErrorCode(0);

                    if (done) done->Run();
                    return;
                }
            } else {
                // 两个连接均超时
                ConnectionPool::instance().closeConnection(clientfd1);
                ha::ConcurrencyLimiter::instance().dec(host_key1);
                ha::CircuitBreaker::instance().report_status(host_key1, false, rpc_timeout_ms);
                
                CallInfo info;
                info.service_key = serviceKey;
                info.host = host_key1;
                info.success = false;
                info.begin_time_us = selectIn.begin_time_us;
                info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                info.timeout_ms = rpc_timeout_ms;
                info.retried_count = attempt;
                lb->feedback(info);
                
                if (req2_sent) {
                    ConnectionPool::instance().closeConnection(clientfd2);
                    ha::ConcurrencyLimiter::instance().dec(host_key2);
                    ha::CircuitBreaker::instance().report_status(host_key2, false, rpc_timeout_ms);
                    
                    info.host = host_key2;
                    lb->feedback(info);
                }
            }
        } else {
            // 主连接超时且无法再发备份请求 (或备份关闭/阈值过短)
             ConnectionPool::instance().closeConnection(clientfd1);
             ha::ConcurrencyLimiter::instance().dec(host_key1);
             ha::CircuitBreaker::instance().report_status(host_key1, false, rpc_timeout_ms);
             
             CallInfo info;
             info.service_key = serviceKey;
             info.host = host_key1;
             info.success = false;
             info.begin_time_us = selectIn.begin_time_us;
             info.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
             info.timeout_ms = rpc_timeout_ms;
             info.retried_count = attempt;
             lb->feedback(info);
        }
        
        LOG_WARN << "Retrying CallMethod (" << attempt + 1 << "/" << MAX_RETRIES << ")";
    }

    // 降级逻辑：所有重试均失败
    LOG_ERROR << "All retries failed for " << serviceKey;
    controller->SetFailed("Degradation: Service Unavailable (Timeout/MaxRetries)");
    span->SetErrorCode(503); // Service Unavailable
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

CHANNEL_CODE MpRpcChannel::getHosts(const std::string& service_name, 
    const std::string& method_name,
    std::vector<std::string>& hosts,
    std::shared_ptr<ConsistentHashRing>& cached_ring,
    google::protobuf::RpcController* controller)
{
    std::string cacheKey = service_name + ":" + method_name;
    bool should_update = false;
    std::vector<std::string> stale_hosts;
    
    // Step 1: Read Cache
    {
        std::shared_lock<std::shared_mutex> lock(g_cacheMutex);
        auto it = g_serviceCache.find(cacheKey);
        if (it != g_serviceCache.end()) {
            time_t now = time(nullptr);
            // 缓存有效
            if (now < it->second.expire_time) {
                hosts = it->second.hosts;
                cached_ring = it->second.ch_ring;
                if (!hosts.empty()) return CHANNEL_CODE::SUCCESS;
                should_update = true; // 缓存为空，强制更新
            }
            // 缓存过期，但在冷却期内（其他线程正在更新或刚失败）
            else if (now < it->second.next_retry_time) {
                hosts = it->second.hosts;
                cached_ring = it->second.ch_ring;
                // 即使过期，如果有数据也先用着
                if (!hosts.empty()) {
                    LOG_WARN << "Cache expired but in cooldown, using stale cache for " << cacheKey;
                    return CHANNEL_CODE::SUCCESS;
                }
                should_update = true; // 无数据可用，强制更新
            } 
            // 缓存过期，且不在冷却期 -> 需要更新
            else {
                stale_hosts = it->second.hosts; // 备份旧数据
                should_update = true;
            }
        } else {
            should_update = true;
        }
    }

    // Step 2: Update (if needed)
    if (should_update) {
        bool my_turn = false;
        
        // 2.1 获取写锁，Double Check
        {
            std::unique_lock<std::shared_mutex> lock(g_cacheMutex);
            auto it = g_serviceCache.find(cacheKey);
            time_t now = time(nullptr);
            
            if (it != g_serviceCache.end()) {
                // 再次检查是否已被别人更新
                if (now < it->second.expire_time && !it->second.hosts.empty()) {
                    hosts = it->second.hosts;
                    cached_ring = it->second.ch_ring;
                    return CHANNEL_CODE::SUCCESS;
                }
                // 再次检查是否进入冷却
                if (now < it->second.next_retry_time) {
                    if (!it->second.hosts.empty()) {
                         hosts = it->second.hosts;
                         cached_ring = it->second.ch_ring;
                         return CHANNEL_CODE::SUCCESS;
                    }
                }
                
                // 抢占更新权
                it->second.next_retry_time = now + 10; // 锁定10秒
                if (stale_hosts.empty()) stale_hosts = it->second.hosts; // 再次确保拿到最新的旧数据
                my_turn = true;
            } else {
                // 初始化条目
                ServiceCacheItem item;
                item.next_retry_time = now + 10;
                g_serviceCache[cacheKey] = item;
                my_turn = true;
            }
        } // 释放写锁，避免阻塞其他线程读取旧数据

        // 2.2 执行网络请求 (无锁状态)
        if (my_turn) {
            std::vector<std::string> new_hosts;
            bool success = false;
            
            int clientfd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (clientfd != -1) {
                std::string registryIp = MpRpcApplication::getInstance().Load("registry_ip");
                uint16_t registryPort = atoi(MpRpcApplication::getInstance().Load("registry_port").c_str());
                if (registryIp.empty() || registryPort == 0) {
                    registryIp = "127.0.0.1";
                    registryPort = 8001;
                }
                
                sockaddr_in server_addr;
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(registryPort);
                server_addr.sin_addr.s_addr = inet_addr(registryIp.c_str());

                if (connectWithTimeout(clientfd, (sockaddr*)&server_addr, sizeof(server_addr), 500) == 0) {
                    std::string msg = "DIS|" + service_name + "|" + method_name + "|";
                    if (::send(clientfd, msg.c_str(), msg.size(), 0) != -1) {
                        char buf[4096] = {0}; // 增大buffer
                        if (::recv(clientfd, buf, 4096, 0) > 0) {
                            std::string response(buf);
                            if (response.substr(0, 3) == "RES") {
                                std::stringstream ss(response);
                                std::string segment;
                                while(std::getline(ss, segment, '|')) {
                                    size_t first = segment.find_first_not_of(" \t\r\n");
                                    if (first == std::string::npos) continue; 
                                    size_t last = segment.find_last_not_of(" \t\r\n");
                                    segment = segment.substr(first, (last - first + 1));
                                    if (segment == "RES") continue;
                                    if (!segment.empty()) new_hosts.push_back(segment);
                                }
                                success = true;
                            }
                        }
                    }
                }
                ::close(clientfd);
            }
            
            // 2.3 更新缓存 (写锁)
            if (success && !new_hosts.empty()) {
                std::unique_lock<std::shared_mutex> lock(g_cacheMutex);
                auto& item = g_serviceCache[cacheKey];
                item.hosts = new_hosts;
                item.expire_time = time(nullptr) + CACHE_TTL_SEC;
                item.next_retry_time = 0; // 解除冷却
                item.ch_ring = std::make_shared<ConsistentHashRing>(160);
                item.ch_ring->Build(new_hosts);
                
                hosts = new_hosts;
                cached_ring = item.ch_ring;
            } else {
                LOG_WARN << "Registry update failed or empty result for " << cacheKey;
                // 更新失败，回退到旧数据
                if (!stale_hosts.empty()) {
                    hosts = stale_hosts;
                    // 确保缓存中也有旧数据（防止是新创建的空条目）
                    std::unique_lock<std::shared_mutex> lock(g_cacheMutex);
                    auto& item = g_serviceCache[cacheKey];
                    if (item.hosts.empty()) {
                        item.hosts = stale_hosts;
                        item.ch_ring = std::make_shared<ConsistentHashRing>(160);
                        item.ch_ring->Build(stale_hosts);
                    }
                    cached_ring = item.ch_ring;
                }
            }
        }
    }
    
    // Step 3: Final Fallback
    if (hosts.empty()) {
        hosts.push_back("127.0.0.1:9999");
        LOG_WARN << "No provider available, fallback to 127.0.0.1:9999 for " << cacheKey;
        
        // 确保这个兜底数据也写入缓存，避免下次还是空
        std::unique_lock<std::shared_mutex> lock(g_cacheMutex);
        auto& item = g_serviceCache[cacheKey];
        if (item.hosts.empty()) {
            item.hosts = hosts;
            item.ch_ring = std::make_shared<ConsistentHashRing>(160);
            item.ch_ring->Build(hosts);
        }
        cached_ring = item.ch_ring;
    }

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
    
    if (len == 0) { 
         return CHANNEL_CODE::SUCCESS;
    }
    
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
