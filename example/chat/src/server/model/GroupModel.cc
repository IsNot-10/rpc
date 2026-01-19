#include "GroupModel.h"
#include "../mysql/ConnectionPool.h"

//创建群组,实际上就是在AllGroup表中插入一条记录
//对应sql语句:insert into AllGroup(groupname,groupdesc) values...
bool GroupModel::createGroup(Group& group)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "insert into AllGroup(groupname,groupdesc) values('%s','%s')",  \
        group.getName().c_str(),group.getDesc().c_str());
    auto conn=ConnectionPool::getInstance().getConnection();

    //如果更新插入记录成功,会给这个群组分配id(否则还是默认的-1)
    if(conn&&conn->update(sql))  
    {
        group.setId(::mysql_insert_id(conn->getConnection()));
        return true;
    }
    return false;
}


    
//用户申请加入群组,实际上是在GroupUser表中插入一条记录
void GroupModel::joinGroup(int userid,int groupid,std::string_view role)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "insert into GroupUser values(%d,%d,'%s')",groupid,userid,role.data());
    auto conn=ConnectionPool::getInstance().getConnection();
    if(conn)  conn->update(sql);
}




//查询指定用户所在的全部群组
std::vector<Group> GroupModel::queryGroups(int userid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "select a.id,a.groupname,a.groupdesc from AllGroup a inner  \
        join GroupUser b on a.id=b.groupid where b.userid=%d",userid);
    auto conn=ConnectionPool::getInstance().getConnection();

    std::vector<Group> groupVec;
    if(!conn)  return groupVec;
    MYSQL_RES* res=conn->query(sql);
    if(res)
    {
        MYSQL_ROW row;
        while(row=::mysql_fetch_row(res))
        {
            Group group{atoi(row[0]),row[1],row[2]};
            groupVec.push_back(group);
        }
        ::mysql_free_result(res);
    }
    
    //上面只是填充了Group的基本信息(id,name和desc)
    //别忘了还要填充一下Group中的用户列表
    for(Group& group:groupVec)
    {
        //查询group群组中的所有用户
        snprintf(sql,sizeof sql, 
            "select a.id,a.name,a.state,b.grouprole from User a inner join  \
            GroupUser b on b.userid=a.id where b.groupid=%d",group.getId());
        MYSQL_RES* res=conn->query(sql);
        if(res)
        {
            MYSQL_ROW row;
            while(row=::mysql_fetch_row(res))
            {
                GroupUser user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                user.setRole(row[3]);
                group.getUserList().push_back(user);
            }
            ::mysql_free_result(res);
        }
    }
    return groupVec;
}



//根据指定用户和群组,查询这个群组内除了这个用户自己其他所有的用户
std::vector<int> GroupModel::queryGroupUsers(int userid,int groupid)
{
    char sql[1024]={0};
    snprintf(sql,sizeof sql,
        "select userid from GroupUser where groupid=%d and userid!=%d",
        groupid,userid);
    auto conn=ConnectionPool::getInstance().getConnection();

    std::vector<int> userList;
    if(!conn)  return userList;
    MYSQL_RES* res=conn->query(sql);
    if(res)
    {
        MYSQL_ROW row;
        while(row=::mysql_fetch_row(res))  userList.push_back(atoi(row[0]));
        ::mysql_free_result(res);
    }
    return userList;    
}