#include "RpcImpl.h"

namespace client {
    rpc_handler_t handler;

    void ClientSetMessageHandler(rpc_handler_t userHandler) {
        handler = userHandler;
    }
}