#include "HttpResponse.h"
#include "Buffer.h"

HttpResponse::HttpResponse(bool close)
:statusCode_(HttpStatusCode::kUnknown),closeConnection_(close)
{}



/*举一个典型响应报文的例子,方便理解
*   HTTP/1.1 200 OK 
*   Date:Mon,31Dec200104:25:57GMT 
*   Server:Apache/1.3.14(Unix) 
*   Content-type:text/html 
*   Last-modified:Tue,17Apr200106:46:28GMT 
*   Etag:"a030f020ac7c01:1e9f" 
*   Content-length:39725426 
*   Content-range:bytes554554-40279979/40279980
*/

//这个函数实际就是把HttpResponse中的数据放到output缓冲区(可以理解成序列化吧)
//http业务的序列化很简单,用"\r\n"标记作为分隔符即可
void HttpResponse::appendToBuffer(Buffer* output)const
{
    //先序列化响应行
    char buf[32]={0};
    snprintf(buf,sizeof buf,"HTTP/1.1 %d ",static_cast<int>(statusCode_));
    output->append(buf);
    output->append(statusMessage_);
    output->append("\r\n");

    snprintf(buf,sizeof buf,"Content-Length: %zd\r\n",body_.size());
    output->append(buf);

    if(closeConnection_)  output->append("Connection: close\r\n");
    else
    {
        output->append("Connection: Keep-Alive\r\n");
    }

    //这里序列化响应头
    for(const auto& [key,val]:headerMap_)
    {
        output->append(key);
        output->append(": ");
        output->append(val);
        output->append("\r\n");
    }

    //最后序列化响应体
    output->append("\r\n");
    output->append(body_);
}