#include "Logging.h"
#include "CurrentThread.h"
#include <time.h>

thread_local char t_errnobuf[512];
thread_local char t_time[64];
thread_local time_t t_lastSecond;


//获取错误信息
const char* getErrnoMsg(int savedErrno)
{
    return ::strerror_r(savedErrno,t_errnobuf,sizeof t_errnobuf);
}


//把日志级别从枚举类转换成字符串
const std::string LogLevelName(Logger::LogLevel level)
{
    if(level==Logger::LogLevel::TRACE)  return "[TRACE] ";
    if(level==Logger::LogLevel::DEBUG)  return "[DEBUG] ";
    if(level==Logger::LogLevel::INFO)  return "[INFO] ";
    if(level==Logger::LogLevel::WARN)  return "[WARN] ";
    if(level==Logger::LogLevel::ERROR)  return "[ERROR ]";  
    return "[FATAL] ";
};




//默认的输出回调函数,这种情况本质上跟std::cout没多大区别,数据被输出到终端
//具体来说这个函数把数据写到了stdout文件流
static void defaultOutput(const char* data,int len)
{
    ::fwrite(data,1,len,stdout);
}



//默认的刷新缓冲区回调函数,就这个函数来说就是把stdout文件流的数据强制写到
//stdout文件对象的页缓存(实际上和::write系统调用函数更像)
//注意fflush不是强制刷新到磁盘,那个功能需要的是sync函数
static void defaultFlush()
{
    ::fflush(stdout);
}



//这些都是全局变量,可以由Logger的static函数设置
//它们都只会被Logger对象的析构函数调用
Logger::OutputFunc g_output=defaultOutput;
Logger::FlushFunc g_flush=defaultFlush;




//把当前的时间转换成字符串的形式,实际上只是Impl对象构造函数用到的一个工具函数
//然后日志打印这个时间(我姑且就叫打印吧,实际上这里还不涉及真正的输出,只是将数据
//放入用户缓冲区,也就是LogStream对象内部的buffer_)
void Logger::Impl::formatTime()
{
    TimeStamp now{TimeStamp::now()};
    time_t seconds=static_cast<time_t>(now.microSecondsSinceEpoch()/TimeStamp::kMicroSecondsPerSecond);
    int microseconds=static_cast<int>(now.microSecondsSinceEpoch()%TimeStamp::kMicroSecondsPerSecond);
    tm* tm_time=::localtime(&seconds);
    snprintf(t_time,sizeof(t_time),
        "%4d/%02d/%02d %02d:%02d:%02d",
        tm_time->tm_year+1900,
        tm_time->tm_mon+1,
        tm_time->tm_mday,
        tm_time->tm_hour,
        tm_time->tm_min,
        tm_time->tm_sec);
    t_lastSecond=seconds;
    char buf[32]={0};
    snprintf(buf,sizeof buf,"%06d ",microseconds);
    stream_<<t_time<<" "<<buf;
}



//会在Logger对象的析构函数中被调用,日志打印文件名和所在行数
void Logger::Impl::finish()
{
    stream_<<" - "<<std::string{basename_.data_,basename_.size_}
        <<':'<<line_<<'\n';
}




//Impl对象的构造函数
//打印当前时间,日志级别和错误信息
Logger::Impl::Impl(LogLevel level,int savedErrno,const char* file,int line)
:level_(level),basename_(file),line_(line)
{
    formatTime();
    stream_<<LogLevelName(level_);
    if(savedErrno!=0)
    {
        stream_<<getErrnoMsg(savedErrno)<<"(errno="<<savedErrno<<")";
    }
}




Logger::Logger(const char* file,int line,Logger::LogLevel level)
:impl_(level,0,file,line)
{}




//Logger对象的构造函数会先调用Impl对象的构造函数,后者会先打印日志的当前时间,
//日志级别和错误信息,然后这里再打印一下所在的函数.
Logger::Logger(const char* file,int line,Logger::LogLevel level,const char* func)
:impl_(level,0,file,line)
{
    impl_.stream_<<func<<' ';
}



//析构函数才是最重要的
Logger::~Logger()
{
    //日志打印文件名和当前行数
    impl_.finish();
    const LogStream::Buffer& buf=getStream().getBuffer();

    //输出回调函数,把原本打印的数据连同刚刚的文件名和当前行数一起写到我指定的地方
    //(可以是输出文件流也可以是其他用户缓冲区)
    g_output(buf.data(),buf.length());

    //如果日志级别是LogLevel::FATAL必须强制刷新缓冲区,把数据写到文件对象的页缓存
    if(impl_.level_==LogLevel::FATAL)
    {
        g_flush();
        ::abort();
    }
}



//设置日志级别
void Logger::setLogLevel(Logger::LogLevel level)
{
    g_logLevel=level;
}


//设置输出回调函数
void Logger::setOutput(OutputFunc out)
{
    g_output=out;
}


//设置刷新缓冲区回调函数
void Logger::setFlush(FlushFunc flush)
{
    g_flush=flush;
}
