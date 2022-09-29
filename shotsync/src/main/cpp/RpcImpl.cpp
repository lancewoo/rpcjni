#include <iostream>
#include "RpcImpl.h"

using namespace std;
namespace client {
    rpc_handler_t handler;

    void ClientSetMessageHandler(rpc_handler_t userHandler) {
        RPC_INFO("ClientSetMessageHandler userHandler:%p", userHandler);
        handler = userHandler;
    }
}