#include "FriendModel.h"
#include "../mysql/ConnectionPool.h"


//添加好友关系
void FriendModel::insert(int userid,int friendid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "insert into Friend values(%d,%d)",userid,friendid);
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)  conn->update(sql);
}


//返回用户的好友列表
std::vector<User> FriendModel::query(int userid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "select a.id,a.name,a.state from User a inner join Friend b  \
        on b.friendid=a.id where b.userid=%d",userid);
    auto conn=ConnectionPool::getInstance().getConnection();
    MYSQL_RES* res=conn->query(sql);

    std::vector<User> vec;
    if(res)
    {
        MYSQL_ROW row;
        while(row=::mysql_fetch_row(res))
        {
            User user;
            user.setId(atoi(row[0]));
            user.setName(row[1]);
            user.setState(row[2]);
            vec.push_back(user);
        }
        ::mysql_free_result(res);
    }
    return vec;
}

