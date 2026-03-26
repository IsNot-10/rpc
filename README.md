# MpRpc - 高性能分布式 RPC 框架

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

基于 Reactor 模式的高性能分布式 RPC 框架，参考 brpc 设计理念，提供完整的服务治理能力。

## 特性

- **Reactor 网络模型**：One Loop Per Thread，基于 epoll
- **Protobuf 序列化**：高效的服务定义和序列化
- **多策略负载均衡**：加权轮询、一致性哈希、延迟感知(LALB)
- **高可用机制**：熔断器、限流器、并发控制
- **可观测性**：Prometheus 指标、分布式追踪、监控面板
- **服务治理**：服务注册中心、HTTP 网关

## 核心组件

### 负载均衡

| 策略 | 说明 |
|------|------|
| Weighted Round Robin | 加权轮询，支持动态权重 |
| Consistent Hash | 一致性哈希，支持虚拟节点 |
| LALB | 延迟感知负载均衡，自动根据响应时间调整权重 |

### 高可用

| 组件 | 说明 |
|------|------|
| Circuit Breaker | 基于 EMA 的熔断器，自动隔离故障节点 |
| Rate Limiter | 令牌桶限流器 |
| Concurrency Limiter | 自动并发度控制 |

### 监控

| 组件 | 说明 |
|------|------|
| Metrics | Counter、Gauge、Histogram 指标 |
| Tracing | 基于 OpenTracing 的分布式追踪 |
| Dashboard | 内置 Web 监控面板 |

## 快速开始

### 依赖

- C++17
- Protobuf >= 3.0
- CMake >= 3.10

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 定义服务

```protobuf
// echo.proto
syntax = "proto3";

service EchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
}

message EchoRequest {
    string message = 1;
}

message EchoResponse {
    string message = 1;
}
```

### 服务端

```cpp
#include "MpRpcProvider.h"
#include "echo.pb.h"

class EchoServiceImpl : public EchoService {
public:
    void Echo(google::protobuf::RpcController* controller,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        response->set_message("Echo: " + request->message());
        done->Run();
    }
};

int main() {
    MpRpcApplication::Init("server.conf");
    
    MpRpcProvider provider;
    provider.notifyService(new EchoServiceImpl());
    provider.run();
    
    return 0;
}
```

### 客户端

```cpp
#include "MpRpcChannel.h"
#include "echo.pb.h"

int main() {
    MpRpcApplication::Init("client.conf");
    
    EchoService_Stub stub(new MpRpcChannel());
    
    EchoRequest request;
    request.set_message("Hello RPC");
    
    EchoResponse response;
    MpRpcController controller;
    
    stub.Echo(&controller, &request, &response, nullptr);
    
    if (!controller.Failed()) {
        std::cout << response.message() << std::endl;
    }
    
    return 0;
}
```

### 配置文件

```ini
# server.conf
rpcserver_ip=0.0.0.0
rpcserver_port=8001
registry_ip=127.0.0.1
registry_port=9000
```

## 目录结构

```
rpc/
├── src/
│   ├── net/                    # 网络层 (Reactor)
│   │   ├── EventLoop.h
│   │   ├── TcpServer.h
│   │   ├── TcpConnection.h
│   │   └── ...
│   ├── base/                   # 基础组件
│   ├── timer/                  # 定时器
│   ├── logger/                 # 日志系统
│   └── ...
├── example/
│   ├── rpc/                    # RPC 框架示例
│   │   ├── src/
│   │   │   ├── MpRpcProvider.cc/h
│   │   │   ├── MpRpcChannel.cc/h
│   │   │   ├── lb/             # 负载均衡
│   │   │   ├── ha/             # 高可用
│   │   │   ├── metrics/        # 监控指标
│   │   │   └── tracing/        # 分布式追踪
│   │   └── example/
│   │       ├── callee/         # 服务端
│   │       └── caller/         # 客户端
│   ├── registry/               # 服务注册中心
│   ├── gateway/                # API 网关
│   ├── http/                   # HTTP 服务器
│   └── chat/                   # 聊天服务器示例
└── CMakeLists.txt
```

## 监控面板

访问 `http://localhost:port/status` 查看实时监控：

- QPS 统计
- 延迟分布 (P50/P90/P99)
- 错误率
- 节点健康状态

## 许可证

MIT License
