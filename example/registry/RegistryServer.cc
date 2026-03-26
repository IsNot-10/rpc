// RegistryServer.cc
#include "RegistryServer.h"
#include "Logging.h"
#include "TimerId.h"
#include <sstream>
#include <algorithm>

// 计算时间差的辅助函数（秒）
static double timeDifference(TimeStamp high, TimeStamp low)
{
    int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / TimeStamp::kMicroSecondsPerSecond;
}

RegistryServer::RegistryServer(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop), server_(loop, listenAddr, "RegistryServer")
{
    // 注册连接建立与断开的回调函数
    server_.setConnectionCallback(std::bind(&RegistryServer::onConnection, this, std::placeholders::_1));
    // 注册读写事件的回调函数
    server_.setMessageCallback(std::bind(&RegistryServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    
    // 启动定时任务：每5秒检查一次是否有过期节点
    loop_->runEvery(5.0, std::bind(&RegistryServer::cleanUpExpiredNodes, this));
}

void RegistryServer::start()
{
    server_.start();
    LOG_INFO << "Registry Server started";
}

void RegistryServer::onConnection(const TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        LOG_INFO << "Registry Client connected: " << conn->getPeerAddr().getIpPort();
    }
    else
    {
        LOG_INFO << "Registry Client disconnected: " << conn->getPeerAddr().getIpPort();
    }
}

// 简单的文本协议处理
// 格式: COMMAND|service_name|method_name|ip:port|...
// 示例:
// 注册: REG|UserService|Login|127.0.0.1:8000|weight
// 发现: DIS|UserService|Login|
// 心跳: HB|127.0.0.1:8000
void RegistryServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp time)
{
    // Retrieve all data
    std::string msg = buf->retrieveAllAsString();
    
    std::stringstream ss(msg);
    std::string line;
    
    // Process line by line
    while(std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back(); // Handle CRLF
        
        std::stringstream ls(line);
        std::string segment;
        std::vector<std::string> parts;
        
        while(std::getline(ls, segment, '|')) {
            parts.push_back(segment);
        }
        
        if (parts.empty()) continue;
        
        std::string cmd = parts[0];
        
        std::lock_guard<std::mutex> lock(mutex_); 
        
        if (cmd == "REG" && parts.size() >= 4) {
            std::string key = parts[1] + "/" + parts[2];
            int weight = 1;
            if (parts.size() >= 5) {
                try {
                    weight = std::stoi(parts[4]);
                } catch(...) {
                    weight = 1;
                }
            }
            handleRegister(key, "", parts[3], weight);
            conn->send("ACK\n"); 
        }
        else if (cmd == "DIS" && parts.size() >= 3) {
            handleDiscovery(conn, parts[1], parts[2]);
        }
        else if (cmd == "HB" && parts.size() >= 2) {
            handleHeartbeat(parts[1]);
        }
    }
}

void RegistryServer::handleRegister(const std::string& key, const std::string& method, const std::string& ipPort, int weight)
{
    auto& nodes = serviceMap_[key];
    bool found = false;
    
    // 如果节点已存在，更新心跳时间和权重
    for(auto& node : nodes) {
        if (node.ipPort == ipPort) {
            node.lastHeartbeat = TimeStamp::now();
            node.weight = weight;
            found = true;
            break;
        }
    }
    
    // 如果是新节点，加入列表
    if (!found) {
        ServiceNode node;
        node.ipPort = ipPort;
        node.lastHeartbeat = TimeStamp::now();
        node.weight = weight;
        nodes.push_back(node);
        LOG_INFO << "Registered new node: " << key << " -> " << ipPort << " (weight=" << weight << ")";
    }

    // 更新反向索引
    nodeServices_[ipPort].insert(key);
}

void RegistryServer::handleDiscovery(const TcpConnectionPtr& conn, const std::string& service, const std::string& method)
{
    std::string key = service + "/" + method;
    std::string response = "RES";
    
    if (serviceMap_.find(key) != serviceMap_.end()) {
        for(const auto& node : serviceMap_[key]) {
            // 响应格式: RES|ip:port:weight|ip:port:weight
            // 使用 ':' 作为权重的分隔符，保持 '|' 为节点间的分隔符
            response += "|" + node.ipPort + ":" + std::to_string(node.weight);
        }
    }
    response += "\n";
    conn->send(response);
}

void RegistryServer::handleHeartbeat(const std::string& ipPort)
{
    // 收到心跳包，更新该IP端口提供的所有服务的心跳时间
    // 优化后：使用反向索引 O(1)
    
    if (nodeServices_.find(ipPort) == nodeServices_.end()) {
        // LOG_INFO << "Heartbeat from unknown node: " << ipPort;
        return;
    }

    const auto& services = nodeServices_[ipPort];
    for (const auto& serviceKey : services) {
        if (serviceMap_.find(serviceKey) != serviceMap_.end()) {
            auto& nodes = serviceMap_[serviceKey];
            for (auto& node : nodes) {
                if (node.ipPort == ipPort) {
                    node.lastHeartbeat = TimeStamp::now();
                    break; 
                }
            }
        }
    }
}

void RegistryServer::cleanUpExpiredNodes()
{
    std::lock_guard<std::mutex> lock(mutex_);
    TimeStamp now = TimeStamp::now();
    double timeout = 15.0; // 设置超时时间为 15 秒
    
    for(auto it = serviceMap_.begin(); it != serviceMap_.end(); ) {
        auto& nodes = it->second;
        for(auto nodeIt = nodes.begin(); nodeIt != nodes.end(); ) {
            // 如果心跳超时，则移除该节点
            if (timeDifference(now, nodeIt->lastHeartbeat) > timeout) {
                LOG_INFO << "Removing expired node: " << it->first << " -> " << nodeIt->ipPort;
                
                // 从反向索引中移除
                auto nsIt = nodeServices_.find(nodeIt->ipPort);
                if (nsIt != nodeServices_.end()) {
                    nsIt->second.erase(it->first);
                    if (nsIt->second.empty()) {
                        nodeServices_.erase(nsIt);
                    }
                }

                nodeIt = nodes.erase(nodeIt);
            } else {
                ++nodeIt;
            }
        }
        
        // 如果该服务下没有任何节点，也可以移除该服务Key（可选）
        if (nodes.empty()) {
            it = serviceMap_.erase(it);
        } else {
            ++it;
        }
    }
}
