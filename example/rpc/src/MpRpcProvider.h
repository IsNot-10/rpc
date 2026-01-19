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
 * 
 * 核心功能：
 * 1. 服务注册：将本地服务类及其方法注册到框架中
 * 2. 启动服务：创建TcpServer，监听端口
 * 3. 请求处理：接收Rpc请求 -> 反序列化 -> 分发调用 -> 序列化响应
 * 4. 高级特性：支持限流 (Token Bucket) 和 过载保护 (Auto Concurrency)
 */
class MpRpcProvider
{
public:
    // 发布服务：将服务对象注册到Rpc框架
    void notifyService(google::protobuf::Service* service);
    
    // 启动Rpc服务节点，开始监听和处理请求
    void run();

private:
    // 内部结构：请求信息
    struct RequestInfo
    {
        std::string service_name;   // 服务名
        std::string method_name;    // 方法名
        uint32_t args_size;         // 参数长度
        std::string args_str;       // 参数序列化字符串
        std::map<std::string, std::string> meta_data; // 元数据
    };

    struct MethodContext {
        const google::protobuf::MethodDescriptor* descriptor;
        std::shared_ptr<metrics::Histogram> latencyHistogram;
    };

    // 内部结构：服务信息
    struct ServiceInfo
    {
        google::protobuf::Service* service_; // 服务对象指针
        std::unordered_map<std::string, MethodContext> methodMap_; // 方法描述符映射
    };

    /**
     * @brief 令牌桶限流算法 (Token Bucket)
     * 用途：限制请求速率 (QPS)，在过载时快速拒绝以保护服务
     * 说明：
     * - rate 为每秒可加入桶中的令牌数；capacity 为桶的最大容量 (支持突发)
     * - consume() 在进入关键路径前调用；若返回 false，表示应当拒绝请求
     * - 使用互斥锁保证线程安全；高并发场景可用分片桶或原子队列优化
     */
    class TokenBucket {
    public:
        TokenBucket(int rate, int capacity) 
            : rate_(rate), capacity_(capacity), tokens_(capacity), last_refill_time_(std::chrono::steady_clock::now()) {}

        // 尝试消耗n个令牌，成功返回true
        bool consume(int n = 1) {
            if (rate_ <= 0) return true; // 如果rate<=0，表示不限流

            std::lock_guard<std::mutex> lock(mutex_);
            refill();
            if (tokens_ >= n) {
                tokens_ -= n;
                return true;
            }
            return false;
        }

    private:
        // 补充令牌：按时间增量线性补充，避免每毫秒定时器带来的上下文切换
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

        int rate_;      // 速率 (tokens/sec)
        int capacity_;  // 桶容量 (burst size)
        int tokens_;    // 当前令牌数
        std::chrono::steady_clock::time_point last_refill_time_;
        std::mutex mutex_;
    };

    /**
     * @brief 自适应并发限制 (Auto Concurrency Limiter)
     * 参考 BRPC 设计: Limit = MaxQPS * MinLatency * (1 + alpha)
     */
    class AutoConcurrencyLimiter {
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
             // 初始采样时间
             last_sampling_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
             next_reset_time_us_ = last_sampling_time_us_ + 10 * 1000 * 1000; // 10s 后重测
        }

        // 尝试获取并发许可
        bool acquire() {
            int current = active_req_.fetch_add(1, std::memory_order_relaxed);
            int max_c = max_concurrency_.load(std::memory_order_relaxed);
            if (current >= max_c) {
                active_req_.fetch_sub(1, std::memory_order_relaxed);
                return false;
            }
            return true;
        }

        // 释放并发许可，并反馈本次请求耗时
        void release(int64_t latency_us) {
            active_req_.fetch_sub(1, std::memory_order_relaxed);
            updateStats(latency_us);
        }

        int maxConcurrency() const { return max_concurrency_; }

    private:
        void updateStats(int64_t latency_us) {
            std::lock_guard<std::mutex> lock(mutex_);
            
            // 忽略无效的延迟（例如作为 0 传递的错误情况）
            if (latency_us <= 0) return;

            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // 1. 更新 Min Latency (No-load Latency)
            // 记录最小延迟，用于估算系统在无负载情况下的处理能力
            if (min_latency_us_ == -1 || latency_us < min_latency_us_) {
                min_latency_us_ = latency_us;
            }

            // 2. 采样窗口聚合 (Sample Window)
            // 简单实现：我们直接在每次请求时更新 EMA QPS，而不是聚合一个窗口再算
            // BRPC 是聚合一个窗口，这里简化为 EMA
            
            // 统计过去 500ms 内的请求数
            
            samples_count_++;
            samples_latency_sum_ += latency_us;

            if (now_us - last_sampling_time_us_ >= 500 * 1000) { // 500ms 窗口
                double interval_s = (now_us - last_sampling_time_us_) / 1000000.0;
                double current_qps = samples_count_ / interval_s;
                // double avg_latency_us = samples_latency_sum_ / samples_count_;

                // 更新 Max QPS
                if (ema_max_qps_ == -1) ema_max_qps_ = current_qps;
                else if (current_qps > ema_max_qps_) ema_max_qps_ = current_qps;
                else ema_max_qps_ = ema_max_qps_ * 0.9 + current_qps * 0.1; // 缓慢衰减

                // BRPC 核心公式
                // MaxConcurrency = MaxQPS * MinLatency * (1 + alpha)
                if (min_latency_us_ > 0) {
                    double min_latency_s = min_latency_us_ / 1000000.0;
                    double new_limit = ema_max_qps_ * min_latency_s * 1.5; // alpha = 0.5
                    
                    // 必须保证有最小并发度
                    new_limit = std::max(5.0, new_limit);
                    
                    // 平滑更新
                    int old_limit = max_concurrency_.load();
                    int next_limit = (int)(old_limit * 0.7 + new_limit * 0.3);
                    
                    max_concurrency_.store(next_limit);
                    
                    // 日志采样打印
                    // LOG_INFO << "AutoLimiter: QPS=" << current_qps 
                    //          << " Latency=" << avg_latency_us 
                    //          << " MinLat=" << min_latency_us_ 
                    //          << " Limit=" << next_limit;
                }

                // 重置采样窗口
                samples_count_ = 0;
                samples_latency_sum_ = 0;
                last_sampling_time_us_ = now_us;
            }

            // 3. 重测机制 (Remeasurement)
            // 每隔一段时间缩小窗口，探测新的 min_latency
            if (now_us > next_reset_time_us_) {
                // 强制缩小窗口 (Punishment)，以便重新测量真实的 MinLatency
                int current_limit = max_concurrency_.load();
                max_concurrency_.store(std::max(5, current_limit / 2));
                
                // 重置 min_latency，准备重新测量
                min_latency_us_ = -1; 
                
                next_reset_time_us_ = now_us + 10 * 1000 * 1000; // 10s 后再来
                // LOG_INFO << "AutoLimiter: Remeasure triggered. Limit reduced.";
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

    // 组件实例
    std::unique_ptr<TokenBucket> tokenBucket_;
    std::unique_ptr<AutoConcurrencyLimiter> concurrencyLimiter_;
    bool enableAccessLog_ = false;

    // Muduo TcpServer 回调
    void onConnection(const TcpConnectionPtr& conn);
    // 协议探测与分发
    void onMessage(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);

    // HTTP 处理逻辑
    void handleHttpRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);
    void sendHttpError(const TcpConnectionPtr& conn, HttpResponse::HttpStatusCode code, const std::string& message);

    // RPC 处理逻辑
    void handleRpcRequest(const TcpConnectionPtr& conn, Buffer* buffer, TimeStamp timeStamp);
    bool parseRequest(Buffer* buffer, RequestInfo* reqInfo);
    void sendRpcResponse(const TcpConnectionPtr& conn, google::protobuf::Message* response);
    void sendRpcResponseAndRelease(const TcpConnectionPtr& conn, google::protobuf::Message* response, std::chrono::steady_clock::time_point start_time);
    void registerRegistry(const InetAddress& addr);

    // 服务注册表: service_name -> ServiceInfo
    std::unordered_map<std::string, ServiceInfo> serviceMap_;
    
    // Server start time
    std::chrono::system_clock::time_point startTime_;
};
