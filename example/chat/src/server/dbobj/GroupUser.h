#pragma once

#include "User.h"

//作为User类的子类,主要用于描述某个用户在某个组中的地位
//(是群主还是普通成员)
class GroupUser
:public User
{
public:
    GroupUser()=default; 

    void setRole(std::string_view role)
    {
        role_=role;
    }
    
    std::string getRole()const
    {
        return role_;
    }

private:
    std::string role_;
};

