#pragma once

#include "GroupUser.h"
#include <vector>

//描述群组信息的类
class Group
{
public:
    Group(int id=-1,std::string_view name="",std::string desc="")
    :id_(id),name_(name),desc_(desc)
    {}

    int getId()const
    {
        return id_;
    }

    std::string getName()const
    {
        return name_;
    }

    std::string getDesc()const
    {
        return desc_;
    }

    std::vector<GroupUser>& getUserList()
    {
        return userList_;
    }

    void setId(int id)
    {
        id_=id;
    }

    void setName(std::string_view name)
    {
        name_=name;
    }

    void setDesc(std::string_view desc)
    {
        desc_=desc;
    }

private:
    int id_;
    std::string name_;
    std::string desc_;
    std::vector<GroupUser> userList_;
};

