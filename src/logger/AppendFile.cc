#include "AppendFile.h"
#include "Logging.h"


//文件存在就直接追加数据进去(而不是截断).否则创建一个新文件
AppendFile::AppendFile(std::string_view fileName)
:fp_(::fopen(fileName.data(),"ae")),writtenBytes_(0)
{
    //将文件的缓冲区设置为buffer_
    //也就是说后面对文件做::fwrite操作都是先把数据写到buffer_
    ::setbuffer(fp_,buffer_,sizeof buffer_);
}


AppendFile::~AppendFile()
{
    ::fclose(fp_);
}


//把数据全部追加到buffer_
void AppendFile::append(const char* data,size_t len)
{
    size_t writen=0;
    while(writen!=len)
    {
        size_t remaining=len-writen;
        size_t n=write(data+writen,remaining);
        if(n!=remaining)
        {
            int err=::ferror(fp_);
            if(err)
            {
                ::fprintf(stderr,"FileUtil::append() failed %s\n",getErrnoMsg(err));
            }
        }
        writen+=n;
    }
    writtenBytes_+=writen;
}



//将buffer_中的数据写到文件页缓存
void AppendFile::flush()
{
    ::fflush(fp_);
}


size_t AppendFile::write(const char* data,size_t len)
{
    return ::fwrite_unlocked(data,1,len,fp_);
}
