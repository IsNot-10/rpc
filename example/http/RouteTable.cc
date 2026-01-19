#include "RouteTable.h"
#include <cassert>
#include <iostream>

RouteTableNode::~RouteTableNode()
{
    for (auto& pair : children_)
    {
        delete pair.second;
    }
}

VerbHandler& RouteTableNode::findOrCreate(std::string_view route, size_t cursor)
{
    if (cursor == route.size())
        return verbHandler_;

    // Handle root "/"
    if (cursor == 0 && route == "/")
    {
        auto it = children_.find(route);
        if (it != children_.end())
        {
            return it->second->findOrCreate(route, ++cursor);
        }
        else
        {
            auto* newNode = new RouteTableNode();
            children_.emplace(route, newNode);
            return newNode->findOrCreate(route, ++cursor);
        }
    }

    // Ignore trailing slash if we are at the end
    if (cursor == route.size() - 1 && route[cursor] == '/')
    {
        return verbHandler_;
    }

    if (route[cursor] == '/')
        cursor++; // skip the /
    
    size_t anchor = cursor;
    while (cursor < route.size() && route[cursor] != '/')
        cursor++;
    
    // get the part between slashes
    std::string_view mid = route.substr(anchor, cursor - anchor);
    
    auto it = children_.find(mid);
    if (it != children_.end())
    {
        return it->second->findOrCreate(route, cursor);
    }
    else
    {
        auto* newNode = new RouteTableNode();
        children_.emplace(mid, newNode);
        return newNode->findOrCreate(route, cursor);
    }
}

RouteTableNode::Iterator RouteTableNode::find(std::string_view route,
                                              size_t cursor,
                                              std::map<std::string, std::string>& params,
                                              std::string& matchPath) const
{
    // cursor is the current position in the request path (route)
    
    // We found the route (reached end of path)
    if (cursor == route.size())
    {
        // If this node has handlers or no children (exact match)
        // logic from wfrest: if (!verb_handler_.verb_handler_map.empty() || children_.empty())
        if (!verbHandler_.handlerMap.empty() || children_.empty())
        {
            return {this, route, &verbHandler_};
        }
    }

    // Check for wildcard match at the end
    // wfrest: if(cursor == route.size() && !children_.empty())
    if (cursor == route.size() && !children_.empty())
    {
        auto it = children_.find("*");
        if (it != children_.end())
        {
            return {it->second, route, &it->second->verbHandler_};
        }
    }

    // If no handlers and reached end, return end
    if (cursor == route.size() && verbHandler_.handlerMap.empty())
    {
        return {nullptr, route, nullptr};
    }

    // Handle root "/"
    if (cursor == 0 && route == "/")
    {
        auto it = children_.find(route);
        if (it != children_.end())
        {
            auto it2 = it->second->find(route, ++cursor, params, matchPath);
            if (it2 != it->second->end())
                return it2;
        }
    }

    if (cursor < route.size() && route[cursor] == '/')
        cursor++; // skip the first /
    
    size_t anchor = cursor;
    while (cursor < route.size() && route[cursor] != '/')
        cursor++;

    std::string_view mid = route.substr(anchor, cursor - anchor);

    // 1. Look for exact match in children
    auto it = children_.find(mid);
    if (it != children_.end())
    {
        auto it2 = it->second->find(route, cursor, params, matchPath);
        if (it2 != it->second->end())
            return it2;
    }

    // 2. Look for patterns (wildcard or params)
    for (auto& kv : children_)
    {
        std::string_view param = kv.first;
        
        // Wildcard match: e.g. /static/*
        // wfrest logic: if (!param.empty() && param[param.size() - 1] == '*')
        if (!param.empty() && param.back() == '*')
        {
            std::string_view match = param;
            match.remove_suffix(1); // remove '*'
            if (mid.substr(0, match.size()) == match)
            {
                std::string_view matchPathSuffix = route.substr(cursor);
                matchPath = std::string(mid) + std::string(matchPathSuffix);
                return {kv.second, route, &kv.second->verbHandler_};
            }
        }

        // Path param match: :id
        if (param.size() > 1 && param.front() == ':')
        {
            // Extract param name
            std::string_view key = param.substr(1);

            params[std::string(key)] = std::string(mid);
            return kv.second->find(route, cursor, params, matchPath);
        }
    }

    return end();
}

VerbHandler& RouteTable::findOrCreate(std::string_view route)
{
    return root_.findOrCreate(route, 0);
}
