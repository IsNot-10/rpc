#include "HttpContext.h"
#include "Buffer.h"
#include "TimeStamp.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace {

}

HttpContext::HttpContext()
:state_(HttpRequestParseState::kExpectRequestLine), contentLength_(0)
{}


//重置请求报文(HttpRequest对象)
void HttpContext::reset()
{
    state_=HttpRequestParseState::kExpectRequestLine;
    contentLength_ = 0;
    HttpRequest temp;
    request_.swap(temp);
}




//只解析请求行的过程,作为parseRequest的工具函数
//举个请求行的例子方便理解

// GET /index.html?id=acer HTTP/1.1
bool HttpContext::processRequestLine(const char* begin,const char* end)
{
    bool succeed=false;
    const char* start=begin;
    const char* space=std::find(start,end,' ');
    if(space!=end&&request_.setMethod(start,space))
    {
        start=space+1;
        space=std::find(start,end,' ');
        if(space!=end)
        {
            const char* question=std::find(start,space,'?');
            if(question!=space)
            {
                request_.setPath(start,question);
                request_.setQuery(question,space);
                
                // 更推荐使用延迟解析 (Lazy parsing)
                // 推荐使用延迟解析 (Lazy Parsing)。
                // 我们不在这里立即解析所有查询参数，以提高简单请求的处理性能。
            }
            else  request_.setPath(start,space);
            start=space+1;
            succeed=end-start==8&&std::equal(start,end-1,"HTTP/1.");
            if(succeed)
            {
                if(*(end-1)=='1')  
                {
                    request_.setVersion(HttpRequest::Version::kHttp11);
                }
                else if(*(end-1)=='0')  
                {
                    request_.setVersion(HttpRequest::Version::kHttp10);
                }
                else  succeed=false;
            }
        }
    }
    return succeed;
}





//举个http get请求报文(只有请求行和请求头)的例子
// GET /index.html?id=acer HTTP/1.1
// Host: 127.0.0.1:8002
// User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:102.0) Gecko/20100101 Firefox/102.0
// Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8
// Accept-Language: en-US,en;q=0.5
// Accept-Encoding: gzip, deflate, br
// Connection: keep-alive

bool HttpContext::parseRequest(Buffer* buf,TimeStamp receiveTime)
{
    bool ok=true;                                
    bool hasMore=true;
    // Max request size check (8MB) to prevent DoS
    // 检查最大请求大小 (8MB) 以防止 DoS 攻击
    static const size_t kMaxRequestSize = 8 * 1024 * 1024;
    
    while(hasMore)
    {
        if (buf->readableBytes() > kMaxRequestSize) {
            ok = false;
            hasMore = false;
            break;
        }

        if(state_==HttpRequestParseState::kExpectRequestLine)
        {
            //找到\r\n的位置
            const char* crlf=buf->findCRLF();     
            if(crlf)                              
            {
                //解析请求行
                ok=processRequestLine(buf->peek(),crlf); 

                //成功就给HttpRequest对象设置接收时间
                //失败就直接退出循环
                if(ok)
                {
                    request_.setTime(receiveTime);
                    buf->retrieveUntil(crlf+2);   
                    state_=HttpRequestParseState::kExpectHeaders;       
                }
                else  hasMore=false;
            }
            else  hasMore=false;
        }

        //解析请求头
        else if(state_==HttpRequestParseState::kExpectHeaders)
        {
            const char* crlf =buf->findCRLF();
            if(crlf)
            {
                const char* colon=std::find(buf->peek(),crlf,':');
                if(colon!=crlf)
                {
                    request_.addHeader(buf->peek(),colon,crlf);
                }

                //请求头解析完成后的空行
                else
                {
                    // Check for Content-Length
                    // 检查 Content-Length 头部
                    const std::string& lenStr = request_.getHeader("Content-Length");
                    if (!lenStr.empty())
                    {
                        contentLength_ = std::strtoul(lenStr.c_str(), nullptr, 10);
                        if (contentLength_ > 0) {
                            state_ = HttpRequestParseState::kExpectBody;
                        } else {
                            state_ = HttpRequestParseState::kGotAll;
                            hasMore = false;
                        }
                    }
                    else
                    {
                        state_=HttpRequestParseState::kGotAll;
                        hasMore=false;
                    }
                }
                buf->retrieveUntil(crlf+2);
            }
            else  hasMore=false;
        }

        else if(state_==HttpRequestParseState::kExpectBody)
        {
             if(buf->readableBytes() >= contentLength_)
             {
                 request_.setBody(buf->retrieveAsString(contentLength_));
                 state_ = HttpRequestParseState::kGotAll;
                 hasMore = false;
             }
             else
             {
                 hasMore = false;
             }
        }
    }
    return ok;
}
