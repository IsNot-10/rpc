#include "LogStream.h"
#include <algorithm>
#include <cstdint>



//出自Matthew Wilson的文章<<Efficient Integer to String Conversions>>
//使用对称的digits数组搞定了负数转换的边界条件
static const char digits[]="9876543210123456789";
static const char* zero=digits+9;

static const char digitsHex[]="0123456789ABCDEF";


//将整数处理成字符串并添加到buffer_中
template <typename T>
void LogStream::formatInteger(T num)
{
    if(buffer_.avail()<kMaxNumericSize)  return;
    
    //标记是否为负数
    bool negative=(num<0);

    //开始写入的位置
    char* start=buffer_.current();
    char* cur=start;

    //这里用do while而不用while,不需要特判num==0的情况
    do
    {
        int remainder=static_cast<int>(num%10);
        *(cur++)=zero[remainder];
        num/=10;
    }while(num!=0);
    if(negative)  *(cur++)='-';
    *cur='\0';
    std::reverse(start,cur);

    //该写的都写完了,单纯移动指针即可
    buffer_.add(cur-start);
}



size_t converHex(char buf[],uintptr_t val)
{
    char* cur=buf;
    do
    {
        int lsd=static_cast<int>(val%16);
        val/=16;
        *(cur++)=digitsHex[lsd];
    } while(val!=0);
    *cur='\0';
    std::reverse(buf,cur);
    return cur-buf;
}




LogStream& LogStream::operator<<(bool v)
{
    buffer_.append((v?"1":"0"),1);
    return *this;
}

LogStream& LogStream::operator<<(short v)
{
    *this<<static_cast<int>(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned short v)
{
    *this<<static_cast<unsigned int>(v);
    return *this;
}

LogStream& LogStream::operator<<(int v)
{
    formatInteger(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned int v)
{
    formatInteger(v);
    return *this;
}

LogStream& LogStream::operator<<(long v)
{
    formatInteger(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned long v)
{
    formatInteger(v);
    return *this;
}

LogStream& LogStream::operator<<(long long v)
{
    formatInteger(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned long long v)
{
    formatInteger(v);
    return *this;
}


LogStream& LogStream::operator<<(float v)
{
    *this<<static_cast<double>(v);
    return *this;
}


LogStream& LogStream::operator<<(double v)
{
    if(buffer_.avail()>=kMaxNumericSize)
    {
        int len=snprintf(buffer_.current(),kMaxNumericSize,"%.12g",v); 
        buffer_.add(len);
    }
    return *this;
}


LogStream& LogStream::operator<<(char c)
{
    buffer_.append(&c,1);
    return *this;
}



//输出地址值
LogStream& LogStream::operator<<(const void* ptr)
{
    uintptr_t v=reinterpret_cast<uintptr_t>(ptr);
    if(buffer_.avail()>=kMaxNumericSize)
    {
        char* buf=buffer_.current();
        buf[0]='0';
        buf[1]='x';
        size_t len=converHex(buf+2,v);
        buffer_.add(len+2);
    }
    return *this;   
}


LogStream& LogStream::operator<<(const char* str)
{
    if(str)  buffer_.append(str,::strlen(str));
    else  buffer_.append("(null)",6);
    return *this;
}


LogStream& LogStream::operator<<(const unsigned char* str)
{
    *this<<reinterpret_cast<const char*>(str);
    return *this;
}


LogStream& LogStream::operator<<(const std::string& str)
{
    buffer_.append(str.c_str(),str.size());
    return *this;
}


LogStream& LogStream::operator<<(std::string_view str)
{ 
    buffer_.append(str.data(),str.size());
    return *this;
}


LogStream& LogStream::operator<<(const Buffer& buf)
{
    *this<<buf.toStringPiece();
    return *this;
}