#pragma once

#include <string>


//时间戳类,是muduo中少见的值语义类
class TimeStamp
{
public:
    TimeStamp(int64_t microSecondsSinceEpoch=0);
    std::string toFormattedString(bool showMicroseconds=false)const;
    
    int64_t microSecondsSinceEpoch()const 
    { 
        return microSecondsSinceEpoch_; 
    }
    
    time_t secondsSinceEpoch()const
    { 
        return static_cast<time_t>(microSecondsSinceEpoch_/kMicroSecondsPerSecond); 
    }        

    bool valid()const
    {
        return microSecondsSinceEpoch_>0;
    }

public:
    static TimeStamp now();

    static TimeStamp invalid()
    {
        return TimeStamp{};
    }

public:
    static const int kMicroSecondsPerSecond=1000*1000;

private:
    int64_t microSecondsSinceEpoch_;
};



//定时器需要比较时间戳,所以也需要这些运算符重载
inline bool operator==(const TimeStamp& lhs,const TimeStamp& rhs)
{
    return lhs.microSecondsSinceEpoch()==rhs.microSecondsSinceEpoch();
}

inline bool operator<(const TimeStamp& lhs,const TimeStamp& rhs)
{
    return lhs.microSecondsSinceEpoch()<rhs.microSecondsSinceEpoch();
}



//这个方法就是返回一个新的TimeStamp代表timestamp时刻的seconds(时间段)后
inline TimeStamp addTime(TimeStamp timestamp,double seconds)
{
    int64_t delta=static_cast<int64_t>(seconds*TimeStamp::kMicroSecondsPerSecond);
    return TimeStamp{timestamp.microSecondsSinceEpoch()+delta};
}

