#include "../include/rpc_provider.hpp"
#include "../include/tcp_client.h"

class RpcClient {
public:
    RpcClient() {}

    void sendTestMessages(int thread_id) {
        for (int stream_id = thread_id; stream_id <= thread_id; ++stream_id) {
            std::string message = "来自线程 " + std::to_string(thread_id) + 
                                " Stream " + std::to_string(stream_id) + 
                                " 的消息 #" + std::to_string(1);
            
            std::cout << "正在发送消息: stream_id=" << stream_id 
                      << ", message=" << message << std::endl;
            
            std::vector<uint8_t> payload(message.begin(), message.end());
            int actual_stream_id;
            std::vector<uint8_t> response;
            
            if (client_.sendAndWaitResponse(payload, actual_stream_id, response)) {
                std::string response_str(response.begin(), response.end());
                std::cout << "线程 " << thread_id << " 收到来自Stream " 
                          << stream_id << " 序列号 1 的响应，大小: " 
                          << response.size() << "字节" << response_str << std::endl;
            } else {
                std::cerr << "线程 " << thread_id << " 发送消息或等待响应超时" << std::endl;
            }
            
            // 添加短暂延迟，避免消息太快发送
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void startMultiThreadTest(int num_threads) {
        if(client_.connect("127.0.0.1", 8080)) {
            std::cout << "连接成功，发送数据..." << std::endl;
            
            std::vector<std::thread> threads;
            for (int i = 0; i < num_threads; ++i) {
                threads.emplace_back([this, i]() {
                    sendTestMessages(i + 1);
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }
        }
    }

private:
    TcpClient client_;
};

int main() {
    RpcClient client;
    std::cout << "正在连接服务器..." << std::endl;
    client.startMultiThreadTest(5);
    std::cout << "按回车键退出" << std::endl;
    std::cin.get();
    return 0;
}