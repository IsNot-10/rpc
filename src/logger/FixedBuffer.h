#pragma once

#include "NonCopy.h"
#include <string>
#include <string.h>


//4KB,日志打印所需的缓冲区大小,也就是LogStream对象底层的那个
inline constexpr int kSmallBuffer=4000;


//4MB,异步日志中所有前端线程所需的缓冲区大小,也就是AsyncLogging底层的那个
inline constexpr int kLargeBuffer=4000*1000; 


template<int SIZE>
class FixedBuffer
:NonCopy
{
public:
    FixedBuffer()
    :cur_(data_)
    {}


    //把数据写到data_缓冲区,事实上就这个函数最重要
    void append(const char* buf,size_t len)
    {
        if(static_cast<size_t>(avail())>len)
        {
            ::memcpy(cur_,buf,len);
            cur_+=len;
        }
    }

    //单纯的移动指针
    void add(size_t len)
    {
        cur_+=len;
    }

    //开始位置地址完全固定
    const char* data()const
    {
        return data_;
    }

    //当前已写数据
    size_t length()const
    {
        return cur_-data_;
    }

    //当前写指针的位置
    char* current()
    {
        return cur_;
    }
    
    //当前还能写多少数据
    size_t avail()const 
    {
        return end()-cur_;
    }
    
    //重置写指针的位置
    void reset()
    {
        cur_=data_;
    }

    //清空data_缓冲区
    void bzero()
    {
        ::memset(data_,0,sizeof data_);
    }

    //把已经写好的数据转换成一个字符串返回
    std::string toString()const
    {
        return std::string{data_,length()};
    }

    std::string_view toStringPiece()const
    {
        return std::string_view{data_,length()};
    }

private:
    //这是固定的结束位置的地址
    const char* end()const
    {
        return data_+sizeof(data_);
    }

private:
    char data_[SIZE];
    char* cur_;    //写指针,在此之前就是写好的数据
};

