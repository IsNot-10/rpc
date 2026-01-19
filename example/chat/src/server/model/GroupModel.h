#pragma once

#include "../dbobj/Group.h"

//操纵群组相关业务的类(涉及AllGroup和GroupUser两个表)
class GroupModel
{
public:
    //创建群组
    bool createGroup(Group& group);
    
    //用户申请加入群组
    void joinGroup(int userid,int groupid,std::string_view role);

    //查询指定用户所在的全部群组
    std::vector<Group> queryGroups(int userid);

    //根据指定用户和群组,查询这个群组内除了这个用户自己其他所有的用户
    std::vector<int> queryGroupUsers(int userid,int groupid);
};

