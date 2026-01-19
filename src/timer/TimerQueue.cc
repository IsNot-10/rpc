#include "TimerQueue.h"
#include "Timer.h"
#include "TimerId.h"
#include "EventLoop.h"
#include "Logging.h"
#include <string.h>
#include "sys/timerfd.h"
#include "unistd.h"

//初始化timerfd
static int createTimerfd()
{
    int timerfd=::timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(timerfd<0)  LOG_ERROR<<"Failed in timerfd_create";
    return timerfd;
}


//读timerfd数据
static void ReadTimerfd(int timerfd)
{
    uint64_t one;
    ssize_t readn=::read(timerfd,&one,sizeof one);
    if(readn!=sizeof one)  
    {
        LOG_ERROR<<"TimerQueue::handleRead() reads "<<
            readn<<" bytes instead of 8";
    }
}



//构造函数一如既往的就是给Channel对象设置读回调并让它关注读事件(注册到epoll监听集合)
TimerQueue::TimerQueue(EventLoop* loop)
:loop_(loop),timerfd_(createTimerfd())
,timerChannel_(timerfd_,loop)
{
    timerChannel_.setReadCallback(
        [this](TimeStamp receiveTime)
        {
            handleRead();
        });
    timerChannel_.enableReading();
}
    


TimerQueue::~TimerQueue()
{
    timerChannel_.disableAll();
    ::close(timerfd_);
    for(auto& [timerStamp,timer]:timerList_)  delete timer;
}





//作为少有的public函数,被EventLoop对象在外面以runAt,runAfter和runEvery的形式
//调用,而这三个函数又是暴露给用户的.因此必须考虑跨线程

//最后返回的TimerId是暴露给用户的,用户想删除这个定时器也是利用这个TimerId
TimerId TimerQueue::addTimer(TimerCallback cb,TimeStamp when,double interval)
{
    Timer* timer=new Timer{std::move(cb),when,interval};
    loop_->runInLoop(
        [this,timer]()
        {
            addTimerInLoop(timer);
        });
    return TimerId{timer,timer->getSequence()};
}




//先插入定时器,整体红黑树定时器列表的超时时间需要更改,那么就要重置timerfd
void TimerQueue::addTimerInLoop(Timer* timer)
{
    if(insert(timer))  resetTimerfd(timer->getExpiration());
}




//整体红黑树定时器列表的读回调函数
//实际上也是在loop循环中被调用的,不要因为没写在EventLoop类里就犯傻
void TimerQueue::handleRead()
{
    TimeStamp now{TimeStamp::now()};
    ReadTimerfd(timerfd_);

    //把超时的定时器全部取出来
    const auto expired=getExpired(now);
    callingExpired_=true;

    //清空一下,如果下面的run回调中存在cacnle自己或其他定时器的情况就会进行填充
    cancleTimerList_.clear();     

    //依次让这些超时定时器执行超时回调函数
    //注意,这些定时器的回调函数很有可能是cancle自己或者其他定时器的!
    //这就不得不详细分析run方法中定时器A调用cancle(定时器B)的情况

    //1.A!=B.B是不存在于expired中没有超时的定时器,它还在红黑树定时器列表(timerList_)
    //中.这种情况最简单,直接安全delete掉B即可
    
    //2.A!=B.B既不在红黑树定时器列表timerList_也不在expired中,说明可能老早就被
    //delete了.这种情况会简单的把B(实际上无效了)放入cancleTimerList_.后面也不会
    //再去对B做delete操作,更不可能把它放回红黑树定时器列表.
    
    //3.A!=B.B确实在expired中.这种情况是很遗憾的,尽管逻辑上B确实不应该再执行回调
    //函数,但实际上没有任何办法判断A在B的前面还是后面.和情况2一样,也是老老实实的把
    //B放入cancleTimerList_.但不同于情况2的是B后面会被delete.
    
    //4.A==B,也就是说定时器A正在调用超时回调函数把自己cancle掉.这其实是情况3特例.
    //同样是暂时放入cancleTimerList_,而且会在后面的reset函数中被delete掉一次.
    //不过这种情况倒不会 像3那样明明是要cancle的定时器调用了超时回调函数.
    for(auto& [timeStamp,timer]:expired)  timer->run();
    callingExpired_=false;

    
    //再遍历一次,如果是一次性定时器就直接删除.但如果是会每隔一段时间就会调用
    //回调函数的定时器,就需要更新超时时间并重新插入红黑树定时器列表

    //当然,cancleTimerList_中的定时器一定不能放回去!

    reset(expired,now);
}





//这个函数就是遍历过这些已经处理完超时回调的定时器.如果是一次性定时器,直接永久删除.
//但如果是每隔一段时间处理超时回调的定时器就要重新插入红黑树定时器列表timerList_
//当然,cancleTimerList_中的定时器一定不能放回去!
void TimerQueue::reset(const std::vector<Entry>& expired,TimeStamp now)
{
    for(auto& [timeStamp,timer]:expired)
    {
        ActiveTimer activerTimer{timer,timer->getSequence()};
        if(timer->isRepeat()
            &&cancleTimerList_.find(activerTimer)==cancleTimerList_.end())
        {
            //这里不可能每插入一个定时器发现超时时间早于原第一个定时器就重置
            //timerfd,这样效率太低了.整个for循环遍历一次最后统一重置一次

            timer->restart(now);
            insert(timer);
        }
        else  delete timer;
    }

    //只要红黑树定时器列表不为空就重置一下定时器
    TimeStamp nextExpire;
    if(timerList_.size())  
    {
        nextExpire=timerList_.begin()->second->getExpiration();
    }
    if(nextExpire.valid())  resetTimerfd(nextExpire);
}




//这个函数就是插入定时器timer的同时判断整体红黑树定时器列表的超时时间是否需要更改
//如果原本红黑树为空或者原来的第一个定时器超时时间比timer超时时间晚
//那么需要更新整体红黑树定时器列表的超时时间
bool TimerQueue::insert(Timer* timer)
{
    bool earliestChanged=false;
    TimeStamp when=timer->getExpiration();
    const auto it=timerList_.begin();
    if(it==timerList_.end()||when<it->first)  earliestChanged=true;

    //这两个数据成员始终要一起维护
    timerList_.emplace(when,timer);
    activeTimerList_.emplace(timer,timer->getSequence());
    return earliestChanged;
}



//以now为基准,返回红黑树定时器列表中所有超时时间早于now的定时器
//当然在此之前别忘了把它们从红黑树定时器列表中删除
std::vector<TimerQueue::Entry> TimerQueue::getExpired(TimeStamp now)
{
    Entry sentry{now,reinterpret_cast<Timer*>(UINTPTR_MAX)};
    const auto end=timerList_.lower_bound(sentry);
    std::vector<Entry> expired{timerList_.begin(),end}; 
    timerList_.erase(timerList_.begin(),end);

    //这个遍历是为了维护activeTimerList_和timerList_保持一致
    for(const auto& [when,timer]:expired)
    {
        ActiveTimer activeTimer{timer,timer->getSequence()};
        activeTimerList_.erase(activeTimer);
    }
    return expired;
}






//当整体的红黑树定时器列表的超时时间发生变化(实际上就是新加入定时器的超时时间
//是否早于原第一个定时器的超时时间),就会调用这个函数重置timerfd
void TimerQueue::resetTimerfd(TimeStamp expiration)
{
    struct itimerspec newValue;
    struct itimerspec oldValue;
    memset(&newValue,'\0',sizeof newValue);
    memset(&oldValue,'\0',sizeof oldValue);
    int64_t microSecondDif=
        expiration.microSecondsSinceEpoch()-TimeStamp::now().microSecondsSinceEpoch();
    if(microSecondDif<100)  microSecondDif=100;
    struct timespec ts;
    ts.tv_sec=static_cast<time_t>(
        microSecondDif/TimeStamp::kMicroSecondsPerSecond);
    ts.tv_nsec=static_cast<long>(
        (microSecondDif%TimeStamp::kMicroSecondsPerSecond)*1000);
    newValue.it_value=ts;

    //这样,到下次某个时刻Channel对象的读回调就会被触发
    //可以理解成这个操作令某个时刻后timerfd可读
    if(::timerfd_settime(timerfd_,0,&newValue,&oldValue))
    {
        LOG_ERROR<<"timerfd_settime faield()";
    }
}




//和addTimer一样是对外暴露的函数,也要考虑线程安全问题
//一定要注意,这个函数可以被直接调用,但也可以被定时器以超时回调函数的形式调用!
void TimerQueue::cancle(TimerId timerId)
{
    loop_->runInLoop(
        [this,timerId]()
        {
            cancleInLoop(timerId);
        });
}




//逻辑就是删除timerId对应的这个定时器,不管它有没有超时
//但是很遗憾,它没办法保证这个待删除的定时器一定不去执行最后的回调函数
void TimerQueue::cancleInLoop(TimerId timerId)
{
    ActiveTimer activeTimer{timerId.timer_,timerId.sequence_};
    const auto it=activeTimerList_.find(activeTimer);
    
    //确实在整体的红黑树定时器列表中找到了,说明它没有超时
    //但也有可能正在被其他定时器(不可能是自己)删除(这时候直接delete删除就行了)
    if(it!=activeTimerList_.end())
    {
        const auto [timer,sequence]=activeTimer;
        
        //这两个数据结构必须始终维护Timer数量和内容都完全一样(只是顺序不一样)
        activeTimerList_.erase(it);
        timerList_.erase(Entry{timer->getExpiration(),timer});
        delete timer;    //直接安全delete
    }

    //说明当前定时器正在被自己或者其他的定时器删除,这个是肯定的
    //但是当前定时器自己要么处于过期的定时器expired容器中,要么早就不存在(被删除了)

    //处理所有超时定时器回调那里是个循环,我总不能真的直接在循环的时候直接delete吧
    //万一前面的定时器把后面的定时器删掉了,后面会出现空悬指针调用回调函数
    //而且我肯定要保证当前定时器不能被二次delete!

    //所以这里一定不能直接delete,只是把它存入cancleTimerList_,反正到时候reset函
    //数中绝对不会把cancleTimerList_中的定时器放回红黑树定时器列表
    //无法放回红黑树的定时器一律都会被删除
    else if(callingExpired_)  
    {
        cancleTimerList_.insert(activeTimer);
    }
}