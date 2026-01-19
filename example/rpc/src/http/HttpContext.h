#pragma once

#include "HttpRequest.h"

class Buffer;
class TimeStamp;


//这个类就是通过解析http报文去生成HttpRequest对象的,实际上算是一个反序列化的过程
//这里用到的设计模式是状态模式和解析器模式
class HttpContext
{
public:
    //解析的状态,分别对应:
    //kExpectRequestLine:初始状态,即将解析请求行
    //kExpectHeaders:请求行解析完成,即将解析请求头
    //kExpectBody:请求行和请求头解析完成,即将解析请求体
    //kGotAll:全部解析完成,请求报文复原成功(实际上是HttpRequest对象构造完成)
    enum class HttpRequestParseState
    {
        kExpectRequestLine,             
        kExpectHeaders,                
        kExpectBody,  
        kGotAll                
    };

public:
    HttpContext();
    void reset();

    //最最核心的函数,通过解析buf中的数据生成HttpRequest对象(复原请求报文)
    bool parseRequest(Buffer* buf,TimeStamp receiveTime);
    
    const HttpRequest& getRequest()const
    {
        return request_;
    }

    HttpRequest& getRequest()
    {
        return request_;
    }
    
    //是否解析完了?
    bool gotAll()const
    {
        return state_==HttpRequestParseState::kGotAll;
    }

private:
    //解析请求行
    bool processRequestLine(const char* begin,const char* end);

private:
    HttpRequestParseState state_;
    HttpRequest request_;
    size_t contentLength_ = 0;
};

