#pragma once

#include "Thread.h"
#include "FixedBuffer.h"
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>


//异步日志系统的逻辑就是在这个类中实现的, 采用的是双缓冲策略
//muduo中的日志系统本质就是个多生产者单消费者的问题,所有日志打印的线程全都是
//前端线程,而只有一个后端线程会完成将日志写入文件的操作
class AsyncLogging
:NonCopy
{
public:
    AsyncLogging(std::string_view basename,off_t rollSize,int flushInterval=3);
    ~AsyncLogging();

    //实际上是设置给日志打印后的回调函数,也就是Logging.cc文件下的g_output全局回调函数
    void append(const char* logling,int len);
    
    //启动后端线程
    void start();
    
    //让后端线程退出循环并汇入
    void stop();

private:
    //后端线程绑定的任务函数
    void threadFunc();
    
private:
    using Buffer=FixedBuffer<kLargeBuffer>;
    using BufferPtr=std::unique_ptr<Buffer>;
    
    //这些都是LogFile对象的属性,毕竟后端线程将日志写入磁盘还是要靠它的
    const std::string basename_;
    const off_t rollSize_; 

    //后端线程每隔一段时间自己会醒来,不可能一直阻塞
    const int flushInterval_;    
    
    //后端线程是否正在运行
    std::atomic_bool running_;

    //后端线程(把日志写入磁盘文件),全局仅有一个
    Thread thread_;

    
    //所有前端线程共用两个缓冲区和一个缓冲队列.因此它们需要被互斥锁保护
    //两个缓冲区大小都是4MB
    BufferPtr currentBuffer_;    //当前缓冲区
    BufferPtr nextBuffer_;       //备用缓冲区
    std::vector<BufferPtr> bufferVec_;    //所有前端线程的缓冲区队列

    std::mutex mtx_;
    std::condition_variable cond_;
};

