#pragma once

#include "NonCopy.h"
#include <vector>
#include <string>




//非阻塞网络编程,必须要有缓冲区.具体来说,muduo网络库中每个TcpConnection
//(tcp连接)都有一个发送缓冲区和接收缓冲区.

//1.发送缓冲区的作用
//假如某一条tcp连接服务端要发送100B的数据,可是socket对象的发送缓冲区只被
//拷贝到了80B数据(这由操作系统决定),剩下的20B数据怎么办呢?总不能等待80B
//数据发送完成吧.事实上tcp层是不可能知道对方有没有接收到80B数据的,这个
//要应用层协议的确认.

//而上面所说的现象必须对用户/应用程序是透明的,20B数据由网络库默默的处理,
//也就是将它全部暂存到网络库自定义的缓冲区中,也就是outputBuffer_.

//如果某个tcp连接的outputBuffer_中还有数据就说明这一次发送操作是并没有
//全部发送完的,那么就可以给Channel对象(封装connfd,每个tcp连接都有一个)
//注册写事件,Channel对象会在loop循环中写就绪,直到把outputBuffer_中的
//数据全部写到socket发送缓冲区,并注销写事件避免造成busy_loop.



//2.接收缓冲区的作用
//如果服务端的某一条tcp连接在收到客户端发来的数据,就必须把这些数据从socket
//对象的接收缓冲区读出来保存到用户缓冲区intputBuffer_,这是很显而易见的目的.
//但是实现起来还是需要关注两个问题.如果发送过来的数据很大怎么办,如果缓冲区
//不够大的话可能就要读很多次(导致多次系统调用)降低性能.那难道我准备一个超级
//大的缓冲区吗?完全有可能因为错误估计导致缓冲区过大从而白白浪费内存.

//为了减小内存浪费的同时减小系统调用次数,muduo网络库中采用了::readv系统调用
//(分散读),可以先分配一块非常大的栈空间(64KB),然后一次readv就可以根据实际的
//数据量写到intputBuffer_,而intputBuffer_接收不了太大的数据,剩下的数据会被
//放到65536B的栈内存,完事以后把栈内存中实际被写入的那部分数据追加到intputBufer_中

//这里也能看出为什么muduo中的epoll采用的是LT模式而不是ET模式,它是把socket接
//收缓冲区上面的数据贪心的一次性全部写入intputBuffer_了,仅仅需要一次::readv
//系统调用.符合直觉并且更简单的LT模式就已经能胜任,也用不着ET模式,更何况原版
//muduo还要考虑poll,不仅仅是顾及epoll一种IO多路复用.



//muduo中的Buffer的结构如下
//底层是std::vector<char>,会动态的扩容.但当然也有缺点,没有提供任何让Buffer
//缩小的接口.原版的muduo中虽然有shrink方法,但感觉意义不大.

//尽管Buffer有可以优化的地方,但实际上没有必要,因为网络库的性能瓶颈不可能在这里

/// +-------------------+------------------+------------------+
/// | prependable bytes |  readable bytes  |  writable bytes  |
/// |                   |     (CONTENT)    |                  |
/// +-------------------+------------------+------------------+
/// |                   |                  |                  |
/// 0      <=      readerIndex   <=   writerIndex    <=     size


class Buffer
{
public:
    explicit Buffer(size_t initalSize=kInitialSize);
    void retrieve(size_t len);
    void retrieveUntil(const char* end);
    void retrieveAll();
    std::string getBufferAllAsString()const;
    std::string retrieveAllAsString();
    std::string retrieveAsString(size_t len);
    void ensureWritableBytes(size_t len);
    void append(const char* data,size_t len);
    void append(const void* data,size_t len);
    void append(std::string_view str);
    ssize_t readFd(int sockfd,int* saveErrno);
    ssize_t writeFd(int sockfd,int* saveErrno);
    const char* findCRLF()const;


    //缓冲区还有多少可读的空间
    size_t readableBytes()const
    {
        return writerIndex_-readerIndex_;
    }
    

    //缓冲区还有多少可写的空间
    size_t writableBytes()const
    {
        return buffer_.size()-writerIndex_;
    }


    size_t prependableBytes()const
    {
        return readerIndex_;
    }
    
    
    //返回读指针的开始地址
    const char* peek()const
    {
        return begin()+readerIndex_;
    }

    //返回写指针的开始地址
    char* beginWrite()
    {
        return begin()+writerIndex_;
    }

    const char* beginWrite()const
    {
        return begin()+writerIndex_;
    }
    
private:
    //整个缓冲区的起始地址
    char* begin()
    {
        return &(*buffer_.begin());
    }

    const char* begin()const
    {
        return &(*buffer_.begin());
    }

private:
    static const size_t kCheapPrepend=8;
    static const size_t kInitialSize=1024;

    //解析http请求报文的时候用
    inline static const char kCRLF[]="\r\n";

private:
    //std::vector有利于让缓冲区扩容
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

