#pragma once

#include "TcpServer.h"
#include "CircularBuffer.h"
#include <unordered_set>


//这是个简单的"乒乓"服务器,服务端收到客户端的数据就会立刻把数据发送会客户端

//另外也限制了最大连接个数,注意这跟Accept类里那个逻辑不一样,那个是因为fd数量
//达到上限,是系统级别的限制.而这里单纯只是应用层上的限制.

//同时也使用时间轮踢掉超时连接.如果一个连接在idleSeconds秒内没有收到数据就认为它
//是空闲连接,服务器把这个连接shutdown掉(关闭写端).

class EchoServer
{
public:
    EchoServer(EventLoop* loop,const InetAddress& listenAddr
        ,std::string_view name,int idleSeconds,int maxConnects);

    void start()
    {
        server_.start();
    }

    void setThreadNum(int num)
    {
        server_.setThreadNum(num);
    }

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn,
        Buffer* buffer,TimeStamp receiveTime);
    
    //定时器的超时回调函数
    void onTimer();

    //单纯debug打印用
    void dumpBucketBuffer();

private:        
    //每个Entry对象包含TcpConnection对象的弱引用指针
    //Entry对象本身也会被std::shared_ptr管理生命周期
    
    //在析构函数中(实际上就是被时间轮踢出来并且引用计数真为0了)会观察这个tcp连接
    //是否还存活,如果还存活就会把这个连接shutdown
    struct Entry
    {
        explicit Entry(const TcpConnectionPtr& conn)
        :conn_(conn)
        {}
        
        ~Entry()
        {
            TcpConnectionPtr conn=conn_.lock();
            if(conn)  conn->shutdown();    
        }

        std::weak_ptr<TcpConnection> conn_;
    };

    using EntryPtr=std::shared_ptr<Entry>;
    using weakEntryPtr=std::weak_ptr<Entry>;
    using Bucket=std::unordered_set<EntryPtr>;

private:
    //底层使用muduo网络库的任何高层服务器都是需要封装一个TcpServer对象的
    //实际上也可也加一个EventLoop指针作为数据成员,但是并非没必要
    TcpServer server_;

    


    //时间轮的核心,数据结构是一个固定大小的队列(逻辑上是环形但实际不是)
    //它尾部每增加一个元素都会把头部的元素析构掉
    
    //假如我现在的规定是:超过8秒没有收到数据的连接会断开
    //那么这个队列的缓冲区就是固定8个格子,每个格子都是一个哈希集合,可以理解成这些哈希
    //集合内部都是TcpConnection对象的弱引用指针,而如果同一个TcpConnection弱引用指针
    //也是需要记录数量的,如果数量被减到0,那么肯定要把tcp连接断开了

    //另外格子结构选用std::unordered_set是为了去重,防止同一个连接1s内被加入多次
    //也就是说每个格子只能出现一个相同的tcp连接的弱引用指针
    CircularBuffer<Bucket> bucketBuffer_;

    //当前连接数以及规定的最大连接数
    int numConnected_;
    const int maxConnects_;
};

