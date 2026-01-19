#pragma once

namespace CurrentThread
{
    extern thread_local int t_cachedTid; 
    
    void cacheTid();

    //获取一个线程id
    inline int tid()
    {
        if (__builtin_expect(t_cachedTid==0,0))  cacheTid();
        return t_cachedTid;
    }
}