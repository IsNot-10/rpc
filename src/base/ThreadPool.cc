#include "ThreadPool.h"
#include <optional>

ThreadPool::ThreadPool(size_t maxQueSize,std::string_view name)
:name_(name),running_(false),maxQueSize_(maxQueSize)
{}


ThreadPool::~ThreadPool()
{
    if(running_)  stop();
}



//启动线程池
void ThreadPool::start(size_t threadNum)
{
    threadVec_.reserve(threadNum);
    running_=true;
    for(size_t i=0;i<threadNum;i++)
    {
        char buf[32]={0};
        snprintf(buf,sizeof buf,"%s%ld",name_.c_str(),i+1);
        auto th=std::make_unique<Thread>(
            [this]()
            {
                threadFunc();
            },buf);
        threadVec_.push_back(std::move(th));
        threadVec_[i]->start();
    }
}



void ThreadPool::stop()
{
    {
        std::lock_guard<std::mutex> lock{mtx_};
        running_=false;
    }
    notEmpty_.notify_all();
    for(auto& th:threadVec_)  th->join();
}



//往任务队列中放入队列
void ThreadPool::addTask(Task cb)
{
    {
        std::unique_lock<std::mutex> lock{mtx_};
        while(que_.size()>=maxQueSize_)  notFull_.wait(lock);
        que_.push(std::move(cb));
    }
    notEmpty_.notify_all();
}




//所有计算线程都会绑定这个回调函数,就是往任务队列中取出任务并执行
//(实际上就是调用回调函数).当然如果队列为空就会阻塞住.
void ThreadPool::threadFunc()
{
    while(running_)
    {
        std::optional<Task> task;
        {
            std::unique_lock<std::mutex> lock{mtx_};
            while(que_.empty()&&running_)  notEmpty_.wait(lock);
            if(que_.size())  
            {
                task=que_.front();
                que_.pop();
            }
        } 
        notFull_.notify_all();
        if(task.has_value())  (*task)();
    }
}