#include "EchoServer.h"
#include "AsyncLogging.h"


int main(int argc,char* argv[])
{
    //Logger::setLogLevel(Logger::DEBUG);

    const off_t kRollSize=8*1024*1024;
    AsyncLogging log{argv[0],kRollSize};

    //这里就是把AsyncLogging对象的append成员函数设为输出回调
    //所有前端线程都会在打印日志完毕后,把日志信息放到4MB大小的缓冲区
    Logger::setOutput(
        [&log](const char* msg,int len)
        {
            log.append(msg,len);
        });


    //启动写入文件的日志后端线程
    log.start();


    
    EventLoop loop;
    InetAddress addr;

    //设置最大连接数为5000
    EchoServer server{&loop,addr,"EchoServer",8,5000};
    server.setThreadNum(4);
    server.start();
    loop.loop();
    return 0;
}

