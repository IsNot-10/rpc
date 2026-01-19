#pragma once

#include "../dbobj/User.h"
#include <vector>


//用来操纵Friend表的类
class FriendModel
{
public:
    //添加好友关系
    void insert(int userid,int friendid);

    //返回用户的好友列表
    std::vector<User> query(int userid);
};

