#include "HttpServer.h"
#include "HttpContext.h"
#include "HttpResponse.h"


//默认的生成http响应报文函数,完全不解析请求报文直接返回404
void defaultHttpCallback(const HttpRequest& req,HttpResponse* resp)
{
    resp->setStatusCode(HttpResponse::HttpStatusCode::k404NotFound);
    resp->setStatusMessage("Not Found");
    resp->setCloseConnection(true);
}



HttpServer::HttpServer(EventLoop *loop,const InetAddress& listenAddr, 
    std::string_view name,TcpServer::Option option)
:server_(loop,listenAddr,name,option)
,httpCallback_(defaultHttpCallback)
{
    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& conn)
        {
            onConnection(conn);
        });
    server_.setMessageCallback(
        [this](const TcpConnectionPtr& conn,Buffer* buf,TimeStamp receiveTime)
        {
            onMessage(conn,buf,receiveTime);
        });
}



void HttpServer::start()
{
    LOG_INFO<<"HttpServer["<<server_.getName()
        <<"] starts listening on "<<server_.getIpPort();
    server_.start();
}




//实际上不一定要在建立连接的时候把HttpContext对象作为TcpConnection的数据成员
//接受消息的时候额外构造也是可以的,这里只是展示muduo网络库提供的弹性
void HttpServer::onConnection(const TcpConnectionPtr& conn)
{
    if(conn->connected())  
    {
        LOG_DEBUG<<"new Connection arrived";
        conn->setContext(HttpContext{});
    }
    else
    {
        LOG_DEBUG<<"Connection closed, name = "<<conn->getName();
    }
}



void HttpServer::onMessage(const TcpConnectionPtr& conn,Buffer* buf,TimeStamp receiveTime)
{
    HttpContext* context=std::any_cast<HttpContext>(conn->getMutableContext());
    
    //解析请求报文失败,直接发送错误信息并断开连接(实际上是关闭写端)
    if(!context->parseRequest(buf,receiveTime))
    {
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();
    }

    //这个逻辑对应解析请求报文成功,HttpRequest对象也被成功构造
    //那就根据HttpRequest对象去生成HttpResponse对象(实际就是为了生成响应报文)
    //最后把响应报文相关的信息发送过去(以上逻辑均由onRequest实现)
    if(context->gotAll())
    {
        onRequest(conn,context->getRequest());
        context->reset();
    }
}



//根据成功解析出来的httpRequest对象去生成httpResponse对象(由httpCallback_实现)
//最后再把httpResponse对象的信息全部放到缓冲区,并发送给客户端
void HttpServer::onRequest(const TcpConnectionPtr& conn,const HttpRequest& req)
{
    //这里需要判断一下是长连接还是短连接
    const std::string& connection=req.getHeader("Connection");
    bool close=(connection=="close"||
        ((req.getVersion()==HttpRequest::Version::kHttp10
        &&connection!="Keep-Alive")));

    //构造响应报文的关键
    HttpResponse response{close};
    
    // 首先尝试路由器
    if (router_.handle(req, conn))
    {
        return; // 请求已由路由器处理
    }

    if(httpCallback_)
    {
        httpCallback_(req,&response);
    }
    
    Buffer buf;
    response.appendToBuffer(&buf);
    conn->send(&buf);
    
    if(response.closeConnection())
    {
        conn->shutdown();
    }
}