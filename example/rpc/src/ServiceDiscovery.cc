#include "ServiceDiscovery.h"
#include "MpRpcApplication.h"
#include "lb/lb_factory.h"
#include "lb/consistent_hash_lb.h"
#include "Logging.h"
#include "SocketAPI.h"
#include "InetAddress.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace {

// Helper to handle non-blocking connect with timeout
int connectWithTimeout(int sockfd, const struct sockaddr* addr, socklen_t addrlen, int timeout_ms) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    // Ensure non-blocking
    if (!(flags & O_NONBLOCK)) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    int res;
    do {
        res = connect(sockfd, addr, addrlen);
    } while (res < 0 && errno == EINTR);

    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sockfd, &wset);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            int s_ret;
            do {
                s_ret = select(sockfd + 1, nullptr, &wset, nullptr, &tv);
            } while (s_ret < 0 && errno == EINTR);

            if (s_ret > 0) {
                int err = SocketAPI::getSocketError(sockfd);
                if (err == 0) {
                    res = 0;
                } else {
                    res = -1;
                }
            } else {
                res = -1;
            }
        }
    }
    
    // Restore/Force Blocking mode for subsequent send/recv operations
    fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
    return res;
}

} // namespace

ServiceDiscovery& ServiceDiscovery::instance() {
    static ServiceDiscovery ins;
    return ins;
}

std::vector<std::string> ServiceDiscovery::queryRegistry(const std::string& service_name, const std::string& method_name) {
    std::vector<std::string> new_hosts;
    int clientfd = SocketAPI::createNonblocking();
    if (clientfd != -1) {
        std::string registryIp = MpRpcApplication::getInstance().Load("registry_ip");
        std::string portStr = MpRpcApplication::getInstance().Load("registry_port");
        uint16_t registryPort = portStr.empty() ? 8001 : atoi(portStr.c_str());
        
        if (registryIp.empty()) {
            registryIp = "127.0.0.1";
        }
        
        InetAddress server_addr(registryIp, registryPort);

        if (connectWithTimeout(clientfd, (sockaddr*)server_addr.getSockAddr(), sizeof(sockaddr_in), 500) == 0) {
            std::string msg = "DIS|" + service_name + "|" + method_name + "|\n";
            
            if (::send(clientfd, msg.c_str(), msg.size(), 0) != -1) {
                char buf[4096] = {0};
                
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
                    }
                }
            }
        }
        ::close(clientfd);
    }
    return new_hosts;
}

// Helper to create LoadBalancer and setup ConsistentHashRing if needed
static std::shared_ptr<LoadBalancer> createLoadBalancerWithRing(
    const std::vector<std::string>& hosts, 
    std::shared_ptr<ConsistentHashRing>& ring) 
{
    ring = std::make_shared<ConsistentHashRing>(160);
    ring->Build(hosts);
    
    std::string lb_algo = MpRpcApplication::getInstance().Load("load_balancer");
    auto new_lb = CreateLoadBalancer(lb_algo);
    
    if (lb_algo == "consistent_hash") {
        auto* chlb = dynamic_cast<ConsistentHashLB*>(new_lb.get());
        if (chlb) chlb->SetRing(ring);
    }
    
    return new_lb;
}

bool ServiceDiscovery::getHosts(const std::string& service_name, 
                                const std::string& method_name,
                                std::vector<std::string>& hosts,
                                std::shared_ptr<LoadBalancer>& lb)
{
    std::string cacheKey = service_name + ":" + method_name;
    const int CACHE_TTL_SEC = 3;
    
    // Use loop to handle wait & retry logic
    while (true) {
        bool should_update = false;
        std::vector<std::string> stale_hosts;
        
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = cache_.find(cacheKey);
            if (it != cache_.end()) {
                time_t now = time(nullptr);
                
                // Case 1: Cache hit and valid
                if (now < it->second.expire_time && !it->second.hosts.empty()) {
                    hosts = it->second.hosts;
                    lb = it->second.lb;
                    return true;
                }
                
                // Case 2: Query in progress by another thread (Wait)
                if (now < it->second.next_retry_time && it->second.hosts.empty()) {
                    // Wait for the query to finish (with timeout to prevent infinite blocking)
                    if (cv_.wait_for(lock, std::chrono::seconds(5)) == std::cv_status::timeout) {
                        LOG_WARN << "Timeout waiting for service discovery query for " << cacheKey;
                        // Fallback or retry? Let's break and try to query ourselves or return false
                        // If we return false, MpRpcChannel will handle failure
                        return false; 
                    }
                    continue; // Re-check after wakeup
                }

                // Case 3: Cooldown period but have stale data
                if (now < it->second.next_retry_time) {
                    hosts = it->second.hosts;
                    lb = it->second.lb;
                    if (!hosts.empty()) {
                        LOG_WARN << "Cache expired but in cooldown, using stale cache for " << cacheKey;
                        return true;
                    }
                    // If empty, should update (though likely blocked by retry time)
                    should_update = true;
                } 
                else {
                    // Case 4: Expired, need update
                    stale_hosts = it->second.hosts;
                    should_update = true;
                }
            } else {
                // Case 5: New entry
                should_update = true;
            }
        } // Drop read lock

        if (should_update) {
            bool my_turn = false;
            
            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                auto it = cache_.find(cacheKey);
                time_t now = time(nullptr);
                
                if (it != cache_.end()) {
                    // Double check after acquiring write lock
                    if (now < it->second.expire_time && !it->second.hosts.empty()) {
                        hosts = it->second.hosts;
                        lb = it->second.lb;
                        return true;
                    }
                    
                    // Check if someone else took the turn
                    if (now < it->second.next_retry_time) {
                         if (!it->second.hosts.empty()) {
                             hosts = it->second.hosts;
                             lb = it->second.lb;
                             return true;
                         }
                         // If empty and time set, someone else is working.
                         // Release lock and continue loop to wait
                         lock.unlock();
                         // We can wait on CV with a dummy lock or just continue to spin-wait in outer loop
                         // Since we are in a loop, 'continue' will take us to read lock and then wait.
                         continue; 
                    }
                    
                    it->second.next_retry_time = now + 10;
                    if (stale_hosts.empty()) stale_hosts = it->second.hosts;
                    my_turn = true;
                } else {
                    ServiceCacheItem item;
                    item.next_retry_time = now + 10;
                    cache_[cacheKey] = item;
                    my_turn = true;
                }
            } 

            if (my_turn) {
                std::vector<std::string> new_hosts = queryRegistry(service_name, method_name);
                bool success = !new_hosts.empty();
                
                {
                    std::unique_lock<std::shared_mutex> lock(mutex_);
                    auto& item = cache_[cacheKey];
                    
                    if (success) {
                        item.hosts = new_hosts;
                        item.expire_time = time(nullptr) + CACHE_TTL_SEC;
                        item.next_retry_time = 0; // Reset retry time so others can read
                        item.lb = createLoadBalancerWithRing(new_hosts, item.ch_ring);
                        
                        hosts = new_hosts;
                        lb = item.lb;
                    } else {
                        LOG_WARN << "Registry update failed or empty result for " << cacheKey;
                        if (!stale_hosts.empty()) {
                            hosts = stale_hosts;
                            if (item.hosts.empty()) {
                                item.hosts = stale_hosts;
                                item.lb = createLoadBalancerWithRing(stale_hosts, item.ch_ring);
                            }
                            lb = item.lb;
                        } else if (item.hosts.empty()) {
                            // Only use fallback if we really have nothing
                             std::vector<std::string> fallback;
                             fallback.push_back("127.0.0.1:9999");
                             LOG_WARN << "No provider available, fallback to 127.0.0.1:9999 for " << cacheKey;
                             item.hosts = fallback;
                             item.lb = createLoadBalancerWithRing(fallback, item.ch_ring);
                             hosts = fallback;
                             lb = item.lb;
                        } else {
                            // Keep existing (even if empty? shouldn't happen if we check stale)
                            hosts = item.hosts;
                            lb = item.lb;
                        }
                    }
                    
                    // Notify waiting threads
                    cv_.notify_all();
                }
                return true;
            }
        }
    }
}
