#include "../include/rpc_provider.hpp"
#include "../include/tcp_server.h"

class rpcServer{
public:
    explicit rpcServer(int port) : server_(port){
        server_.setRequestCallback([this](TcpServer::ClientId client_id, uint32_t stream_id, 
                                        uint32_t sequence, const std::vector<uint8_t>& payload) {
            handleRequest(client_id, stream_id, sequence, payload);
        });
        server_.setSendBufferSize(64 * 1024);  // 64KB发送缓冲区
        server_.setRecvBufferSize(64 * 1024);  // 64KB接收缓冲区
    }

    void start() {
        server_.start();
    }

private:
    void handleRequest(TcpServer::ClientId client_id, uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>& payload){
        std::string message(payload.begin(), payload.end());
        std::cout << "收到来自客户端 " << client_id << " Stream " << stream_id 
                 << " 序列号 " << sequence << " 的请求：" << message << std::endl;
                 
        std::string response = "服务器响应: " + message;
        std::cout << "发送响应: " << response << std::endl;
        
        server_.response(client_id, stream_id, sequence, std::vector<uint8_t>(response.begin(), response.end()));
    }
    TcpServer server_;
};

int main(){
    rpcServer server(8080);
    server.start();

    std::cin.get();
}