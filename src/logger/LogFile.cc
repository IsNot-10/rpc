#include "LogFile.h"
#include "AppendFile.h"


LogFile::LogFile(std::string_view basename,off_t rollSize
    ,bool threadSafe,int flushInterval,int checkEveryN)
:basename_(basename),rollSize_(rollSize)
,flushInterval_(flushInterval),checkEveryN_(checkEveryN)
,count_(0),startOfPeriod_(0),lastRoll_(0),lastFlush_(0)
,mtx_(threadSafe?std::make_unique<std::mutex>():nullptr)
{
    //每创建一个LogFile对象是要新建一个文件的
    rollFile();
}


LogFile::~LogFile()=default;



//写文件需要考虑是否线程安全
//如果是同步日志,那是线程不安全的,因为多个线程都会把日志写入文件
//但如果是异步日志,总共就只有一个后端线程会把日志写入文件,那必然是线程安全的
void LogFile::append(const char* logline,int len)
{
    if(mtx_)
    {
        std::lock_guard<std::mutex> lock{*mtx_};
        append_unlocked(logline,len);
    }
    else  append_unlocked(logline,len);
}



//真正的写文件逻辑
void LogFile::append_unlocked(const char* logline,int len)
{
    file_->append(logline,len);

    //如果文件的数据大小超过了阈值就要创建新的日志文件
    if(file_->writtenBytes()>rollSize_)  rollFile();
    
    //如果append的次数超过了这个阈值
    else if(++count_>=checkEveryN_)
    {
        count_=0;
        time_t now=::time(nullptr);

        //从纪元开始计算,直到今天零点的秒数
        time_t thisPeriod=now/kRollPerSeconds_*kRollPerSeconds_;

        //如果今天还没有创建过日志并且append的次数超过阈值,也要创建新的日志文件
        if(thisPeriod!=startOfPeriod_)  rollFile();
        
        //走到这里说明就不会创建新的日志文件,如果刷新间隔超过阈值就刷新
        else if(now-lastFlush_>flushInterval_)
        {
            lastFlush_=now;
            file_->flush();
        }
    }
}




//底层调用file_的flush函数,同样也要考虑是否线程安全的问题
//具体行为就是把file_的buffer_缓冲区数据写到文件(实际上是页缓存)
void LogFile::flush()
{
    if(mtx_)  
    {
        std::lock_guard<std::mutex> lock{*mtx_};
        file_->flush();
    }
    else  file_->flush();
}



//滚动日志,其实就是创建一个新的日志文件
bool LogFile::rollFile()
{
    time_t now=0;
    std::string filename=getLogFileName(&now);
    time_t start=now/kRollPerSeconds_*kRollPerSeconds_;
    if(now>lastRoll_)
    {
        lastRoll_=now;
        lastFlush_=now;
        startOfPeriod_=start;  
        file_.reset(new AppendFile{filename});
        return true;
    }
    return false;
}




//每次创建一个新文件的时候,都要根据当前时间去生成一个新的文件名
//本项目中日志名格式为: basename + time + ".log"
std::string LogFile::getLogFileName(time_t* now)
{
    std::string filename;
    filename.reserve(basename_.size()+64);
    filename=basename_;

    //根据当前时间生成相应的字符串
    char timebuf[32];
    struct tm tm;
    *now=::time(nullptr);
    ::localtime_r(now,&tm);
    ::strftime(timebuf,sizeof timebuf,".%Y%m%d-%H%M%S",&tm);
    
    filename+=timebuf;
    filename+=".log";
    return filename;
}


