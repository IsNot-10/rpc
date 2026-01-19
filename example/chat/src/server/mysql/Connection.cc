#include "Connection.h"
#include "Logging.h"

Connection::Connection()
{
    conn_=::mysql_init(nullptr);
}


Connection::~Connection()
{
    if(conn_)  ::mysql_close(conn_);
}


bool Connection::connect(std::string_view ip,uint16_t port,
	std::string_view user,std::string_view pwd,std::string_view dbName)
{
    MYSQL* conn=::mysql_real_connect(conn_,ip.data(),user.data(),
        pwd.data(),dbName.data(),port,nullptr,0);
    return conn!=nullptr;
}



bool Connection::update(std::string_view sql)
{
    if(::mysql_query(conn_,sql.data()))
    {
        LOG_INFO<<"更新失败!";
        return false;
    }
    return true;
}



MYSQL_RES* Connection::query(std::string_view sql)
{
    if(::mysql_query(conn_,sql.data()))
    {
        LOG_INFO<<"查询失败!";
        return nullptr;
    }
    return ::mysql_use_result(conn_);
}

