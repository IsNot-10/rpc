#include "ChatServer.h"
#include "AsyncLogging.h"
#include <signal.h>

void resetHandler(int)
{
    LOG_INFO<<"capture the SIGINT,will reset state";
    ChatServer::closeExceptionReset();
    exit(0);
}


int main(int argc,char* argv[])
{
    if(argc<3)
    {
        LOG_FATAL<<"参数太少!必须输入./ChatServer 网络号 端口号";
    }

    //设置异步日志
    const off_t kRollSize=8*1024*1024;
    AsyncLogging async_log{argv[0],kRollSize};
    Logger::setOutput(
        [&async_log](const char* msg,int len)
        {
            async_log.append(msg,len);
        }
    );
    async_log.start();    //启动后端写日志文件的线程

    char* ip=argv[1];
    uint16_t port=atoi(argv[2]);
    InetAddress addr{ip,port};
    EventLoop loop;
    ChatServer server{&loop,addr,argv[0]};
    server.setThreadNum(4);

    //设置信号回调函数,在终端按下ctrl+c的时候就会触发resetHandler函数然后
    //正常退出.(而不像原来一样直接杀死进程)
    ::signal(SIGINT,resetHandler);
    
    //正式启动ChatServer
    server.start();
    loop.loop();
    return 0;
}