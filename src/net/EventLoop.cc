#include "EventLoop.h"
#include "Channel.h"
#include "Epoller.h"
#include "TimerQueue.h"
#include "TimerId.h"
#include "Logging.h"
#include "sys/eventfd.h"
#include "unistd.h"

//防止一个线程创建多个EventLoop对象,它就只能绑定一个
thread_local EventLoop* t_loopInThisThread=nullptr;

//通过eventfd系统调用初始化wakeupfd_
static int createEventFd()
{
    int evfd=::eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(evfd<0)  LOG_FATAL<<"eventfd error:"<<errno;
    return evfd;
}



EventLoop::EventLoop()
:threadId_(CurrentThread::tid()),quit_(false),callPending_(false)
,wakeupfd_(createEventFd())
,wakeupChannel_(std::make_unique<Channel>(wakeupfd_,this))
,poller_(std::make_unique<Epoller>(this))
,timerQue_(std::make_unique<TimerQueue>(this))
{
    LOG_DEBUG<<"EventLoop created "<<this<<", the threadId is "<<threadId_;
    LOG_DEBUG<<"EventLoop created wakeupFd "<<wakeupChannel_->getFd();
    
    //同一个线程不允许绑定多个EventLoop对象
    if(t_loopInThisThread)
    {
        LOG_FATAL<<"Another EventLoop "<<t_loopInThisThread
            <<" exists in this thread "<<threadId_;
    }
    else  t_loopInThisThread=this;

    //给wakeupfd_设置读回调并让它可读.wakeupfd_本身也只关注读事件
    wakeupChannel_->setReadCallback(
        [this](TimeStamp receiveTime)
        {
            handleRead(receiveTime);
        });
    wakeupChannel_->enableReading();
}



//每个EventLoop对象都管理一个wakeupfd_的生命周期
//析构函数中会注销它的读事件,并且把它的Channel对象也从epoll监听集中移除掉
//最后别忘了关闭它,因为Channel对象不负责管理fd的生命周期
EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll();
    ::close(wakeupfd_);
    t_loopInThisThread=nullptr;
}





//代表事件循环的函数,它的逻辑其实也非常简单,主要以下两个步骤

//1.一直调用epoll_wait函数(通过调用Epoller对象的poll函数)去等待就绪可
//处理的事件.如果当前确实有Channel关注的事件到来了,就会让这些Channel
//对象去调用它们对应的回调函数

//2.这些Channel对象处理完了自己的回调函数,那么还需要调用pendingFunctors_
//中的回调函数(这些回调函数一般都是其他线程放进pendingFunctors_容器的)
void EventLoop::loop()
{
    LOG_INFO<<"EventLoop "<<this<<" start looping";
    while(!quit_)
    {
        activeChannels_.clear();
        pollReturnTime_=poller_->Poll(kPollTimeMs,&activeChannels_);
        for(Channel* channel:activeChannels_)  channel->handleEvent(pollReturnTime_);
        doPendingFunctor();
    }
}





//quit函数就是用来退出loop循环的
//1.有可能当前正在loop循环中,但是当前线程的定时器的回调函数可以是quit
//2.也有可能当前线程中TcpConnection对象对应的Channel的回调函数中调用quit

//上面两种情况都是在当前IO线程的正常流程中调用quit函数,都走到这了当然无需唤醒
//但是如果并非当前IO线程直接调用的quit函数,那就必须唤醒当前线程
void EventLoop::quit()
{
    quit_=true;
    if(!isInLoopThread())  wakeup();
}




//也是自动保证线程安全的函数.如果确实是当前线程调用的,那就直接调用
//否则会把这个任务加到pendingFunctors_中,等所有活跃Channel对象的回调函数
//全部调用完成再去执行这些任务(后者其实就是queueInLoop的逻辑)
void EventLoop::runInLoop(Functor cb)
{
    if(isInLoopThread())  cb();
    else  queueInLoop(std::move(cb));
}




//queueInLoop函数就是把任务放入pendingFunctors_而非直接调用
//必要情况会调用wakeup唤醒这个loop所在的线程

//必要情况分为两种
//1.并不是当前的IO线程调用的,那就必须唤醒当前的IO线程

//2.确实是当前的IO线程在调用,但是当前线程正在执行pendingFunctors_中的
//回调函数.这样的话下次从loop循环头开始又要阻塞在epoll_wait那里了.
//新加入的cb不能被即时调用,所以当然也是要唤醒的

//情况2就说明queueInLoop也是有可能在当前线程中调用的,不一定是跨线程调用的情况
void EventLoop::queueInLoop(Functor cb)
{
    {
        std::lock_guard<std::mutex> lock{mtx_};
        pendingFunctors_.push_back(std::move(cb));
    }
    if(!isInLoopThread()||callPending_)  wakeup();
}




//这里通过std::vector的swap方法缩小了临界区
void EventLoop::doPendingFunctor()
{
    std::vector<Functor> functorVec;
    callPending_=true;
    {
        std::lock_guard<std::mutex> lg{mtx_};
        functorVec.swap(pendingFunctors_);
    }
    for(const auto& functor:functorVec)  functor();
    callPending_=false;
}



//往wakeupfd_中写数据,这样它就可读了,loop循环也不会一直卡死在epoll_wait
void EventLoop::wakeup()
{
    uint64_t one=1;
    ssize_t writen=::write(wakeupfd_,&one,sizeof one);
    if(writen!=sizeof one)
    {
        LOG_ERROR<<"EventLoop::wakeup writes "<<
            writen<<" bytes instead of 8";
    }
}



//没啥好说的,就是wakeupChannel_的读回调
void EventLoop::handleRead(TimeStamp receiveTime)
{
    uint64_t one=1;
    ssize_t readn=::read(wakeupfd_,&one,sizeof one);
    if(readn!=sizeof one)
    {
        LOG_ERROR<<"EventLoop::handleRead() reads "<<
            readn<<" bytes instead of 8";
    }
}



void EventLoop::updateChannel(Channel* channel)
{
    poller_->updateChannel(channel);
}



TimerId EventLoop::runAt(TimeStamp timeStamp,Functor cb)
{
    return timerQue_->addTimer(std::move(cb),timeStamp,0.0);
}

TimerId EventLoop::runAfter(double waitTime,Functor cb)
{
    TimeStamp time{addTime(TimeStamp::now(),waitTime)};
    return runAt(time,std::move(cb));
}
    

TimerId EventLoop::runEvery(double interval,Functor cb)
{
    TimeStamp time{addTime(TimeStamp::now(),interval)};
    return timerQue_->addTimer(std::move(cb),time,interval);
}


void EventLoop::cancle(TimerId timerId)
{
    timerQue_->cancle(timerId);
}
