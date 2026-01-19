#pragma once

#include "RouteTable.h"
#include <set>
#include <string>

class Router
{
public:
    void addRoute(HttpRequest::Method method, const std::string& route, const HttpHandler& handler);
    
    // Returns true if handled, false otherwise
    bool handle(const HttpRequest& req, const TcpConnectionPtr& conn) const;

private:
    RouteTable routeTable_;
    std::set<std::string> routes_; // Owns the route strings
};

class RouterGroup
{
public:
    RouterGroup(Router* router, const std::string& basePath)
        : router_(router), basePath_(basePath)
    {
    }

    void GET(const std::string& route, const HttpHandler& handler);
    void POST(const std::string& route, const HttpHandler& handler);
    void PUT(const std::string& route, const HttpHandler& handler);
    void DELETE(const std::string& route, const HttpHandler& handler);
    void PATCH(const std::string& route, const HttpHandler& handler);
    void HEAD(const std::string& route, const HttpHandler& handler);
    void OPTIONS(const std::string& route, const HttpHandler& handler);

    RouterGroup Group(const std::string& relativePath);

private:
    Router* router_;
    std::string basePath_;
};
