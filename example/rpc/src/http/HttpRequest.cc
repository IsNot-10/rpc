#include "HttpRequest.h"
#include <string_view>
#include <sstream>
#include <iomanip>

HttpRequest::HttpRequest()
:method_(Method::kInvalid),version_(Version::kUnknown),queryParsed_(false)
{}



//把字符串转化为请求方法(枚举类)
bool HttpRequest::setMethod(const char* start,const char* end)
{
    std::string_view m(start, end - start);
    if(m=="GET")  method_=Method::kGet;
    else if(m=="POST")  method_=Method::kPost;
    else if(m=="HEAD")  method_=Method::kHead;
    else if(m=="PUT")  method_=Method::kPut;
    else if(m=="DELETE")  method_=Method::kDelete;
    else if(m=="OPTIONS") method_=Method::kOptions;
    else if(m=="PATCH")   method_=Method::kPatch;
    else  method_=Method::kInvalid;
    return method_!=Method::kInvalid;
}



//和上面操作相反,把请求方法(枚举类)转化为字符串
const char* HttpRequest::methodString()const
{
    const char* result="UNKNOWN";
    switch(method_)
    {
        case Method::kGet:result="GET";break;
        case Method::kPost:result="POST";break;
        case Method::kHead:result="HEAD";break;
        case Method::kPut:result="PUT";break;
        case Method::kDelete:result="DELETE";break;
        case Method::kOptions:result="OPTIONS";break;
        case Method::kPatch:result="PATCH";break;
        default: break;
    }
    return result;
}

void HttpRequest::setPath(const char* start,const char* end)
{
    std::string encoded(start, end);
    path_ = urlDecode(encoded);
}



//根据请求头信息,在哈希表中增加相应记录
void HttpRequest::addHeader(const char* start,const char* colon,const char* end)
{
    std::string field{start,colon};
    ++colon;

    //把空格都跳过去
    while(colon<end&&::isspace(*colon))  ++colon;
    std::string value{colon,end};
    while(value.size()&&::isspace(value[value.size()-1]))
    {
        value.resize(value.size()-1);
    }
    headerMap_[field]=value;
}



//根据key去获取请求头列表的val,找不到就返回空字符串
const std::string& HttpRequest::getHeader(const std::string& key)const
{
    const auto it=headerMap_.find(key);
    if(it!=headerMap_.end())  return it->second;
    static const std::string empty;
    return empty;
}



//两个HttpRequest对象的交换操作,其实就是各个数据成员的交换
void HttpRequest::swap(HttpRequest& rhs)
{
    std::swap(method_,rhs.method_);
    std::swap(version_,rhs.version_);
    path_.swap(rhs.path_);
    query_.swap(rhs.query_);
    body_.swap(rhs.body_);
    std::swap(receiveTime_,rhs.receiveTime_);
    headerMap_.swap(rhs.headerMap_);
    queryParameters_.swap(rhs.queryParameters_);
    std::swap(queryParsed_, rhs.queryParsed_);
}

const std::unordered_map<std::string,std::string>& HttpRequest::getQueryParameters()const
{
    parseQuery();
    return queryParameters_;
}

std::string HttpRequest::getQueryParam(const std::string& key)const
{
    parseQuery();
    auto it = queryParameters_.find(key);
    return it != queryParameters_.end() ? it->second : "";
}

void HttpRequest::addQueryParam(const std::string& key, const std::string& value)
{
    queryParameters_[key] = value;
}

std::string HttpRequest::urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int hex_value;
            std::istringstream hex_stream(str.substr(i + 1, 2));
            if (hex_stream >> std::hex >> hex_value) {
                result += static_cast<char>(hex_value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

void HttpRequest::parseQuery() const
{
    if (queryParsed_) return;
    
    if (query_.empty()) {
        queryParsed_ = true;
        return;
    }

    // Parse query parameters
    // The query_ string does not contain the leading '?' because it was set using (question+1, space) in HttpContext
    // Wait, let's check HttpContext.cc.
    // In HttpContext.cc: request_.setQuery(question,space);
    // question points to '?', so query_ includes '?'.
    
    size_t startPos = 0;
    if (!query_.empty() && query_[0] == '?') {
        startPos = 1;
    }
    
    std::string queryString = query_.substr(startPos);
    std::stringstream ss(queryString);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = urlDecode(pair.substr(0, eq_pos));
            std::string value = urlDecode(pair.substr(eq_pos + 1));
            queryParameters_[key] = value;
        } else if (!pair.empty()) {
             // Case where parameter has no value (e.g., ?flag)
            std::string key = urlDecode(pair);
            queryParameters_[key] = "";
        }
    }
    
    queryParsed_ = true;
}