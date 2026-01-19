#pragma once

#include "NonCopy.h"
#include "sys/epoll.h"
#include <memory>
#include <functional>

class TimeStamp;
class EventLoop;
class Epoller;
class TcpConnection;


//Channel类本质就是封装了一个fd并绑定上它关注的事件(最核心就是读和写这两种)
//但是Channel对象并不会管理fd的生命周期

//最后为了清晰的梳理整个muduo架构,有必要给项目中的所有Channel分个类

//1.封装listenfd的Channel对象,全局只有主线程会有一个,在TcpServer中名为
//acceptorChannel_.它只会关注读事件(具体来说就是是否监听到新的连接)
//读回调就是调用accept函数接收新连接返回一个connfd,再把这个connfd封装成
//一个TcpConnection对象,再完成一系列连接建立的后续工作.它的生命周期由全局唯一
//的Acceptor对象直接管理.


//2.封装connfd的Channel对象,这是全muduo中最重要也是最复杂的Channel对象.如果设立
//IO子线程数量,那么每个IO子线程(subLoop)中都会有一个或多个.它们用来完成Tcp编程的
//三个半事件,因此不仅要关注读事件也要关注写事件.会注册读回调,写回调,关闭回调和
//错误处理回调(4种回调全部都要注册).它的生命周期由TcpConnection对象直接管理.


//3.封装wakeupfd的Channel对象,每个IO线程(包括mainLoop和所有subLoop)都有且仅有一
//个.它由系统调用eventfd生成,用来完成一个线程唤醒另一个线程的操作.具体就是往wakeupfd
//中写数据,这样wakeupfd就读就绪而避免loop循环卡死在epoll_wait了.很明显它也只关注
//读事件,也只需要注册读回调.它的生命周期由EventLoop对象直接管理.


//4.封装timerfd的Channel对象,每个IO线程(包括mainLoop和所有subLoop)都有且仅有一
//个红黑树定时器列表(被封装成TimerQueue对象)管理这个loop中所有的定时器,而它也仅有
//一个相对应的Channel对象.只会关心读事件和注册读回调.生命周期由TimerQueue对象管理.



//5.封装try_connfd的Channel对象,何为try_connfd?我这里暂且不详细解释,就是
//Connector对象内部的那个Channel对象,准确的说try_connfd表达的就是客户端fd和服务
//的连接成功以前的形态.(实际上我们当然知道客户端fd本身是不会在connect之后就完全
//变化的,但它确实前后被两种不一样的Channel对象封装着).这种Channel对象只会关心
//写事件不会关心读事件,会注册写回调和错误处理回调.而它的生命周期其实并非直接由
//Connector对象管理,只要客户端在::connect后真正成功(没有错误也没有自连接)建立
//一个tcp连接后,这个Channel对象就自然没有任何用处而被销毁了.

class Channel
:NonCopy
{
public:
    using EventCallback=std::function<void()>;
    using EventReadCallback=std::function<void(TimeStamp)>;

public:

    //Channel对象并不会管理fd的生命周期,fd不是在Channel对象的构造函数
    //中创建,也不是在Channel对象的析构函数中关闭
    Channel(int fd,EventLoop* loop);
    ~Channel()=default;
    
    //处理Channel感兴趣的事件
    void handleEvent(TimeStamp receiveTime);

    //这个函数的逻辑相对复杂,后续说明
    void tie(const std::shared_ptr<TcpConnection>& conn);

    int getFd()const
    {
        return fd_;
    }

    void setRevent(int revent)
    {
        revent_=revent;
    }

    int getRevent()const
    {
        return revent_;
    }

    int getEvent()const
    {
        return events_;
    }

    bool isNoneEvent()const
    {
        return events_==kNoneEvent;
    }

    bool isReading()const
    {
        return events_&kReadEvent;
    }

    bool isWriting()const
    {
        return events_&kWriteEvent;
    }

    //像这些enable函数的作用都是说明自己新关注了某个事件并改变events_
    //然后还要调用update函数保证这些关注的事件也被注册到epoll监听集中了
    void enableReading()
    {
        events_|=kReadEvent;
        update();
    }

    void disableReading()
    {
        events_&=~kReadEvent;
        update();
    }

    void enableWriting()
    {
        events_|=kWriteEvent;
        update();
    }

    void disableWriting()
    {
        events_&=~kWriteEvent;
        update();
    }

    //调用disableAll时,update函数必然能保证Channel对象也从epoll监听集合中
    //删除.所以我根本就没有提供remove函数(原muduo调用remove函数时候必须要保证
    //Channel对象此时是不关注任何事件的)
    void disableAll()
    {
        events_=kNoneEvent;
        update();
    }
    
    EventLoop* getLoop()const
    {
        return loop_; 
    }

    int getState()const
    {
        return state_;
    }

    void setState(int state)
    {
        state_=state;
    }

    //muduo中几乎所有类型的Channel对象都会关心读事件并会注册读回调
    //唯独try_connfd(客户端和服务端连接成功以前的fd)的Channel对象不会关心读事件
    void setReadCallback(EventReadCallback cb)
    {
        readCallback_=std::move(cb);
    }

    //封装connfd的Channel对象会关心写事件,并会额外注册下面3种回调函数
    //封装try_connfd的Channel对象只会关心写事件,会注册写回调和错误处理回调
    void setWriteCallback(EventCallback cb)
    {
        writeCallback_=std::move(cb);
    }

    void setCloseCallback(EventCallback cb)
    {
        closeCallback_=std::move(cb);
    }

    void setErrorCallback(EventCallback cb)
    {
        errorCallback_=std::move(cb);
    }

private:
    void handleEventWithGuard(TimeStamp receiveTime);
    void update();

private:

    //分别代表无事件,读事件和写事件
    //如果fd_需要关心读事件就让revent_和kReadEvent做或运算
    static constexpr int kNoneEvent=0;
    static constexpr int kReadEvent=EPOLLIN|EPOLLPRI;
    static constexpr int kWriteEvent=EPOLLOUT;

private:
    const int fd_;
    EventLoop* loop_;
    
    //用来描述fd关心事件的组合(其实有点像是位图思想)
    //插入epoll的红黑树监听集合之前注册的时候用
    int events_;

    //这个表示epoll_wait返回后,哪些关心的事件到来了
    //一般来说会是events_的子集(但实际上也不一定)
    int revent_;


    //这个表示fd_的状态,标记它是否在epoll的红黑树监听集合中
    //原muduo中使用三个值表示它的状态,我觉得用一个bool类型表示两种状态就够了
    bool state_;

    //只有封装connfd的Channel对象,tied才会为true,conn_才会起作用
    //其他类型的Channel对象的tied_永远为false,也根本用不到conn_
    bool tied_;
    std::weak_ptr<TcpConnection> conn_;

    //这就是fd有可能关注的4种事件
    //根据revents_判断,如果对应的事件到来那就调用对应的回调函数
    EventReadCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

