#pragma once

#include <string>
#include <vector>

//专门操纵OfflineMessage表的类
class OfflineMessageModel
{
public:
    //插入用户的离线信息
    void insert(int userid,const std::string& msg);
    
    //清空用户的离线信息
    void remove(int userid);

    //查询用户的离线信息
    std::vector<std::string> query(int userid);
};

