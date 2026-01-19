#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <functional>

class Redis
{
public:
    using Functor=std::function<void(int,std::string)>;

    Redis();
    ~Redis();
    bool connect();
    bool publish(int channel,const std::string& msg);
    bool subscribe(int channel);
    bool unsubscribe(int channel);
    
    //设置回调函数notifyMessageCb_
    void initNotifyCb(Functor cb)
    {
        notifyMessageCb_=std::move(cb);
    }

private:
    //业务层调用subscribe函数并不会像redis的subscribe命令一样真的阻塞住
    //而是会开启一个线程一直检查服务进程订阅的Channel上是否有消息
    void observerChannelMessage();

private:
    //负责publish的上下文
    redisContext* publishConText_;
    
    //负责subscribe的上下文
    redisContext* subscribeContext_;

    //服务进程(订阅方)关心的Channel上有消息会被通知,并调用这个回调函数
    Functor notifyMessageCb_;
};

