#include "ChatService.h"
#include "Logging.h"
#include "../MsgType.h"
#include "./dbobj/User.h"

ChatService& ChatService::getInstance()
{
    static ChatService service;
    return service;
}



ChatService::ChatService()
{
    //构造函数中首先会把所有的消息id和业务函数存入哈希表
    msgHandlerMap_.emplace(LOGIN_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            login(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(LOGINOUT_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            loginout(conn,js,timeStamp);
        });
     msgHandlerMap_.emplace(REGIST_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            regist(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(ONE_CHAT_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            oneChat(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(ADD_FRIEND_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            addFriend(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(CREATE_GROUP_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            createGroup(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(JOIN_GROUP_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            joinGroup(conn,js,timeStamp);
        });
    msgHandlerMap_.emplace(GROUP_CHAT_MSG,
        [this](const TcpConnectionPtr& conn,nlohmann::json& js,TimeStamp timeStamp)
        {
            groupChat(conn,js,timeStamp);
        });

    //连接redis服务端,并设置回调函数
    if(redis_.connect())
    {
        redis_.initNotifyCb(
            [this](int channel,const std::string& msg)
            {
                redisSubscribeMsgCb(channel,msg);
            });
    }
}   



//根据消息类型id去获取对应的业务处理函数
MsgHandler ChatService::getHandler(int msgid)
{
    if(msgHandlerMap_.find(msgid)==msgHandlerMap_.end())
    {
        LOG_ERROR<<"msgid: "<<msgid<<" can not find handler!";
        return MsgHandler{};   
    }
    return msgHandlerMap_[msgid];
}


//这个channel其实就是用户id
void ChatService::redisSubscribeMsgCb(int channel,const std::string& msg)
{
    //用户在线的情况
    {
        std::lock_guard<std::mutex> lock{connMtx_};
        if(connMap_.find(channel)!=connMap_.end())
        {
            connMap_[channel]->send(msg);
            return;
        }
    }

    //转存离线
    offlineMessageModel_.insert(channel,msg);
}



//登录业务
void ChatService::login(const TcpConnectionPtr& conn,
        nlohmann::json& js,TimeStamp timeStamp)
{
    LOG_DEBUG<<"do login service!";
    int userid=js["id"].get<int>();

    //根据id从User表中查询记录
    User user=userModel_.query(userid);
    std::string pwd=js["password"];

    //准备发送回去的响应信息
    nlohmann::json resp;
    resp["msgid"]=LOGIN_MSG_ACK;

    //如果没有查询成功的话用户id不会被分配,那就是-1了,不可能成功登录的
    if(user.getId()==userid&&user.getPwd()==pwd)
    {
        //用户已经在线,说明重复登录
        if(user.getState()=="online")
        {
            resp["errno"]=2;
            resp["errmsg"]="this account is using,input another!";
        }

        //用户原本离线,那就是登录成功的情况了,并把离线状态改为在线状态
        else
        {
            //先存储一下在线用户的id和对应tcp连接
            {
                std::lock_guard<std::mutex> lock{connMtx_};
                connMap_.emplace(userid,conn);
                conn->setContext(userid);
            }

            //这里在数据库中也更新一下用户状态(离线改成在线)
            user.setState("online");
            userModel_.updateState(user);
            resp["errno"]=0;
            resp["id"]=user.getId();
            resp["name"]=user.getName();

            //当前服务进程上所有登录(在线)用户,都会关注它们的id对应channel
            //以便其他服务进程上有其他用户向这个用户发消息
            redis_.subscribe(userid);

            //登录成功时需要查看一下有没有离线信息
            const auto offlinemsg=offlineMessageModel_.query(userid);
            if(offlinemsg.size())
            {
                resp["offlinemsg"]=offlinemsg;
                offlineMessageModel_.remove(userid);
            }

            //每次登录成功时,服务器还需要告诉客户端用户的所有好友已经群组

            //获取好友信息
            const auto friendVec=friendModel_.query(userid);
            if(friendVec.size())
            {
                std::vector<std::string> friendJs;
                for(const auto& friendUser:friendVec)
                {
                    nlohmann::json user_js;
                    user_js["id"]=friendUser.getId();
                    user_js["name"]=friendUser.getName();
                    user_js["state"]=friendUser.getState();
                    friendJs.push_back(user_js.dump());
                }
                resp["friends"]=friendJs;
            }

            //获取群组信息
            auto groupVec=groupModel_.queryGroups(userid);
            if(groupVec.size())
            {
                std::vector<std::string> groupJs;
                for(auto& group:groupVec)
                {
                    nlohmann::json group_js;
                    group_js["groupid"]=group.getId();
                    group_js["groupname"]=group.getName();
                    group_js["groupdesc"]=group.getDesc();

                    //群内有哪些成员,的信息也要给出
                    std::vector<std::string> userVec;
                    for(auto& groupUser:group.getUserList())
                    {
                        nlohmann::json user_js;
                        user_js["id"]=groupUser.getId();
                        user_js["name"]=groupUser.getName();
                        user_js["state"]=groupUser.getState();
                        user_js["role"]=groupUser.getRole();
                        userVec.push_back(user_js.dump());
                    }

                    group_js["users"]=userVec;
                    groupJs.push_back(group_js.dump());
                }
                resp["groups"]=groupJs;
            }
        }
    }

    //登录失败(也不是重复登录)的情况
    else
    {
        resp["errno"]=1;
        resp["errmsg"]="id or password is invalid!";
    }
    conn->send(resp.dump());
}



//退出登录业务
//其实和处理客户断开连接的情况差不多
void ChatService::loginout(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int userid=js["id"].get<int>();

    //先在连接表中删除这个tcp连接
    {
        std::lock_guard<std::mutex> lock{connMtx_};
        if(connMap_.find(userid)!=connMap_.end())  connMap_.erase(userid);
    }

    //再在数据库中把这个用户状态改为离线状态
    User user{userid,"","","offline"};
    userModel_.updateState(user);

    //当前服务进程不会再关注这个用户相关的消息
    redis_.unsubscribe(userid);
}



//注册业务
void ChatService::regist(const TcpConnectionPtr& conn,
        nlohmann::json& js,TimeStamp timeStamp)
{
    LOG_DEBUG<<"do register service";
    std::string name=js["name"];
    std::string pwd=js["password"];
    User user;
    user.setName(name);
    user.setPwd(pwd);

    //尝试在User表中插入一条用户记录
    bool success=userModel_.insert(user);
    nlohmann::json resp;
    resp["msgid"]=REGIST_MSG_ACK;

    //如果注册成功(插入记录成功)是会给这个用户分配id号的
    if(success)
    {
        resp["id"]=user.getId();
        resp["errno"]=0;
    }

    //注册失败
    else  resp["errno"]=1;
    conn->send(resp.dump());
}




//一对一聊天的业务
//首先需要查找聊天对象用户是否在线
void ChatService::oneChat(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int toid=js["toid"].get<int>();

    //这种情况说明源用户和对象用户不仅在同一台机器上而且聊天对象是在线的
    //服务器充当转发器,直接把js原样转发给聊天对象用户
    {
        std::lock_guard<std::mutex> lock{connMtx_};
        if(connMap_.find(toid)!=connMap_.end())
        {
            connMap_[toid]->send(js.dump());
            return;
        }
    }

    User user=userModel_.query(toid);
    
    //如果查到在线,那说明这个用户在线,但是在另一个服务进程中而不在当前进程
    if(user.getState()=="online")
    {
        redis_.publish(toid,js.dump());
        return;
    }

    //真的离线的话,存储一下聊天对象用户的离线信息
    offlineMessageModel_.insert(toid,js.dump());
}



//实现添加好友业务的函数
void ChatService::addFriend(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int userid=js["id"].get<int>();
    int friendid=js["friendid"].get<int>();
    friendModel_.insert(userid,friendid);
}



//创建群组
void ChatService::createGroup(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int userid=js["id"].get<int>();
    std::string name=js["groupname"];
    std::string desc=js["groupdesc"];
    Group group{-1,name,desc};

    //创建成功的话,记得把这个用户设为创建者(群主)
    if(groupModel_.createGroup(group))
    {
        groupModel_.joinGroup(userid,group.getId(),"creator");
    }
}



//加入群组
void ChatService::joinGroup(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int userid=js["id"].get<int>();
    int groupid=js["groupid"].get<int>();
    groupModel_.joinGroup(userid,groupid,"normal");
}



//群组聊天
void ChatService::groupChat(const TcpConnectionPtr& conn,
    nlohmann::json& js,TimeStamp timeStamp)
{
    int userid=js["id"].get<int>();
    int groupid=js["groupid"].get<int>();
    const auto userVec=groupModel_.queryGroupUsers(userid,groupid);
    
    std::lock_guard<std::mutex> lock{connMtx_};
    for(int id:userVec)
    {
        //这个情况说明该用户在线而且也确实在同一个服务进程中
        if(connMap_.find(id)!=connMap_.end())  
        {
            connMap_[id]->send(js.dump());
            continue;
        }
        
        User user=userModel_.query(id);

        //这个分支说明这个群友确实在线但在另一个服务进程中
        if(user.getState()=="online")  redis_.publish(id,js.dump());

        //走到这里说明用户离线,存储离线信息
        else  offlineMessageModel_.insert(id,js.dump());
    }
}



//处理客户端断开连接的恢复操作.其实很简单,客户断开连接相当于退出登录
//把用户状态从在线变成离线,包含哈希表删除连接和数据库更新状态两个逻辑
void ChatService::clientCloseReset(const TcpConnectionPtr& conn)
{
    User user;

    //获取用户id,并从连接表中把对应的连接删除掉
    int userId=std::any_cast<int>(conn->getContext());
    {
        std::lock_guard<std::mutex> lock{connMtx_};
        if(connMap_.find(userId)!=connMap_.end())
        {
            connMap_.erase(userId);
            user.setId(userId);            
        }
    }

    //然后再在User表中更新一下状态
    if(user.getId()!=-1)
    {
        user.setState("offline");
        userModel_.updateState(user);
    }

    //不再关注这个用户相关消息
    redis_.unsubscribe(userId);
}



//处理服务端异常断开(ctrl+c)的恢复函数
//实际上就是把所有在线用户的状态在数据库中改成离线
void ChatService::serverCloseExceptionReset()
{
    userModel_.resetState();
}



