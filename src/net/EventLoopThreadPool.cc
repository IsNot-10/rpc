#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* loop,std::string_view name)
:loop_(loop),name_(name),threadNum_(0),next_(0)
{}


EventLoopThreadPool::~EventLoopThreadPool()
{}



//启动线程池的函数,它会在TcpServer的start函数中调用
//其实就是一个个地启动IO线程并保存起来,并把它们所对应的Loop也保存起来

//如果不打算启动subLoop(子IO线程)
//那么所有的IO操作都需要主线程去完成,那就调用之前设置的cb回调函数
void EventLoopThreadPool::start(ThreadInitCallback cb)
{
    for(int i=0;i<threadNum_;i++)
    {
        char buf[128]={0};
        snprintf(buf,sizeof buf,"%s%d",name_.c_str(),i);
        pool_.push_back(std::make_unique<EventLoopThread>(cb,buf));
        loopList_.push_back(pool_[i]->startLoop());
    }
    if(threadNum_==0&&cb)  cb(loop_);
}




//获取下一个loop对象,当每次生成一个TcpConnection对象就需要获取一个loop
//去负责它的各种IO任务(建立和断开连接以及收发操作)

//muduo中给loop分配tcp连接就是采用最简单的轮询方法
EventLoop* EventLoopThreadPool::getNextLoop()
{
    EventLoop* loop=loop_;
    if(loopList_.size())  
    {
        loop=loopList_[next_];
        if(++next_==loopList_.size())  next_=0;
    }
    return loop;
}



//获取loop列表,没啥好说的
std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    if(loopList_.empty())  return std::vector<EventLoop*>{loop_};
    return loopList_;
}