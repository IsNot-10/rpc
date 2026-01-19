#include "EchoServer.h"
#include "TimerId.h"
#include "Logging.h"



EchoServer::EchoServer(EventLoop* loop,const InetAddress& listenAddr
    ,std::string_view name,int idleSeconds,int maxConnects)
:server_(loop,listenAddr,name)
,bucketBuffer_(idleSeconds)
,numConnected_(0),maxConnects_(maxConnects)
{
    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& conn)
        {
            onConnection(conn);
        });
    server_.setMessageCallback(
        [this](const TcpConnectionPtr& conn,
            Buffer* buffer,TimeStamp receiveTime)
        {
            onMessage(conn,buffer,receiveTime);
        });

    // Initialize the bucket buffer with empty buckets
    for(int i=0; i<idleSeconds; ++i)
    {
        bucketBuffer_.push_back(Bucket{});
    }

    //往红黑树定时器列表中插入一个每隔1秒就会调用超时回调函数onTime的定时器
    loop->runEvery(1.0,[this]()
                    {
                        onTimer();
                    });
}




void EchoServer::onConnection(const TcpConnectionPtr& conn)
{
    if(conn->connected())
    {
        if(++numConnected_>maxConnects_)  conn->shutdown();
        else
        {
            LOG_INFO<<"Connection UP : "<<conn->getPeerAddr().getIpPort();
            EntryPtr entry=std::make_shared<Entry>(conn);
            bucketBuffer_.back().insert(entry);

            //注意要让TcpConnection对象保存一下,因为接受到消息调用onMessage函数
            //的时候需要从中获取
            weakEntryPtr weakEntry{entry};
            conn->setContext(weakEntry);
        }
    }
    else
    {
        LOG_INFO<<"Connection DOWN : "<<conn->getPeerAddr().getIpPort();
        weakEntryPtr weakEntry=std::any_cast<weakEntryPtr>(conn->getContext());
        LOG_DEBUG<<"Entry use_count="<<weakEntry.use_count();
        --numConnected_;
    }
    //dumpBucketBuffer();
}




void EchoServer::onMessage(const TcpConnectionPtr& conn,
    Buffer* buffer,TimeStamp receiveTime)
{
    buffer->retrieveAll();
    static const std::string response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    conn->send(response);

    //接受到消息,那么说明时间轮队列需要把这个tcp连接加入到尾部
    //从哪里获取?TcpConnection对象内部保存了弱引用指针,就从这里获取.
    weakEntryPtr weakEntry=std::any_cast<weakEntryPtr>(conn->getContext());
    EntryPtr entry=weakEntry.lock();
    if(entry)  
    {
        bucketBuffer_.back().insert(entry);
        //dumpBucketBuffer();
    }
}



//实际上这个超时回调很简单,单纯的往时间轮队列中加入一个空格子(但把队首的格子析构了)
//队首格子内部的tcp连接就是超时的空闲tcp连接,但如果这个tcp连接弱引用指针在后面的
//格子中依然存在就不会被析构,当前仅当它是最后一个弱引用计数的时候才会真正的析构
void EchoServer::onTimer()
{
    bucketBuffer_.push_back(Bucket{});
    //dumpBucketBuffer();
}




void EchoServer::dumpBucketBuffer()
{
    LOG_DEBUG<<"size="<<bucketBuffer_.size();
    int idx=0;
    for(auto bucketI=bucketBuffer_.begin();
        bucketI!=bucketBuffer_.end();++bucketI,++idx)
    {
        const Bucket& bucket=*bucketI;
        LOG_DEBUG<<"the "<<idx<<"th bucket"<<" has "
            <<bucket.size()<<" connections";
        for(const auto& it:bucket)
        {
            bool connectionDead=it->conn_.expired();
            LOG_DEBUG<<"the connection ("<<static_cast<void*>(it.get()) 
                <<") use_count is "<<it.use_count()
                <<(connectionDead?" (DEAD)":" (ALIVE)");
        }
    }
}
