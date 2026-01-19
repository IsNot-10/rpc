#pragma once

#include "NonCopy.h"
#include <string>
#include <unordered_map>

class Buffer;

class HttpResponse
{
public:
    //各种响应状态码
    enum class HttpStatusCode
    {
        kUnknown,
        k200Ok=200,
        k301MovedPermanently=301,
        k400BadRequest=400,
        k404NotFound=404,
        k500InternalServerError=500,
        k503ServiceUnavailable=503
    };

public:
    explicit HttpResponse(bool close);

    //最核心的方法,把当前HttpResponse对象的各个属性转为字符串放入output缓冲区中
    //以便webserver将其发送给客户端
    void appendToBuffer(Buffer* output)const;


    //下面全是设置各种属性(不需要获取)
    void setStatusCode(HttpStatusCode code) 
    { 
        statusCode_=code; 
    }

    void setStatusMessage(std::string_view message) 
    { 
        statusMessage_=message; 
    }

    void setCloseConnection(bool on) 
    { 
        closeConnection_=on; 
    }

    bool closeConnection()const 
    { 
        return closeConnection_; 
    }

    void setContentType(const std::string& contentType) 
    { 
        addHeader("Content-Type",contentType); 
    }

    void addHeader(const std::string& key,const std::string& val) 
    { 
        headerMap_.emplace(key,val);
    }

    void setBody(std::string_view body) 
    { 
        body_=body; 
    }

private:
    HttpStatusCode statusCode_;      //响应报文的状态码   
    std::string statusMessage_;      //响应报文的状态信息
    bool closeConnection_;           //如果为true就是短连接,反之为长连接
    std::string body_;               //响应体

    //响应报文头列表
    std::unordered_map<std::string,std::string> headerMap_; 
};

