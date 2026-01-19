#include "MpRpcChannel.h"
#include "MpRpcApplication.h"
#include "MpRpcController.h"
#include "../user.pb.h"
#include "Logging.h"
#include <unistd.h>


void loginService(fixbug::UserServiceRpc_Stub* stub
    ,MpRpcController* controller)
{
    //发送rpc请求,肯定要告诉对方:服务方法名是啥,参数是啥,返回类型是啥
    //参数都是放在request数据结构上的,返回结果response肯定一开始是空的,
    //只有在rpc响应完成后才会填充它.
    fixbug::LoginRequest request;
    request.set_name("syj");
    request.set_pwd("Syj139115!");
    fixbug::LoginResponse response;
    
    //实际上stub调用Login函数,底层调用的正是RpcChannel对象的CallMethod方法
    //而CallMethod方法会将request结构的数据序列化成字符流(rpc请求)发送
    //出去,然后一直等待rpc响应.最后根据rpc响应填充response结构的数据.

    //如下所示,别忘了UserServiceRpc_Stub类是继承的UserServiceRpc类,所以
    //stub当然认识Login方法,也会有这个方法对应的方法描述符指针.
    //而stub内部还有一个RpcChannel对象的指针channel_,那么stub就会把方法
    //描述符指针作为channel_的CallMethod函数第一个参数,这样Channel对象
    //也会知道自己调用的是哪一个方法.
    
    /*
        void UserServiceRpc_Stub::Login(RpcController* controller,
                    const fixbug::LoginRequest* request,
                    fixbug::LoginResponse* response,
                    google::protobuf::Closure* done) 
        {
            channel_->CallMethod(descriptor()->method(0),
                    controller,request,response,done);
        }
    */
    stub->Login(controller,&request,&response,nullptr);
    
    if(controller->Failed())  LOG_ERROR<<controller->ErrorText();
    else
    {
        if(response.result().errcode()==0)
        {
            LOG_INFO<<"rpc Login response success:"<<response.success();
        }
        else  LOG_ERROR<<"rpc response error:"<<response.result().errcode();
    }
}



#include <iostream>

int main(int argc,char* argv[])
{
    std::cout << "Client starting..." << std::endl;
    //加载文件配置读到内存(哈希表)
    MpRpcApplication::getInstance().Init(argc,argv);
    
    //应用程序发布rpc请求的时候就是这样使用的
    MpRpcChannel channel;
    fixbug::UserServiceRpc_Stub stub{&channel};
    MpRpcController controller;

    int total_success = 0;
    int total_failure = 0;

    // Verify Consistent Hashing
    LOG_INFO << "=== Phase 1: Consistent Hashing Verification ===";
    std::vector<std::string> users = {"user_A", "user_B", "user_C", "user_A"}; // A again to verify consistency
    
    for (const auto& user : users) {
        LOG_INFO << "--- Requesting for " << user << " ---";
        for (int i = 0; i < 3; ++i) { // 3 calls per user to show stability
            fixbug::LoginRequest req;
            req.set_name(user);
            req.set_pwd("123456");
            
            fixbug::LoginResponse rsp;
            
            // Stub calls channel->CallMethod
            stub.Login(&controller, &req, &rsp, nullptr);
            
            if (controller.Failed()) {
                LOG_ERROR << "Rpc Call Failed: " << controller.ErrorText();
                controller.Reset();
                total_failure++;
            } else {
                if (rsp.result().errcode() == 0) {
                    LOG_INFO << "Rpc Login response success:" << rsp.success();
                    total_success++;
                } else {
                    LOG_ERROR << "Rpc Login response error:" << rsp.result().errmsg();
                    total_failure++;
                }
            }
            usleep(100000); // 100ms
        }
        sleep(1); // Gap between users
    }

    LOG_INFO << "Phase 1 Stats -> Success: " << total_success << ", Failure: " << total_failure;

    // Verify Rate Limiting (Spamming)
    LOG_INFO << "=== Phase 2: Rate Limiting Verification (Spamming) ===";
    // We try to hit the provider with Rate Limit (8003). 
    // Since we don't know which user maps to 8003, we just spam a bit.
    // Or we can rely on previous logs to see which one mapped to 8003.
    // But for automation, let's just pick "user_B" (or whoever) and spam.
    std::string target_user = "user_B"; 
    LOG_INFO << "--- Spamming requests for " << target_user << " ---";
    int p2_success = 0;
    int p2_fail = 0;
    for (int i = 0; i < 10; ++i) {
        fixbug::LoginRequest req;
        req.set_name(target_user);
        req.set_pwd("123456");
        fixbug::LoginResponse rsp;
        stub.Login(&controller, &req, &rsp, nullptr);
        
        if (controller.Failed()) {
             // This might be due to Rate Limit closing connection
             LOG_WARN << "Rpc Call Failed (Expected if Rate Limited): " << controller.ErrorText();
             controller.Reset();
             total_failure++;
             p2_fail++;
        } else {
             LOG_INFO << "Success";
             total_success++;
             p2_success++;
        }
        usleep(50000); // 50ms interval (20 QPS) -> Should trigger 1 QPS limit
    }
    LOG_INFO << "Phase 2 Stats -> Success: " << p2_success << ", Failure: " << p2_fail;
    
    // Verify Circuit Breaker
    LOG_INFO << "=== Phase 3: Circuit Breaker Verification ===";
    LOG_INFO << "Please manually kill a provider (e.g., 8002) now. Waiting 5s...";
    sleep(5);
    
    int p3_success = 0;
    int p3_fail = 0;
    for (int i = 0; i < 20; ++i) {
        fixbug::LoginRequest req;
        req.set_name("user_A"); // Assuming user_A mapped to 8002? 
        // We will rotate users to hit the broken node
        req.set_name("user_" + std::to_string(i % 3)); 
        
        req.set_pwd("123456");
        fixbug::LoginResponse rsp;
        stub.Login(&controller, &req, &rsp, nullptr);
        
        if (controller.Failed()) {
            LOG_ERROR << "Rpc Call Failed: " << controller.ErrorText();
            controller.Reset();
            total_failure++;
            p3_fail++;
        } else {
            LOG_INFO << "Success";
            total_success++;
            p3_success++;
        }
        usleep(500000); // 500ms
    }
    LOG_INFO << "Phase 3 Stats -> Success: " << p3_success << ", Failure: " << p3_fail;

    std::cout << "\n========================================" << std::endl;
    std::cout << "TOTAL STATISTICS" << std::endl;
    std::cout << "Total Requests: " << (total_success + total_failure) << std::endl;
    std::cout << "Total Success : " << total_success << std::endl;
    std::cout << "Total Failure : " << total_failure << std::endl;
    std::cout << "Success Rate  : " << (total_success * 100.0 / (total_success + total_failure)) << "%" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}

