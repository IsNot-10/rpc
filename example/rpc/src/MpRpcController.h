#pragma once

#include <google/protobuf/service.h>
#include <string>

/**
 * @brief Rpc控制器
 * 
 * 作用：
 * 1. 管理Rpc调用的状态（成功/失败）
 * 2. 携带错误信息
 * 3. 可扩展用于传递额外控制信息（如一致性哈希Key）
 */
class MpRpcController : public google::protobuf::RpcController
{
public:
    MpRpcController() : failed_(false), errText_(""), has_hash_key_(false), hash_key_(0) {}

    // 重置控制器状态，以便复用
    void Reset() override
    {
        failed_ = false;
        errText_ = "";
        has_hash_key_ = false;
        hash_key_ = 0;
    }
    
    // 判断调用是否失败
    bool Failed() const override
    {
        return failed_;
    }

    // 获取错误详情
    std::string ErrorText() const override
    {
        return errText_;
    }

    // 设置调用失败及原因
    void SetFailed(const std::string& reason) override
    {
        failed_ = true;
        errText_ = reason;
    }

    // --- 目前未实现的高级功能 (取消操作) ---
    void StartCancel() override {}
    bool IsCanceled() const override { return false; }
    void NotifyOnCancel(google::protobuf::Closure* callback) override {}

    // --- 自定义扩展功能 ---

    // 设置元数据 (Metadata)
    void SetMetadata(const std::string& key, const std::string& value) {
        metadata_[key] = value;
    }

    std::string GetMetadata(const std::string& key) const {
        auto it = metadata_.find(key);
        if (it != metadata_.end()) {
            return it->second;
        }
        return "";
    }

    const std::map<std::string, std::string>& GetAllMetadata() const {
        return metadata_;
    }

    // 设置一致性哈希的Key (用于客户端负载均衡)
    void SetHashKey(uint64_t key) {
        hash_key_ = key;
        has_hash_key_ = true;
    }

    bool HasHashKey() const {
        return has_hash_key_;
    }

    uint64_t GetHashKey() const {
        return hash_key_;
    }

    // 获取对端地址 (IP:Port)
    void SetRemoteAddr(const std::string& addr) {
        remote_addr_ = addr;
    }

    std::string GetRemoteAddr() const {
        return remote_addr_;
    }

private:
    bool failed_;           // 是否失败
    std::string errText_;   // 错误信息
    std::string remote_addr_; // 对端地址
    
    bool has_hash_key_;     // 是否设置了哈希Key
    uint64_t hash_key_;     // 哈希Key的值
    
    std::map<std::string, std::string> metadata_; // 元数据
};
