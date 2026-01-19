#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Logging.h"
#include <iostream>

using namespace std;

void sendResponse(const TcpConnectionPtr& conn, const std::string& msg) {
    HttpResponse resp(true);
    resp.setStatusCode(HttpResponse::HttpStatusCode::k200Ok);
    resp.setStatusMessage("OK");
    resp.setContentType("text/plain");
    resp.setBody(msg);
    Buffer buf;
    resp.appendToBuffer(&buf);
    conn->send(&buf);
}

int main() {
    EventLoop loop;
    InetAddress addr("0.0.0.0", 8000);
    HttpServer server(&loop, addr, "HttpServer");

    server.GET("/", [](const HttpRequest& req, const TcpConnectionPtr& conn){
        sendResponse(conn, "Hello World");
    });

    server.GET("/hello", [](const HttpRequest& req, const TcpConnectionPtr& conn){
        sendResponse(conn, "Hello " + req.getQuery());
    });
    
    server.GET("/api/v1/user", [](const HttpRequest&, const TcpConnectionPtr& conn){
        sendResponse(conn, "API V1 User");
    });

    server.GET("/api/v1/admin/info", [](const HttpRequest&, const TcpConnectionPtr& conn){
        sendResponse(conn, "API V1 Admin Info");
    });

    server.GET("/static/*", [](const HttpRequest& req, const TcpConnectionPtr& conn){
        sendResponse(conn, "Static file: " + req.getPath());
    });

    server.setThreadNum(4);
    server.start();
    loop.loop();
    return 0;
}
