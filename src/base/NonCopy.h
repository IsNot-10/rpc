#pragma once


//禁止复制的类,任何继承了NonCopy的类都是引用语义(或者说是对象语义)
//muduo网络库中大部分的类都是对象语义

//继承Noncopy也只需要private继承而不需要public继承
//也就是只有实现继承没有接口继承
class NonCopy
{
public:
    NonCopy(const NonCopy&)=delete;
    NonCopy& operator=(const NonCopy&)=delete;

protected:
    NonCopy()=default;
    ~NonCopy()=default;
};

