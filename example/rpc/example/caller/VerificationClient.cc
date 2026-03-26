#include <iostream>
#include <string>
#include <vector>
#include <getopt.h>
#include <google/protobuf/util/json_util.h>
#include "MpRpcApplication.h"
#include "MpRpcChannel.h"
#include "MpRpcController.h"
#include "user.pb.h"

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " -m <method> -d <json_data> [-s <service_name>]\n"
              << "Example:\n"
              << "  " << prog << " -m Login -d '{\"name\":\"test\", \"pwd\":\"123456\"}'\n";
}

int main(int argc, char** argv) {
    std::string method_name;
    std::string json_data;
    std::string service_name = "UserServiceRpc";
    std::string config_file = "rpc_config.xml";

    int opt;
    while ((opt = getopt(argc, argv, "m:d:s:i:h")) != -1) {
        switch (opt) {
            case 'm': method_name = optarg; break;
            case 'd': json_data = optarg; break;
            case 's': service_name = optarg; break;
            case 'i': config_file = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (method_name.empty() || json_data.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Initialize RPC
    // Reset getopt state because we used it above
    optind = 1;
    char* init_argv[] = { argv[0], (char*)"-i", (char*)config_file.c_str(), nullptr };
    MpRpcApplication::getInstance().Init(3, init_argv);

    // Look up service and method
    const google::protobuf::ServiceDescriptor* serviceDesc = fixbug::UserServiceRpc::descriptor();
    if (!serviceDesc) {
        std::cerr << "Service descriptor not found!" << std::endl;
        return 1;
    }
    
    // If user specified a different service name (though we only link UserServiceRpc here effectively)
    if (service_name != serviceDesc->name()) {
         // In a real generic client we would look up by name from a pool, but here we are hardcoded to UserServiceRpc
         std::cerr << "Warning: This client is compiled for UserServiceRpc. Assuming you meant that." << std::endl;
    }

    const google::protobuf::MethodDescriptor* methodDesc = serviceDesc->FindMethodByName(method_name);
    if (!methodDesc) {
        std::cerr << "Method '" << method_name << "' not found in service '" << serviceDesc->name() << "'!" << std::endl;
        std::cerr << "Available methods: ";
        for (int i = 0; i < serviceDesc->method_count(); ++i) {
            std::cerr << serviceDesc->method(i)->name() << " ";
        }
        std::cerr << std::endl;
        return 1;
    }

    // Create Request and Response
    const google::protobuf::Descriptor* requestDescriptor = methodDesc->input_type();
    const google::protobuf::Descriptor* responseDescriptor = methodDesc->output_type();
    
    const google::protobuf::Message* requestPrototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(requestDescriptor);
    const google::protobuf::Message* responsePrototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(responseDescriptor);
    
    if (!requestPrototype || !responsePrototype) {
        std::cerr << "Failed to get message prototypes!" << std::endl;
        return 1;
    }

    std::unique_ptr<google::protobuf::Message> request(requestPrototype->New());
    std::unique_ptr<google::protobuf::Message> response(responsePrototype->New());
    
    // Parse JSON to Request
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = true;
    auto status = google::protobuf::util::JsonStringToMessage(json_data, request.get(), options);
    if (!status.ok()) {
        std::cerr << "Failed to parse JSON: " << status.ToString() << std::endl;
        return 1;
    }

    // Call RPC
    MpRpcChannel channel;
    MpRpcController controller;
    
    channel.CallMethod(methodDesc, &controller, request.get(), response.get(), nullptr);
    
    if (controller.Failed()) {
        std::cerr << "RPC Failed: " << controller.ErrorText() << std::endl;
        return 1;
    }
    
    // Print Response as JSON
    std::string response_json;
    google::protobuf::util::MessageToJsonString(*response, &response_json);
    std::cout << response_json << std::endl;

    return 0;
}
