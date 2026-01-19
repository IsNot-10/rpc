#include "RegistryServer.h"
#include "EventLoop.h"
#include "InetAddress.h"

int main(int argc, char* argv[])
{
    // 创建事件循环（Reactor）
    EventLoop loop;
    
    // 监听地址和端口 (0.0.0.0:8001)
    InetAddress addr("0.0.0.0", 8001);
    
    // 创建注册中心服务实例
    RegistryServer server(&loop, addr);
    
    // 启动服务
    server.start();
    
    // 开始事件循环，阻塞等待网络事件
    loop.loop();
    
    return 0;
}
