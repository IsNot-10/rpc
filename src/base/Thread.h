#pragma once

#include "NonCopy.h"
#include <string>
#include <functional>
#include <thread>
#include <memory>
#include <atomic>


//底层封装的是std::thread对象
//当然也要包含了它的名字,线程id,状态以及将要绑定的回调函数

//muduo网络库中总共3种线程
//1.计算线程,也有专门的线程池
//2.IO线程,也有专门的线程池.是最重要的线程,Reactor模型全靠IO线程池
//3.异步日志模块的写日志文件线程,实际就是后台线程,全局只有一个.
class Thread
:NonCopy
{
public:
    using ThreadFunc=std::function<void()>;

public:
    explicit Thread(ThreadFunc threadFunc,std::string_view name="");
    ~Thread();
    void start();
    void join();

    bool started()const
    {
        return started_;
    }

    const std::string& getName()const
    {
        return name_;
    }

    const pid_t getPid()const
    {
        return tid_;
    }

    static int getThreadNum()
    {
        return threadNum;
    }

private:
    static inline std::atomic_int threadNum{0};
    
private:
    bool started_;
    bool joined_;
    ThreadFunc threadFunc_;
    pid_t tid_;
    std::string name_;
    std::unique_ptr<std::thread> thread_;
};

