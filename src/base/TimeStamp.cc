#include "TimeStamp.h"
#include <sys/time.h>
#include <time.h>

TimeStamp::TimeStamp(int64_t microSecondsSinceEpoch)
:microSecondsSinceEpoch_(microSecondsSinceEpoch)
{}


//转成字符串的格式
//"%4d年%02d月%02d日 星期%d %02d:%02d:%02d.%06d",时分秒.微秒
std::string TimeStamp::toFormattedString(bool showMicroseconds)const
{
    char buf[64]={0};
    time_t seconds=static_cast<time_t>(microSecondsSinceEpoch_/kMicroSecondsPerSecond);
    tm* tm_time=::localtime(&seconds);
    if(showMicroseconds)
    {
        int microseconds=static_cast<int>(
            microSecondsSinceEpoch_ %kMicroSecondsPerSecond);
        snprintf(buf, sizeof buf,"%4d%02d%02d %02d:%02d:%02d.%06d",
            tm_time->tm_year+1900,tm_time->tm_mon+1,
            tm_time->tm_mday,tm_time->tm_hour,
            tm_time->tm_min,tm_time->tm_sec,microseconds);
    }
    else
    {
        snprintf(buf,sizeof buf,"%4d%02d%02d %02d:%02d:%02d",
            tm_time->tm_year+1900,tm_time->tm_mon+1,
            tm_time->tm_mday,tm_time->tm_hour,
            tm_time->tm_min,tm_time->tm_sec);
    }
    return buf;
}


TimeStamp TimeStamp::now()
{
    timeval tv;
    ::gettimeofday(&tv,nullptr);
    int64_t seconds=tv.tv_sec;
    return TimeStamp{seconds*kMicroSecondsPerSecond+tv.tv_usec};
}