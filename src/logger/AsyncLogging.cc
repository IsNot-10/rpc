#include "AsyncLogging.h"
#include "LogFile.h"
#include "TimeStamp.h"


AsyncLogging::AsyncLogging(std::string_view basename
    ,off_t rollSize,int flushInterval)
:basename_(basename),rollSize_(rollSize)
,flushInterval_(flushInterval),running_(false)
,thread_(
    [this]()
    {
        threadFunc();
    })
,currentBuffer_(std::make_unique<Buffer>())
,nextBuffer_(std::make_unique<Buffer>())
,mtx_(),cond_()
{
    currentBuffer_->bzero();
    nextBuffer_->bzero();
    bufferVec_.reserve(16);
}



AsyncLogging::~AsyncLogging()
{
    if(running_)  stop();
}


//启动后端线程
void AsyncLogging::start()
{
    running_=true;
    thread_.start();
}



//让后端线程退出循环并汇入
void AsyncLogging::stop()
{
    running_=false;
    cond_.notify_all();
    thread_.join();
}




//如果确定是使用异步日志而非同步日志,那么日志打印以后的输出回调函数就是它了
//简单的来说就是任何前端线程每打印完一次日志,数据就会从LogStream对象的小缓冲区
//(4kB)中被写到大缓冲区(4MB)currentBuffer_或者nextBuffer_上
void AsyncLogging::append(const char* logling,int len)
{
    //两个4MB的缓冲区以及bufferVec_这个缓冲队列,都是暴露给所有前端线程的
    //所以当然要用互斥锁保护
    std::lock_guard<std::mutex> lock{mtx_};
    
    //currentBuffer_能容纳的下,那就直接放进去
    if(currentBuffer_->avail()>=len)  currentBuffer_->append(logling,len);
    
    //假如现在len的大小为100B,而currentBuffer_剩余空间为80B
    //那么首先这80B的数据要移动到currentBuffer_.nextBuffer_正常情况都不会空的
    //它作为备用的缓冲区被移动到currentBuffer_中,然后currentBuffer_继续去
    //接收剩下的20B数据.大部分情况nextBuffer_不会为空,后端线程是会按时把自己
    //那边已经处理好的缓冲区移动给nextBuffer_的,所以几乎不会涉及内存分配.

    //但是也存在特殊情况,前端日志写数据过快过多,导致后端线程根本还没来得及处理
    //前端线程就一下子把currentBuffer_和nextBuffer_全部用完,也就是下面代码
    //中nextBuffer_为空的情况.这种情况下就不得不给nextBuffer_分配内存了.

    else 
    {
        bufferVec_.push_back(std::move(currentBuffer_));
        if(nextBuffer_)  currentBuffer_=std::move(nextBuffer_);
        else  nextBuffer_.reset(new Buffer{});
        currentBuffer_->append(logling,len);
        cond_.notify_all();
    }
}




//后端线程绑定的任务函数
void AsyncLogging::threadFunc()
{
    //newBuffer1和newBuffer2可以理解成分别给前端线程的currentBuffer_和
    //nextBuffer_提供内存的(移动操作,不涉及任何拷贝和内存分配)
    BufferPtr newBuffer1=std::make_unique<Buffer>();
    BufferPtr newBuffer2=std::make_unique<Buffer>();
    newBuffer1->bzero();
    newBuffer2->bzero();
    
    //后端线程这里也有个缓冲区队列,它只会和前端线程的缓冲区队列交互方法是做交换
    //(实际上底层原理是三次移动操作),因此开销很低,同时也降低了锁的粒度
    std::vector<BufferPtr> bufferToWrite;
    bufferToWrite.reserve(16);
    
    //毕竟后端线程是要把日志写到文件的,因此需要LogFile对象
    //因为异步日志中只有后端线程会涉及文件操作,所以写文件不需要考虑线程安全问题
    LogFile output{basename_,rollSize_,false};
    
    while(running_)
    {
        //每次走到这个地方,newBuffer1_和nextBuffer2_绝对是有内存的(指针非空)
        {
            std::unique_lock<std::mutex> lock{mtx_};

            //如果bufferVec_为空后端线程会阻塞,但凡里面有Buffer对象就会被唤醒
            //当然每隔3秒后端线程也会醒来,因而这里的条件是if不是while
            if(bufferVec_.empty())  
            {
                cond_.wait_for(lock,std::chrono::seconds(flushInterval_));
            }

            bufferVec_.push_back(std::move(currentBuffer_));
            //走到这里了,bufferVec_中的Buffer数量至少为1
            //实际上大部分情况bufferVec_中的数量最多是2,超过2就属于上述提及的
            //nextBuffer_还是分配内存的情况
            
            
            
            //newBuffer1和newBuffer2的作用就在这里,它们就是为了防止前端线程的
            //currentBuffer_永远不会去分配内存,nextBuffer_尽量不分配内存

            //那么问题来了,nexBuffer1和newBuffer2的内存空间又是谁给的?
            //继续往下看
            currentBuffer_=std::move(newBuffer1);
            if(!nextBuffer_)  nextBuffer_=std::move(newBuffer2);

            //将前端和后端的两个缓冲区队列做交换
            bufferToWrite.swap(bufferVec_);  
        }

        //处理日志堆积现象(它会严重占用内存影响性能)
        //muduo会对它们做简单的丢弃(只保留前两个),并在文件中写入错误信息
        if(bufferToWrite.size()>25)
        {
            char buf[256]={0};
            snprintf(buf,sizeof buf,"Dropped log messages at %s, %zd larger buffers\n",
               TimeStamp::now().toFormattedString().c_str(),
               bufferToWrite.size()-2);
            ::fputs(buf,stderr);
            output.append(buf,static_cast<int>(::strlen(buf)));
            bufferToWrite.erase(bufferToWrite.begin()+2,bufferToWrite.end());
        }

        //这里就是正常的写文件操作
        for(const auto& buffer:bufferToWrite)  
        {
            output.append(buffer->data(),buffer->length());
        }

        //所有buffer的数据都做写文件处理了,理论上它们都没有用了,但是为了给
        //newBuffer1和newBuffer2填充内存(移动操作),保留前面两个Buffer
        //缓冲区复用也算是这里很精巧的设计
        if(bufferToWrite.size()>2)  bufferToWrite.resize(2);

        //如果这里bufferToWrite的大小是1(不可能是0),说明前端线程的nextBuffer_
        //当初是没有被移给currentBuffer_的,那么newBuffer2肯定也就没有把内存移
        //给nextBuffer_.那么代码走到这里newBuffer2肯定是非空的
        
        newBuffer1=std::move(bufferToWrite.back());
        newBuffer1->reset();
        bufferToWrite.pop_back();

        if(!newBuffer2)
        {
            newBuffer2=std::move(bufferToWrite.back());
            newBuffer2->reset();
            bufferToWrite.pop_back();
        }
        bufferToWrite.clear();
        output.flush();
    }
    output.flush();
}