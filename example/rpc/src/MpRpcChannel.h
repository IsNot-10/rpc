#pragma once

#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <string>
#include <vector>

// Forward declarations
namespace google {
namespace protobuf {
class RpcController;
class Closure;
}
}

enum class CHANNEL_CODE
{
    SUCCESS,
    PACKAGE_ERR,
    SEND_ERR,
    RECEIVE_ERR,
    getServiceAddr_ERR
};

class MpRpcChannel : public google::protobuf::RpcChannel
{
public:
    void CallMethod(const google::protobuf::MethodDescriptor* methodDesc,
            google::protobuf::RpcController* controller,
            const google::protobuf::Message* request,
            google::protobuf::Message* response,
            google::protobuf::Closure* done) override;
    
private:
    CHANNEL_CODE packageRpcRequest(std::string* send_str,
            const google::protobuf::MethodDescriptor* methodDesc,
            google::protobuf::RpcController* controller,
            const google::protobuf::Message* request);
    
    CHANNEL_CODE receiveRpcResponse(const int connfd,
            google::protobuf::Message* response,
            google::protobuf::RpcController* controller,
            int timeout_ms = 5000);
};
