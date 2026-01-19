#include "UserModel.h"
#include "../mysql/ConnectionPool.h"


//根据user对象的属性,在User表中插入一条记录
bool UserModel::insert(User& user)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "insert into User(name,password,state) values ('%s','%s','%s')",
        user.getName().c_str(),user.getPwd().c_str(),user.getState().c_str());
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)
    {
        //sql语句执行成功,那么会自动生成主键赋给user
        if(conn->update(sql))
        {
            user.setId(::mysql_insert_id(conn->getConnection()));
            return true;
        }
    }
    return false;
}



//执行sql语句"select * from User where id=..."
User UserModel::query(int id)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,"select * from User where id=%d",id);
    auto conn=ConnectionPool::getInstance().getConnection();
    if(!conn)  return User{};
    MYSQL_RES* res=conn->query(sql);
    if(res)
    {
        MYSQL_ROW row=::mysql_fetch_row(res);
        if(row)
        {
            User user{atoi(row[0]),row[1],row[2],row[3]};
            ::mysql_free_result(res);
            return user;
        }
    }
    return User{};
}



//更改用户的状态信息
bool UserModel::updateState(User& user)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,"update User set state='%s' where id=%d",
        user.getState().c_str(),user.getId());
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn&&conn->update(sql))  return true;
    return false;
}



//重置用户的状态,实际上就是把在线状态全部改成离线状态
void UserModel::resetState()
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "update User set state='offline' where state='online'");
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)  conn->update(sql);
}