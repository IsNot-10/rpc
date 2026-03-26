#pragma once

#include <google/protobuf/service.h>
#include <string>

/**
 * @brief RPC控制器类
 * 
 * 继承自 Google Protobuf 的 RpcController 接口，用于管理 RPC 调用的生命周期和状态
 * 主要功能：
 * 1. 跟踪 RPC 调用的状态（成功/失败）
 * 2. 存储和传递错误信息
 * 3. 支持设置元数据，用于传递附加信息
 * 4. 支持一致性哈希 Key，用于客户端负载均衡
 * 5. 记录对端地址信息
 * 
 * 该类可以被客户端和服务端同时使用，用于在 RPC 调用过程中传递控制信息
 */
class MpRpcController : public google::protobuf::RpcController
{
public:
    /**
     * @brief 构造函数
     * 
     * 初始化 RPC 控制器的默认状态
     */
    MpRpcController() : 
        failed_(false),
        errText_(""),
        remote_addr_(""),
        has_hash_key_(false),
        hash_key_(0)
    {}

    void Reset() override
    {
        failed_ = false;
        errText_ = "";
        has_hash_key_ = false;
        hash_key_ = 0;
    }
    
    bool Failed() const override
    {
        return failed_;
    }

    std::string ErrorText() const override
    {
        return errText_;
    }

    void SetFailed(const std::string& reason) override
    {
        failed_ = true;
        errText_ = reason;
    }

    void StartCancel() override {}
    
    bool IsCanceled() const override { return false; }
    
    /**
     * @brief 注册取消回调函数
     * 
     * 目前未实现该功能
     * 
     * @param callback 取消时触发的回调函数
     */
    void NotifyOnCancel(google::protobuf::Closure* callback) override {}

    // --- 以下为自定义扩展功能 ---

    /**
     * @brief 设置元数据
     * 
     * 用于在 RPC 调用过程中传递额外的元数据信息
     * 
     * @param key 元数据键名
     * @param value 元数据值
     */
    void SetMetadata(const std::string& key, const std::string& value) {
        metadata_[key] = value;
    }

    /**
     * @brief 获取指定键名的元数据
     * 
     * @param key 元数据键名
     * @return std::string 元数据值，如果键不存在则返回空字符串
     */
    std::string GetMetadata(const std::string& key) const {
        auto it = metadata_.find(key);
        if (it != metadata_.end()) {
            return it->second;
        }
        return "";
    }

    /**
     * @brief 获取所有元数据
     * 
     * @return const std::map<std::string, std::string>& 包含所有元数据的映射表
     */
    const std::map<std::string, std::string>& GetAllMetadata() const {
        return metadata_;
    }

    /**
     * @brief 设置一致性哈希 Key
     * 
     * 用于客户端负载均衡，根据哈希 Key 将请求路由到特定的服务实例
     * 
     * @param key 一致性哈希 Key 值
     */
    void SetHashKey(uint64_t key) {
        hash_key_ = key;          ///< 设置哈希 Key 值
        has_hash_key_ = true;     ///< 标记已设置哈希 Key
    }

    /**
     * @brief 检查是否已设置一致性哈希 Key
     * 
     * @return bool 已设置哈希 Key 返回 true，否则返回 false
     */
    bool HasHashKey() const {
        return has_hash_key_;
    }

    /**
     * @brief 获取一致性哈希 Key
     * 
     * @return uint64_t 一致性哈希 Key 值
     */
    uint64_t GetHashKey() const {
        return hash_key_;
    }

    /**
     * @brief 设置对端地址
     * 
     * 记录 RPC 调用的对端地址信息（IP:Port 格式）
     * 
     * @param addr 对端地址字符串
     */
    void SetRemoteAddr(const std::string& addr) {
        remote_addr_ = addr;
    }

    /**
     * @brief 获取对端地址
     * 
     * @return std::string 对端地址字符串（IP:Port 格式）
     */
    std::string GetRemoteAddr() const {
        return remote_addr_;
    }

private:
    bool failed_;                  ///< RPC 调用是否失败
    std::string errText_;          ///< 错误信息文本
    std::string remote_addr_;      ///< 对端地址（IP:Port 格式）
    
    bool has_hash_key_;            ///< 是否设置了一致性哈希 Key
    uint64_t hash_key_;            ///< 一致性哈希 Key 值，用于负载均衡
    
    std::map<std::string, std::string> metadata_; ///< 存储元数据的映射表
};
