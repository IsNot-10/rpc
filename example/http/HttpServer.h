#pragma once

#include "TcpServer.h"
#include "Router.h"
#include "HttpRequest.h"

class HttpRequest;
class HttpResponse;


//这个webserver只实现了解析get请求但是并没有实现解析post请求,功能不完善
class HttpServer
:NonCopy
{
public:
    using HttpCallback=std::function<void(const HttpRequest&,HttpResponse*)>;    

public:
    HttpServer(EventLoop *loop,const InetAddress& listenAddr, 
            std::string_view name,
            TcpServer::Option option=TcpServer::Option::kNoReusePort);
    void start();

    void setThreadNum(int num)
    {
        server_.setThreadNum(num);
    }

    void setHttpCallback(HttpCallback cb)
    {
        httpCallback_=std::move(cb);
    }

    void GET(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kGet, route, handler);
    }

    void POST(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kPost, route, handler);
    }

    void PUT(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kPut, route, handler);
    }

    void DELETE(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kDelete, route, handler);
    }

    void PATCH(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kPatch, route, handler);
    }

    void HEAD(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kHead, route, handler);
    }

    void OPTIONS(const std::string& route, const HttpHandler& handler)
    {
        router_.addRoute(HttpRequest::Method::kOptions, route, handler);
    }

    RouterGroup Group(const std::string& path)
    {
        return RouterGroup(&router_, path);
    }

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn,Buffer* buf,TimeStamp receiveTime);
    void onRequest(const TcpConnectionPtr& conn,const HttpRequest& req);

private:
    TcpServer server_;

    //这个回调函数的逻辑是:在webserver解析完http请求报文后,
    //如何生成相应的http响应报文
    HttpCallback httpCallback_;
    Router router_;
};

