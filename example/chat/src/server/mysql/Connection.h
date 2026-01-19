#pragma once

#include <string>
#include <time.h>
#include <mysql/mysql.h>


//描述单个mysql数据库连接
class Connection
{
public:
    Connection();
    ~Connection();
    bool connect(std::string_view ip,uint16_t port,
		std::string_view user,std::string_view pwd,std::string_view dbName);
	bool update(std::string_view sql);
	MYSQL_RES* query(std::string_view sql);

	MYSQL* getConnection()
    {
        return conn_;
    }

    //每次把连接加入连接池,都要刷新最近加入的时间点
    void refreshAliveTime() 
    { 
        aliveTime_=::clock(); 
    }
	
    //获取该连接在连接池中的空闲时间
	clock_t getAliveeTime()const
    {
        return ::clock()-aliveTime_;
    }

private:
    MYSQL* conn_;

    //表示连接上一次被加入数据库连接池的时间点
    clock_t aliveTime_;
};

