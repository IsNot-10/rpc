#pragma once

#include "FixedBuffer.h"


//这个类有点像ostream类,重载了<<运算符并且因为返回引用类型从而能实现链式编程的目的
//但是它的性能远远高于std::cout,因为不涉及乱七八糟的继承和虚函数,没有动态内存分配

//它可以称为memory buffer output stream(内存缓冲区输出流)
class LogStream
:NonCopy
{
public:
    using Buffer=FixedBuffer<kSmallBuffer>;

public:
    LogStream()=default;

    //这些重载了<<运算符的函数都是先把数据暂存到buffer_上
    //至于啥时候才能持久化到文件,那要等到封装LogStream对象的Logger对象调用析构函数

    //本质来说都是往自己缓冲区中加数据
    LogStream& operator<<(bool);
    LogStream& operator<<(short);
    LogStream& operator<<(unsigned short);
    LogStream& operator<<(int);
    LogStream& operator<<(unsigned int);
    LogStream& operator<<(long);
    LogStream& operator<<(unsigned long);
    LogStream& operator<<(long long);
    LogStream& operator<<(unsigned long long);
    LogStream& operator<<(float);
    LogStream& operator<<(double);
    LogStream& operator<<(char);
    LogStream& operator<<(const void* data);
    LogStream& operator<<(const char* str);
    LogStream& operator<<(const unsigned char* str);
    LogStream& operator<<(const std::string& str);
    LogStream& operator<<(std::string_view str);
    LogStream& operator<<(const Buffer& buf);

    //将字符串写入buffer_,其实都是提供给上面重载<<运算符的函数用的
    void append(const char* data,int len)
    {
        buffer_.append(data,len);
    }

    //重置buffer_的写指针
    void resetBuffer()
    {
        buffer_.reset();
    }

    const Buffer& getBuffer()const
    {
        return buffer_;
    }

private:
   
   //这个成员函数模板,用来把整形数字转化为字符串再写入缓冲区
   //不建议用std::to_string函数
   template <typename T>
   void formatInteger(T num);

private:
    //比较保守的安全上限
    static constexpr int kMaxNumericSize=48;

    //所有数据都写到这个FixBuffer对象(缓冲区)中,固定大小为4KB
    Buffer buffer_;
};

