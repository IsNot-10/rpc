#include "Router.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Logging.h"

void Router::addRoute(HttpRequest::Method method, const std::string& route, const HttpHandler& handler)
{
    auto it = routes_.insert(route).first;
    std::string_view routeView = *it;
    
    VerbHandler& vh = routeTable_.findOrCreate(routeView);
    vh.handlerMap[method] = handler;
}

bool Router::handle(const HttpRequest& req, const TcpConnectionPtr& conn) const
{
    // 如果之前的修改保留，path 在 HttpRequest::setPath 中应该已经被 URL 解码了。
    // 假设 HttpRequest::getPath 返回的是用于匹配的路径。
    std::string path = req.getPath();
    LOG_INFO << "Router::handle path=" << path;
    
    std::map<std::string, std::string> params;
    std::string matchPath;
    
    auto it = routeTable_.find(path, params, matchPath);
    
    if (it != routeTable_.end() && it.handler)
    {
        auto method = req.getMethod();
        auto handlerIt = it.handler->handlerMap.find(method);
        if (handlerIt != it.handler->handlerMap.end())
        {
            // 将路径参数注入到请求中
            // 我们需要临时移除 const 属性来注入参数，
            // 或者我们应该让 HttpRequest 可变，或者传递一个副本。
            // 目前，我们使用 const_cast 作为这种类中间件行为的实用解决方案。
            HttpRequest& mutableReq = const_cast<HttpRequest&>(req);
            for (const auto& p : params) {
                mutableReq.addPathParam(p.first, p.second);
            }
            
            handlerIt->second(req, conn);
            return true;
        } else {
             LOG_INFO << "Method mismatch: req=" << (int)method;
        }
    } else {
        LOG_INFO << "No route match for path: " << path;
    }
    return false;
}

// RouterGroup implementation

static std::string joinPaths(const std::string& base, const std::string& path)
{
    std::string result = base;
    if (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    
    if (path.empty()) {
        return result.empty() ? "/" : result;
    }
    
    if (path[0] != '/') {
        result += '/';
    }
    
    result += path;
    return result;
}

void RouterGroup::GET(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kGet, joinPaths(basePath_, route), handler);
}

void RouterGroup::POST(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kPost, joinPaths(basePath_, route), handler);
}

void RouterGroup::PUT(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kPut, joinPaths(basePath_, route), handler);
}

void RouterGroup::DELETE(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kDelete, joinPaths(basePath_, route), handler);
}

void RouterGroup::PATCH(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kPatch, joinPaths(basePath_, route), handler);
}

void RouterGroup::HEAD(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kHead, joinPaths(basePath_, route), handler);
}

void RouterGroup::OPTIONS(const std::string& route, const HttpHandler& handler)
{
    router_->addRoute(HttpRequest::Method::kOptions, joinPaths(basePath_, route), handler);
}

RouterGroup RouterGroup::Group(const std::string& relativePath)
{
    return RouterGroup(router_, joinPaths(basePath_, relativePath));
}
