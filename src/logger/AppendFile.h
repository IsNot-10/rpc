#pragma once

#include "NonCopy.h"
#include <stdio.h>
#include <string>


//其实就是一个仅仅只关心往磁盘文件写数据的类(无需关注是否要滚动日志)
//自身作为LogFile的工具类,作为其数据成员存在
//也是个RAII类,在构造函数打开文件,在析构函数关闭文件
class AppendFile
:NonCopy
{
public:
    explicit AppendFile(std::string_view fileName);
    ~AppendFile();
    void append(const char* data,size_t len);
    void flush();

    //当前文件已经写了多少数据,以便LogFile根据写入数据量来判断是否需要滚动日志
    off_t writtenBytes()const
    {
        return writtenBytes_;
    }

private:
    size_t write(const char* data,size_t len);

private:
    //操控文件的指针
    FILE* fp_;

    //会被设置为被打开文件的缓冲区,大小为64KB
    char buffer_[64*1024];
    
    //当前写入的数据量
    off_t writtenBytes_;
};

