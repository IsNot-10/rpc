#pragma once

#include "NonCopy.h"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <ctime>

class AppendFile;


//这个类负责向磁盘文件中写入日志信息
//它需要关心很多问题,比如是否因为数据量足够多而需要滚动日志,是否线程安全
class LogFile
:NonCopy
{
public:
    LogFile(std::string_view basename,off_t rollSize
        ,bool threadSafe=true,int flushInterval=3,int checkEveryN=1024);
    ~LogFile();
    void append(const char* logline,int len);
    void flush();
    bool rollFile();

private:
    void append_unlocked(const char* logline,int len);
    std::string getLogFileName(time_t* now);

private:
    static constexpr int kRollPerSeconds_=60*60*24;   //一天的时间

    //作为文件名字的一部分(前缀)
    const std::string basename_;

    //文件中写入数据超过这个阈值就会滚动日志     
    const off_t rollSize_;       
    
    //刷新的时间间隔
    const int flushInterval_;                       
    
    //文件中写入数据没有超过阈值的情况下(没有滚动日志),
    //写入次数超过这个数就会刷新一下(把file_的buffer_中数据写到文件页缓存)
    const int checkEveryN_;                        
    
    //往file_的buffer_写日志的次数
    int count_;
    
    //上一个日志的创建时间
    time_t startOfPeriod_;                         
    
    //上次滚动日志的时间
    time_t lastRoll_;                              
    
    //上次刷新的时间
    time_t lastFlush_;

    //专门负责往文件中追加数据,除此之外啥也不用关心
    //LogFile对象调用它的writtenBytes函数判断是否需要滚动日志
    std::unique_ptr<AppendFile> file_;

    //如果线程安全(异步日志),那么它就为空,根本不存在互斥锁
    std::unique_ptr<std::mutex> mtx_;
};

