#include "EventLoopThread.h"
#include "EventLoop.h"


//给Thread对象传入ThreadFunc这个回调函数以及线程名字
EventLoopThread::EventLoopThread(ThreadInitCallback cb,std::string_view name)
:loop_(nullptr)
,thread_([this]()
{
    ThreadFunc();   
},name)
,callback_(std::move(cb))
{}


//实际上析构函数往往走不到"loop_不空"这个分支
EventLoopThread::~EventLoopThread()
{
    if(loop_)  
    {
        loop_->quit();
        thread_.join();
    }
}



//让Thread线程开始执行,然后返回它对应的loop指针
//而这个loop指针是在线程回调函数中设置的,必须要直到loop指针被设置好才能
//返回.因此需要利用条件变量等待
EventLoop* EventLoopThread::startLoop()
{
    thread_.start();
    std::unique_lock<std::mutex> lock{mtx_};
    while(!loop_)  cond_.wait(lock);
    return loop_;
}



//这个就是绑定给Thread对象的线程内回调函数,具体细节如下
//在栈上创建一个EventLoop对象,如果存在循环前回调就调用一下这个回调
//再把当前EventLoopThread的数据成员loop_指针设成这个EventLoop的指针
//然后才让这个EventLoop对象调用loop方法,也就是进入事件循环

//可以说one loop per thread的实现基本就是靠这个函数
void EventLoopThread::ThreadFunc()
{
    EventLoop loop;
    if(callback_)  callback_(&loop);
    {
        std::unique_lock<std::mutex> lock{mtx_};
        loop_=&loop;
    }
    cond_.notify_all();
    loop.loop();
    std::lock_guard<std::mutex> lock{mtx_};
    loop_=nullptr;
}