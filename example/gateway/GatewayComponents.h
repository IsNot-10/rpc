#pragma once

#include "Logging.h"
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/compiler/importer.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <dirent.h>
#include <chrono>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <tuple>

// --- Thread Pool for IO/Computation Separation ---
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back(
                [this] {
                    for(;;) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock,
                                [this]{ return this->stop || !this->tasks.empty(); });
                            if(this->stop && this->tasks.empty())
                                return;
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task();
                    }
                }
            );
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers)
            worker.join();
    }
    template<class F, class... Args>
    void addTask(F&& f, Args&&... args) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            // Use lambda instead of std::bind to avoid std::result_of deprecation warnings in C++17+
            tasks.emplace([func = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                std::apply(func, args);
            });
        }
        condition.notify_one();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// --- Token Bucket for Rate Limiting ---
class TokenBucket {
public:
    TokenBucket(int rate, int capacity) 
        : rate_(rate), capacity_(capacity), tokens_(capacity) {
        lastTime_ = std::chrono::steady_clock::now();
    }

    bool consume(int tokens = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime_).count();
        
        // Refill tokens
        int newTokens = (duration * rate_) / 1000;
        if (newTokens > 0) {
            tokens_ = std::min(capacity_, tokens_ + newTokens);
            lastTime_ = now;
        }

        if (tokens_ >= tokens) {
            tokens_ -= tokens;
            return true;
        }
        return false;
    }

private:
    int rate_;
    int capacity_;
    int tokens_;
    std::chrono::steady_clock::time_point lastTime_;
    std::mutex mutex_;
};

// --- Circuit Breaker ---
class CircuitBreaker {
public:
    struct State {
        int failureCount = 0;
        std::chrono::steady_clock::time_point lastFailureTime;
        bool isOpen = false;
    };

    bool allowRequest(const std::string& serviceName) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = states_[serviceName];
        
        if (state.isOpen) {
            // Check cooldown (5 seconds)
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - state.lastFailureTime).count() > 5) {
                // Half-Open
                state.isOpen = false;
                state.failureCount = 0;
                LOG_INFO << "Circuit Breaker RECOVERING for " << serviceName;
                return true;
            }
            return false;
        }
        return true;
    }

    void recordSuccess(const std::string& serviceName) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (states_.find(serviceName) != states_.end()) {
             states_[serviceName].failureCount = 0;
        }
    }

    void recordFailure(const std::string& serviceName) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = states_[serviceName];
        state.failureCount++;
        state.lastFailureTime = std::chrono::steady_clock::now();
        
        if (state.failureCount >= 3) { // Threshold: 3 failures
            state.isOpen = true;
            LOG_ERROR << "Circuit Breaker OPEN for " << serviceName << " (3 consecutive failures)";
        }
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, State> states_;
};

// --- RpcUtils for Protobuf Reflection (with Dynamic Loading) ---
class RpcUtils {
public:
    static void Init(const std::string& protoPath) {
        protoPath_ = protoPath;
        sourceTree_.MapPath("", protoPath);
        importer_.reset(new google::protobuf::compiler::Importer(&sourceTree_, &errorCollector_));
        ScanAndLoad();
    }

    // Scan directory and load protos
    static void ScanAndLoad() {
        DIR* dir;
        struct dirent* ent;
        if ((dir = opendir(protoPath_.c_str())) != NULL) {
            while ((ent = readdir(dir)) != NULL) {
                std::string filename = ent->d_name;
                if (filename.length() > 6 && filename.substr(filename.length() - 6) == ".proto") {
                    importer_->Import(filename);
                }
            }
            closedir(dir);
        } else {
            LOG_ERROR << "Could not open proto directory: " << protoPath_;
        }
    }

    static const google::protobuf::MethodDescriptor* GetMethodDescriptor(const std::string& serviceName, const std::string& methodName) {
        // Read Lock
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto* serviceDesc = importer_->pool()->FindServiceByName(serviceName);
            if (serviceDesc) {
                auto* methodDesc = serviceDesc->FindMethodByName(methodName);
                if (methodDesc) return methodDesc;
            }
        }

        // Not found, try reload (Write Lock)
        LOG_INFO << "Service/Method not found: " << serviceName << "/" << methodName << ", trying to reload protos...";
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            // Double check
            auto* serviceDesc = importer_->pool()->FindServiceByName(serviceName);
            if (serviceDesc) {
                 return serviceDesc->FindMethodByName(methodName);
            }

            ScanAndLoad();

            serviceDesc = importer_->pool()->FindServiceByName(serviceName);
            if (!serviceDesc) {
                LOG_ERROR << "Service still not found after reload: " << serviceName;
                return nullptr;
            }
            const auto* methodDesc = serviceDesc->FindMethodByName(methodName);
            if (!methodDesc) {
                LOG_ERROR << "Method not found after reload: " << methodName;
            }
            return methodDesc;
        }
    }
    
    static google::protobuf::compiler::Importer* GetImporter() {
        return importer_.get();
    }

private:
    class MyErrorCollector : public google::protobuf::compiler::MultiFileErrorCollector {
        void AddError(const std::string& filename, int line, int column, const std::string& message) override {
            LOG_ERROR << "Proto Error: " << filename << ":" << line << " - " << message;
        }
    };

    // Inline static definitions for simplicity in header-only usage (C++17)
    // For older C++, these should be in .cc file. 
    // Since we are moving to header, we should use 'inline' keyword or separate .cc.
    // Given the project structure, I'll use inline variables (C++17) if supported, or just hope for the best (ODR violations if included in multiple .cc, but GatewayServer.cc is the only one using it likely).
    // Actually, GatewayServer is the only consumer.
    
    static std::string protoPath_;
    static google::protobuf::compiler::DiskSourceTree sourceTree_;
    static MyErrorCollector errorCollector_;
    static std::unique_ptr<google::protobuf::compiler::Importer> importer_;
    static std::shared_mutex mutex_;
};

// Static member definitions (must be outside class)
// Note: If included in multiple files, this causes linker errors.
// Since we only use it in GatewayServer.cc, it is fine.
// Ideally should be in .cc, but we are doing a quick refactor.
inline std::string RpcUtils::protoPath_;
inline google::protobuf::compiler::DiskSourceTree RpcUtils::sourceTree_;
inline RpcUtils::MyErrorCollector RpcUtils::errorCollector_;
inline std::unique_ptr<google::protobuf::compiler::Importer> RpcUtils::importer_;
inline std::shared_mutex RpcUtils::mutex_;
