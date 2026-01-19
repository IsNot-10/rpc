#pragma once

#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <string>
#include <string_view>
#include <functional>
#include "HttpRequest.h"
#include "TcpConnection.h"

// Define HttpHandler based on mymuduo's callback signature
using HttpHandler = std::function<void(const HttpRequest&, const TcpConnectionPtr&)>;

struct VerbHandler
{
    std::map<HttpRequest::Method, HttpHandler> handlerMap;
};

class RouteTableNode
{
public:
    ~RouteTableNode();

    VerbHandler& findOrCreate(std::string_view route, size_t cursor);

    struct Iterator
    {
        const RouteTableNode* ptr;
        std::string_view route; 
        const VerbHandler* handler;
        
        bool operator!=(const Iterator& other) const { return ptr != other.ptr; }
    };

    Iterator find(std::string_view route,
                  size_t cursor,
                  std::map<std::string, std::string>& params,
                  std::string& matchPath) const;

    Iterator end() const { return {nullptr, {}, nullptr}; }

private:
    VerbHandler verbHandler_;
    std::map<std::string_view, RouteTableNode*> children_;
    
    friend class RouteTable;
};

class RouteTable
{
public:
    RouteTable() = default;
    ~RouteTable() = default;

    // route string must outlive RouteTable (e.g. stored in Router)
    VerbHandler& findOrCreate(std::string_view route);
    
    RouteTableNode::Iterator find(std::string_view route,
                                  std::map<std::string, std::string>& params,
                                  std::string& matchPath) const
    {
        return root_.find(route, 0, params, matchPath);
    }

    RouteTableNode::Iterator end() const { return root_.end(); }

private:
    RouteTableNode root_;
};
