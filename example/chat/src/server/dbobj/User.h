#pragma once

#include <string>

class User
{
public:
    User(int id=-1,std::string_view name="",
        std::string pwd="",std::string_view state="offline")
    :id_(id),name_(name),pwd_(pwd),state_(state)
    {}
    
    void setId(int id)
    {
        id_=id;
    }

    void setName(std::string_view name)
    {
        name_=name;
    }

    void setPwd(std::string pwd)
    {
        pwd_=pwd;
    }

    void setState(std::string_view state)
    {
        state_=state;
    }

    int getId()const
    {
        return id_;
    }

    std::string getName()const
    {
        return name_;
    }

    std::string getPwd()const
    {
        return pwd_;
    }

    std::string getState()const
    {
        return state_;
    }

protected:
    int id_;
    std::string name_;
    std::string pwd_;
    std::string state_;
};

