#include "../user.pb.h"
#include "Logging.h"
#include "MpRpcApplication.h"
#include "MpRpcProvider.h"
#include "MpRpcController.h"
#include <thread>
#include <chrono>

class UserService
:public fixbug::UserServiceRpc
{
public:
    //做本地业务的函数
    bool Login(std::string_view name,std::string_view pwd)
    {
        LOG_INFO<<"doing local service: Login";
        LOG_INFO<<"name="<<name<<" ,pwd="<<pwd;
        
        // 模拟长尾延迟 (Backup Request 测试)
        if (name == "delay") {
            LOG_INFO << "Simulating slow request (200ms delay)...";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // 环境变量控制延迟 (用于 LALB 测试)
        const char* delay_env = std::getenv("SERVER_DELAY_MS");
        if (delay_env) {
            int delay = std::atoi(delay_env);
            if (delay > 0) {
                 // LOG_INFO << "Server forced delay: " << delay << "ms";
                 std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        
        return true;
    };



    //实际上这个函数是从UserServiceRpc类继承过来的
    void Login(google::protobuf::RpcController* controller,
            const fixbug::LoginRequest* request,
            fixbug::LoginResponse* response,
            google::protobuf::Closure* done)override
    {
        bool enable_log = (std::getenv("ENABLE_ACCESS_LOG") != nullptr);
        // 打印元数据
        if (controller && enable_log) {
            MpRpcController* mpController = dynamic_cast<MpRpcController*>(controller);
            if (mpController) {
                const auto& metadata = mpController->GetAllMetadata();
                LOG_INFO << "Received Metadata:";
                for (const auto& [key, value] : metadata) {
                    LOG_INFO << "  " << key << ": " << value;
                }
            }
        }

        //request数据结构是rpc框架已经反序列化得到的
        std::string name=request->name();
        std::string pwd=request->pwd();
        
        //做本地业务
        bool success=Login(name,pwd);
        
        //response数据结构之前一直都是空的,现在根据本地业务处理的结果
        //对它进行填充
        response->set_success(success);
        fixbug::ResultCode* code=response->mutable_result();
        code->set_errmsg("");
        code->set_errcode(0);

        //最后一步回调函数是必须做的,这里其实就是把response数据结构序列化
        //成字符串并发送给客户端
        done->Run();
    }
};




int main(int argc,char* argv[])
{
    //加载配置文件,把文件数据都读到内存(哈希表)
    MpRpcApplication::getInstance().Init(argc,argv);
    
    // 设置日志级别
    const char* log_level_env = std::getenv("LOG_LEVEL");
    if (log_level_env) {
        std::string level(log_level_env);
        if (level == "TRACE") Logger::setLogLevel(Logger::LogLevel::TRACE);
        else if (level == "DEBUG") Logger::setLogLevel(Logger::LogLevel::DEBUG);
        else if (level == "INFO") Logger::setLogLevel(Logger::LogLevel::INFO);
        else if (level == "WARN") Logger::setLogLevel(Logger::LogLevel::WARN);
        else if (level == "ERROR") Logger::setLogLevel(Logger::LogLevel::ERROR);
        else if (level == "FATAL") Logger::setLogLevel(Logger::LogLevel::FATAL);
    } else {
        Logger::setLogLevel(Logger::LogLevel::INFO);
    }

    //注意,在应用程序享受下层的rpc服务,就是这样调用rpc的
    //先注册所有的服务,再启动tcp服务器去收发数据和做业务处理
    MpRpcProvider provider;
    UserService service;
    provider.notifyService(&service);
    provider.run();

    return 0;
}
