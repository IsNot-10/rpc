#include "OfflineMessageModel.h"
#include "../mysql/ConnectionPool.h"


//插入用户的离线信息
//对应sql语句:insert into OfflineMessage values(...)
void OfflineMessageModel::insert(int userid,const std::string& msg)
{
    char sql[1024]={0};
    ::snprintf(sql,sizeof sql,
        "insert into OfflineMessage values(%d,'%s')",userid,msg.c_str());
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)  conn->update(sql);
}


    
//清空用户的离线信息
//对应sql语句:delete from OfflineMessage where userid=...
void OfflineMessageModel::remove(int userid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "delete from OfflineMessage where userid=%d",userid);
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)  conn->update(sql);   
}



//查询用户的离线信息
//对应sql语句:select message from OfflineMessage where userid=...
std::vector<std::string> OfflineMessageModel::query(int userid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "select message from OfflineMessage where userid=%d",userid);
    auto conn=ConnectionPool::getInstance().getConnection();
    std::vector<std::string> vec;
    if(!conn)  return vec;
    MYSQL_RES* res=conn->query(sql);
    if(res)
    {
        MYSQL_ROW row;
        while(row=::mysql_fetch_row(res))  vec.push_back(row[0]);
        ::mysql_free_result(res);
    }
    return vec;
}

