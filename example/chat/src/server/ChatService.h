#pragma once

#include "TimeStamp.h"
#include "TcpConnection.h"
#include "../json.h"
#include "./model/UserModel.h"
#include "./model/OfflineMessageModel.h"
#include "./model/FriendModel.h"
#include "./model/GroupModel.h"
#include "./redis/Redis.h"
#include <unordered_map>

using MsgHandler=std::function<void(const TcpConnectionPtr&,nlohmann::json&,TimeStamp)>;


//业务模块,主要任务就是实现各种业务(还会根据不同的消息类型调用不同业务函数)
class ChatService
{
public:
    static ChatService& getInstance();
    MsgHandler getHandler(int msgid);
    void clientCloseReset(const TcpConnectionPtr& conn);
    void serverCloseExceptionReset();
    void redisSubscribeMsgCb(int channel,const std::string& msg);

    //以下属于客户端请求的各种业务函数
    void login(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void loginout(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void regist(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void oneChat(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp); 
    void addFriend(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void createGroup(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void joinGroup(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);
    void groupChat(const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp);

public:
    ChatService(const ChatService&)=delete;
    ChatService& operator=(const ChatService&)=delete;

private:
    ChatService();    

private:
    //存储了消息id和对应的处理消息的业务函数
    std::unordered_map<int,MsgHandler> msgHandlerMap_;
   
    //存储在线的用户id和对应用户的tcp连接,需要考虑线程安全,用互斥锁保护
    //可以用它来查看某用户是否在此台机器上在线
    std::unordered_map<int,TcpConnectionPtr> connMap_;
    std::mutex connMtx_;

    //数据操作类对象(调用mysql api的)
    UserModel userModel_;
    OfflineMessageModel offlineMessageModel_;
    FriendModel friendModel_;
    GroupModel groupModel_;

    //redis操作对象
    Redis redis_;
};

