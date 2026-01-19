#pragma once

#include "TimeStamp.h"
#include <unordered_map>
#include <map>

class HttpRequest
{
public:
    enum class Method{kInvalid,kGet,kPost,kHead,kPut,kDelete,kOptions,kPatch};
    enum class Version{kUnknown,kHttp10,kHttp11};

public:
    HttpRequest();
    bool setMethod(const char* start,const char* end);
    const char* methodString()const;
    void setPath(const char* start,const char* end);
    void addHeader(const char* start,const char* colon,const char* end);
    const std::string& getHeader(const std::string& key)const;
    void swap(HttpRequest& rhs);


    //设置和获取HttpRequest对象的各种属性,没啥好说的
    //设置主要是HttpContext解析请求报文时做的
    //获取主要是根据HttpRequest对象生成HttpResponse对象时做的
    Method getMethod()const
    {
        return method_;
    }

    Version getVersion()const
    {
        return version_;
    }

    void setVersion(Version v)
    {
        version_=v;
    }

    const std::string& getPath()const
    {
        return path_;
    }

    const std::string& getQuery()const
    {
        return query_;
    }

    void setQuery(const char* start,const char* end)
    {
        query_.assign(start,end);
    }

    const std::string& getBody()const
    {
        return body_;
    }

    void setBody(const std::string& body)
    {
        body_=body;
    }

    TimeStamp getTime()const
    {
        return receiveTime_;
    }

    void setTime(TimeStamp t)
    {
        receiveTime_=t;
    }

    const std::unordered_map<std::string,std::string>& getHeaderMap()const
    {
        return headerMap_;
    }

    const std::unordered_map<std::string,std::string>& getQueryParameters()const;
    std::string getQueryParam(const std::string& key)const;
    void addQueryParam(const std::string& key, const std::string& value);

    void addPathParam(const std::string& key, const std::string& value) const
    {
        pathParameters_[key] = value;
    }

    std::string getPathParam(const std::string& key) const
    {
        auto it = pathParameters_.find(key);
        return it != pathParameters_.end() ? it->second : "";
    }

private:
    void parseQuery() const;
    static std::string urlDecode(const std::string& str);

private:
    Method method_;              //请求方法(本项目中肯定是get)
    Version version_;            //协议版本号
    std::string path_;           //请求路径
    std::string query_;          //查询参数
    std::string body_;           //请求体
    TimeStamp receiveTime_;      //请求时间

    //请求头的列表,前面四个数据成员都是在解析请求行的时候就设置好了
    //唯独它是在解析请求头的时候被设置的
    std::unordered_map<std::string,std::string> headerMap_;
    mutable std::unordered_map<std::string,std::string> queryParameters_;
    mutable bool queryParsed_ = false;
    mutable std::map<std::string, std::string> pathParameters_;
};

