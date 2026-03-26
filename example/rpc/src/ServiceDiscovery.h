#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <condition_variable>
#include "lb/load_balancer.h"

// Forward declaration
class ConsistentHashRing;

struct ServiceCacheItem {
    std::vector<std::string> hosts;
    time_t expire_time;
    std::shared_ptr<ConsistentHashRing> ch_ring;
    std::shared_ptr<LoadBalancer> lb;
    time_t next_retry_time = 0;
};

class ServiceDiscovery {
public:
    static ServiceDiscovery& instance();

    bool getHosts(const std::string& service_name, 
                  const std::string& method_name,
                  std::vector<std::string>& hosts,
                  std::shared_ptr<LoadBalancer>& lb);

private:
    ServiceDiscovery() = default;
    ~ServiceDiscovery() = default;
    ServiceDiscovery(const ServiceDiscovery&) = delete;
    ServiceDiscovery& operator=(const ServiceDiscovery&) = delete;

    std::vector<std::string> queryRegistry(const std::string& service_name, const std::string& method_name);
    
    std::unordered_map<std::string, ServiceCacheItem> cache_;
    std::shared_mutex mutex_;
    std::condition_variable_any cv_;
};
