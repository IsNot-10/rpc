#include "Redis.h"
#include "Logging.h"
#include <thread>

Redis::Redis()
:publishConText_(nullptr),subscribeContext_(nullptr)
{}

Redis::~Redis()
{
    if(publishConText_)  ::redisFree(publishConText_);
    if(subscribeContext_)  ::redisFree(subscribeContext_);
}



//真正让redis客户端连接redis服务端
bool Redis::connect()
{
    publishConText_=::redisConnect("127.0.0.1",6379);
    if(!publishConText_)
    {
        LOG_ERROR<<"connect redis failed!";
        return false;
    }
    subscribeContext_=::redisConnect("127.0.0.1",6379);
    if(!subscribeContext_)
    {
        LOG_ERROR<<"connect redis failed!";
        return false;   
    }
    
    //开启一个后台线程,专门负责观察服务进程订阅的Channel是否有消息
    //如果有的话就会通知业务层并调用回调函数
    std::thread th{
        [this]()
        {
            observerChannelMessage();
        }};
    th.detach();
    LOG_INFO<<"connect redis-server success!";
    return true;
}



bool Redis::publish(int channel,const std::string& msg)
{
    redisReply* reply=(redisReply*)::redisCommand(
        publishConText_,"publish %d %s",channel,msg.c_str());
    if(!reply)
    {
        LOG_ERROR<<"publish command failed!";
        return false;
    }
    ::freeReplyObject(reply);
    return true;
}



bool Redis::subscribe(int channel)
{
    if(::redisAppendCommand(
        subscribeContext_,"subscribe %d",channel)==REDIS_ERR)
    {
        LOG_ERROR<<"subscibe command failed";
        return false;
    }
    int done=0;
    while(done==0)
    {
        if(::redisBufferWrite(subscribeContext_,&done)==REDIS_ERR)
        {
            LOG_ERROR<<"subscribe command failed";
            return false;
        }
    }
    return true;
}



bool Redis::unsubscribe(int channel)
{
    if(::redisAppendCommand(
        subscribeContext_,"unsubscribe %d",channel)==REDIS_ERR)
    {
        LOG_ERROR<<"unsubscibe command failed";
        return false;
    }
    int done=0;
    while(done==0)
    {
        if(::redisBufferWrite(subscribeContext_,&done)==REDIS_ERR)
        {
            LOG_ERROR<<"unsubscribe command failed";
            return false;
        }
    }
    return true;
}


//后台线程绑定的就是这个函数
void Redis::observerChannelMessage()
{
    redisReply* reply=nullptr;
    while(::redisGetReply(subscribeContext_,(void**)&reply)==REDIS_OK)
    {
        //reply里面是返回的数据有三个
        //0.message  1.通道号   2.消息
        if(reply&&reply->element[2]&&reply->element[2]->str)
        {
            notifyMessageCb_(atoi(reply->element[1]->str),
                reply->element[2]->str);
        }
        ::freeReplyObject(reply);
    } 
}
