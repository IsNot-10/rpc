#pragma once

#include "../dbobj/User.h"
#include <optional>


//纯接口类,没有任何数据成员
class UserModel
{
public:
    bool insert(User& user);
    User query(int id);
    bool updateState(User& user);
    void resetState();
};

