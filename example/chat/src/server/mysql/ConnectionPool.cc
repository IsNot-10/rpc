#include "ConnectionPool.h"
#include "Logging.h"
#include <fstream>
#include <thread>


ConnectionPool::ConnectionPool()
{
    if(!loadConfigFile())  return;
    for(int i=0;i<initSize_;i++) 
    {
        Connection* conn=new Connection{};
        if(conn->connect(ip_,port_,userName_,pwd_,dbName_))
        {
            connQue_.push(conn);
            conn->refreshAliveTime();
            ++connNum_;
        }
    }

    //生产者线程,只有连接队列为空的时候才会加入新连接进队列
    //当然不会超过连接数上限
    std::thread producer{
        [this]()
        {
            while(true)
            {
                std::unique_lock<std::mutex> lock{mtx_};
                while(connQue_.size())  cv_.wait(lock);
                if(connNum_<maxSize_)  
                {
                    Connection* conn=new Connection{};
                    if(conn->connect(ip_,port_,userName_,pwd_,dbName_))
                    {
                        connQue_.push(conn);
                        conn->refreshAliveTime();
                        ++connNum_;
                    }
                    cv_.notify_all();
                }
            }
        }
    };



    //清理线程,会遍历连接队列,如果发现某个连接空闲时间超过阈值
    //就会把这个连接删除
    std::thread cleaner{
        [this]()
        {
            while(true)
            {
                //每500ms检测一次
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                std::lock_guard<std::mutex> lock{mtx_};
                while(connQue_.size()>initSize_)
                {
                    auto conn=connQue_.front();
                    if(conn->getAliveeTime()<maxIdleTime_)  break;  
                    delete conn;
                    connQue_.pop();
                    --connNum_;
                }
            }
        }
    };
    
    //都设置为后台线程
    producer.detach();
    cleaner.detach();
}



ConnectionPool::~ConnectionPool()
{
    while(connQue_.size())
    {
        auto conn=connQue_.front();
        delete conn;
        connQue_.pop();
        --connNum_;
    }
}



//提供给外部获取连接的接口,会返回智能指针并且重写了删除器
//重写的删除器会把连接放回连接池而不会销毁
std::unique_ptr<Connection,ConnectionPool::delFunc> ConnectionPool::getConnection()
{
    std::unique_lock<std::mutex> lock{mtx_};
    while(connQue_.empty())  
    {
        if(std::cv_status::timeout==
            cv_.wait_for(lock,std::chrono::milliseconds(connectionTimeOut_)))
        {
            if(connQue_.empty())  
            {
                return nullptr;
            }
        }
    }
    std::unique_ptr<Connection,delFunc> ptr{
        connQue_.front(),
        [this](Connection* conn)
        {
            std::lock_guard<std::mutex> lock{mtx_};
            connQue_.push(conn);
            conn->refreshAliveTime();
        }
    };
    connQue_.pop();
    cv_.notify_all();
    return ptr;
}



//工具函数,读取配置文件初始化数据库连接池的信息
bool ConnectionPool::loadConfigFile()
{
    std::ifstream ifs("mysql.conf");
    if(!ifs.is_open())
    {
        LOG_FATAL<<"can not open file mysql.conf!";
    }
    std::string line;
    while(std::getline(ifs,line))
    {
        if(line.empty()||line[0]=='#')  continue;
        int part=line.find('=');
        std::string key=line.substr(0,part),val=line.substr(part+1);
        if(key=="ip")  ip_=val;
        else if(key=="port")  port_=std::stoi(val);
        else if(key=="username")  userName_=val;
        else if(key=="password")  pwd_=val;
        else if(key=="dbname")  dbName_=val;
        else if(key=="initSize")  initSize_=std::stoi(val);
        else if(key=="maxSize")  maxSize_=std::stoi(val);
        else if(key=="maxIdleTime")  maxIdleTime_=std::stoi(val);
        else if(key=="connectionTimeOut")  connectionTimeOut_=std::stoi(val);
    }
    LOG_INFO<<"load mysql.conf success!";
    return true;
}


ConnectionPool& ConnectionPool::getInstance()
{
    static ConnectionPool pool;
    return pool;
}





