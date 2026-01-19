#pragma once

enum MsgType
{
    LOGIN_MSG=1,        //1:登录
    LOGIN_MSG_ACK,      //2:服务器响应登录
    LOGINOUT_MSG,       //3:退出登录
    REGIST_MSG,         //4:注册
    REGIST_MSG_ACK,     //5:服务器响应注册
    ONE_CHAT_MSG,       //6:单人聊天
    ADD_FRIEND_MSG,     //7:添加好友
    CREATE_GROUP_MSG,   //8:创建群组
    JOIN_GROUP_MSG,     //9:加入群组 
    GROUP_CHAT_MSG      //10:群组聊天
};

