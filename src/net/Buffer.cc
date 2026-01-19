#include "Buffer.h"
#include <algorithm>
#include <unistd.h>
#include <sys/uio.h>

Buffer::Buffer(size_t initalSize)
:buffer_(initalSize+kCheapPrepend)
,readerIndex_(kCheapPrepend),writerIndex_(kCheapPrepend)
{}

void Buffer::retrieve(size_t len)
{
    if(len<readableBytes())  readerIndex_+=len;
    else  retrieveAll();
}

void Buffer::retrieveUntil(const char* end)
{
    return retrieve(end-peek());
}

void Buffer::retrieveAll()
{
    writerIndex_=readerIndex_=kCheapPrepend;
}



//把缓冲区中可读的部分(都是字符串)全部取出来返回,但是不会移动读指针
std::string Buffer::getBufferAllAsString()const
{
    std::string res{peek(),readableBytes()};
    return res;
}


//把缓冲区中可读的数据(字符串)取出前len长度的部分,并相应的移动读指针
std::string Buffer::retrieveAsString(size_t len)
{
    std::string res{peek(),len};
    retrieve(len);
    return res;
}



//用户最常用的函数,不仅把所有可读数据取出来还会移动读指针
std::string Buffer::retrieveAllAsString()
{
    return retrieveAsString(readableBytes());
}



//用来保证当前缓冲区能够写入len长度的字符串数据
void Buffer::ensureWritableBytes(size_t len)
{
    if(len>writableBytes())
    {
        //读指针也是会移动的,而它前面的空间也尽量利用起来,可以尽可能避免扩容
        if(len<=writableBytes()+readerIndex_-kCheapPrepend)  
        {
            size_t readAble=readableBytes();
            std::copy(peek(),peek()+readAble,begin()+kCheapPrepend);
            readerIndex_=kCheapPrepend;
            writerIndex_=kCheapPrepend+readAble;
        }

        //如果读指针之前的空间也无法挽救局面,那就只能扩容了
        else  buffer_.resize(buffer_.size()+len);
    }
}


void Buffer::append(const void* data,size_t len)
{
    append(static_cast<const char*>(data),len);
}


void Buffer::append(std::string_view str)
{
    append(str.data(),str.size());
}



//把字符串全部写到缓冲区中,期间可能涉及扩容
void Buffer::append(const char* data,size_t len)
{
    ensureWritableBytes(len);
    std::copy(data,data+len,beginWrite());
    writerIndex_+=len;
}


const char* Buffer::findCRLF()const
{
    const char* crlf=std::search(peek(),beginWrite(),kCRLF,kCRLF+2);
    return crlf==beginWrite()?nullptr:crlf;
}




//将socket接收缓冲区的数据全部写到intputBuffer_中
//这里就用到了::readv系统调用(分散读)技巧,只需一次系统调用
//那么为什么extrabuf栈内存是64kB?

//64kB缓冲足够容纳千兆网在500us内全速收到的数据,在一定意义上可视为延迟带宽积
ssize_t Buffer::readFd(int fd,int* saveErrno)
{
    char extrabuf[65536]={0};
    const size_t writeAble=writableBytes();
    struct iovec vec[2];
    vec[0].iov_base=beginWrite();
    vec[0].iov_len=writeAble;
    vec[1].iov_base=&extrabuf;
    vec[1].iov_len=sizeof extrabuf;
    const ssize_t n=::readv(fd,vec,2);
    if(n<0)  *saveErrno=errno;
    
    //intputBuffer_本身就能容纳数据,不需要extrabuf了
    else if(n<=writeAble)  writerIndex_+=n;
    
    //intputBuffer_本身不能容纳这么多数据,剩下的数据在extrabuf中
    //当然也会把extrabuf中的数据加到intputBuffer_中,但大概率要扩容了
    else
    {
        writerIndex_=buffer_.size();
        append(extrabuf,n-writeAble);
    }  
    return n;
}




//将outBuffer_中的数据全部读出来,写到socket发送缓冲区上
//实际上这个函数只有在发送数据时,没有把全部数据放到socket发送缓冲区的情况下才会被调用
//用来收留剩余的数据(当然它们后面也会被继续放到socket发送缓冲区)
ssize_t Buffer::writeFd(int sockfd,int* saveErrno)
{
    ssize_t n=::write(sockfd,peek(),readableBytes());
    if(n<0)  *saveErrno=errno;
    return n;
}

