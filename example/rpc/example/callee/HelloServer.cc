#include "../Hello.pb.h"
#include "Logging.h"
#include "MpRpcApplication.h"
#include "MpRpcProvider.h"
#include "MpRpcController.h"
#include <iostream>

class HelloService : public hello::HelloService {
public:
    void SayHello(google::protobuf::RpcController* controller,
                  const hello::HelloRequest* request,
                  hello::HelloResponse* response,
                  google::protobuf::Closure* done) override {
        
        LOG_INFO << "Received SayHello request: name=" << request->name();

        // 打印元数据
        if (controller) {
            MpRpcController* mpController = dynamic_cast<MpRpcController*>(controller);
            if (mpController) {
                const auto& metadata = mpController->GetAllMetadata();
                LOG_INFO << "Received Metadata:";
                for (const auto& [key, value] : metadata) {
                    LOG_INFO << "  " << key << ": " << value;
                }
            }
        }

        std::string reply = "Hello, " + request->name();
        response->set_message(reply);
        
        done->Run();
    }
};

int main(int argc, char* argv[]) {
    MpRpcApplication::getInstance().Init(argc, argv);
    
    MpRpcProvider provider;
    HelloService service;
    provider.notifyService(&service);
    provider.run();

    return 0;
}
