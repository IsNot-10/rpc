#pragma once

#include "LogStream.h"
#include "TimeStamp.h"
#include <functional>



class Logger
{
public:

    //6中日志级别
    enum class LogLevel
    {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        FATAL
    };


    //实际上这个类就是从一个文件路径中获取文件具体名字和名字长度
    struct SourceFile
    {
        explicit SourceFile(const char* filename)
        :data_(filename)
        {
            //找出data_中最后一个出现'/'的位置,往后起就是文件的具体名字
            const char* slash=::strrchr(data_,'/');
            if(slash)  data_=slash+1;
            size_=::strlen(data_);
        }

        const char* data_;
        size_t size_;
    };

    using OutputFunc=std::function<void(const char* msg,int len)>;
    using FlushFunc=std::function<void()>;

public:
    Logger(const char* file,int line,LogLevel level);

    //也就level为LogLevel::TRACE或者LogLevel::DEBUG时候,需要func
    Logger(const char* file,int line,LogLevel level,const char* func);
    
    //Logger类的析构函数才真正完成数据的输出和缓冲区的刷新
    ~Logger();

    //这些static函数全部都是获取/设置全局的变量
    static LogLevel getLogLevel();
    static void setLogLevel(LogLevel level);
    static void setOutput(OutputFunc);
    static void setFlush(FlushFunc);
    
    LogStream& getStream()
    {
        return impl_.stream_;
    }

private:
    struct Impl
    {
        Impl(LogLevel level,int savedErrno,const char* file,int line);
        void formatTime();
        void finish();

        //别和全局的日志级别搞混了,这个根据我们打印日志时用的宏决定
        //比如我写LOG_DEBUG<<.....,那么level_就是LogLevel::DEBUG
        LogLevel level_;

        //这三个数据成员分别代表文件名,所在的行数和时间
        //它们都是被Logger对象的构造函数初始化的(实际上也是宏给的)
        SourceFile basename_;
        int line_;
        TimeStamp time_;

        //自定义的高性能输出流
        LogStream stream_;
    };

    Impl impl_;
};


//这是全局变量,日志最低级别
//比如默认情况下日志最低级别是INFO,那么DEBUG日志就无法输出
inline Logger::LogLevel g_logLevel=Logger::LogLevel::INFO;


//获取全局日志最低级别
inline Logger::LogLevel Logger::getLogLevel()
{
    return g_logLevel;
}



//获取错误信息
const char* getErrnoMsg(int savedErrno);




//日志宏,实际上当我写LOG_INFO<<"....."这行代码的时候
//就是先创建一个Logger临时对象并返回它内部的LogStream对象,而LogStream对象又重载
//了<<运算符并且返回类型是引用,因此可以像std::cout那样灵活的进行链式编程
//只不过LogStream对象的<<运算符是把数据写到内部的FixBuffer缓冲区

//而写完这句代码Logger对象就析构了,析构的时候会把FixBuffer缓冲区(用户缓冲区)里的
//数据全部写到文件流,最后也会同步刷新到文件对象页缓存

#define LOG_TRACE if(Logger::getLogLevel()<=Logger::LogLevel::TRACE) \
    Logger(__FILE__,__LINE__,Logger::LogLevel::TRACE,__func__).getStream() 

#define LOG_DEBUG if(Logger::getLogLevel()<=Logger::LogLevel::DEBUG) \
    Logger(__FILE__,__LINE__,Logger::LogLevel::DEBUG,__func__).getStream()  
    
#define LOG_INFO if(Logger::getLogLevel()<=Logger::LogLevel::INFO) \
    Logger(__FILE__,__LINE__,Logger::LogLevel::INFO).getStream()    

#define LOG_WARN Logger(__FILE__,__LINE__,Logger::LogLevel::WARN).getStream()
#define LOG_ERROR Logger(__FILE__,__LINE__,Logger::LogLevel::ERROR).getStream()
#define LOG_FATAL Logger(__FILE__,__LINE__,Logger::LogLevel::FATAL).getStream()
