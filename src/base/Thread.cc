#include "Thread.h"
#include "CurrentThread.h"
#include "semaphore.h"

Thread::Thread(ThreadFunc threadFunc,std::string_view name)
:started_(false),joined_(false)
,threadFunc_(std::move(threadFunc))
,tid_(0),name_(name)
{
    if(name_.empty())
    {
        char buf[128]={0};
        snprintf(buf,sizeof buf,"Thread%d",++threadNum);
        name_=buf;
    }
}


//如果线程还在启动状态并且之前没有调用join,那就直接设成后台守护线程
Thread::~Thread()
{
    if(started_&&!joined_)  thread_->detach();
}



//启动线程,启动时会先初始化线程的id再调用绑定的回调函数
//注意:线程的tid初始化工作完成才可以判定start调用结束(线程异步执行中)
//不用信号量同步的话有可能start调用完线程id还没初始化好
void Thread::start()
{
    sem_t sem;
    sem_init(&sem,false,0);
    started_=true;
    thread_=std::make_unique<std::thread>(
        [this,&sem]()
        {
            tid_=CurrentThread::tid();
            sem_post(&sem);
            threadFunc_();
        });
    sem_wait(&sem);    
}



void Thread::join()
{
    thread_->join();
    joined_=true;
}
