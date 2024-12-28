#include "../include/tcp_server.h"
#include "../include/tcp_client.h"
#include <iostream>
#include <chrono>
#include <random>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

class FrameTestServer {
public:
    explicit FrameTestServer(int port) : server_(port) {
        server_.setRequestCallback([this](TcpServer::ClientId client_id, uint32_t stream_id, 
                                        uint32_t sequence, const std::vector<uint8_t>& payload) {
            handleRequest(client_id, stream_id, sequence, payload);
        });

        server_.setConnectCallback([](TcpServer::ClientId client_id, bool connected) {
            std::cout << "客户端 " << client_id << (connected ? " 连接" : " 断开") << std::endl;
        });

        server_.setChecksumCallback([](bool valid, uint32_t stream_id, uint32_t sequence) {
            std::cout << "Stream " << stream_id << " 序列号 " << sequence 
                     << " 的校验和验证" << (valid ? "通过" : "失败") << std::endl;
        });
    }

    void start() {
        // 设置小的发送缓冲区
        server_.setSendBufferSize(1024);
        server_.start();
    }

private:
    void handleRequest(TcpServer::ClientId client_id, uint32_t stream_id, 
                      uint32_t sequence, const std::vector<uint8_t>& payload) {
        std::string message(payload.begin(), payload.end());
        std::cout << "收到来自客户端 " << client_id << " Stream " << stream_id 
                 << " 序列号 " << sequence << " 的请求：" << message << std::endl;

        // 生成大量响应数据
        std::string response_data(4096, 'A');  // 4KB数据
        for (int i = 0; i < 5; ++i) {
            // 随机延迟
            if (i % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            server_.response(client_id, stream_id, sequence * 5 + i, 
                           std::vector<uint8_t>(response_data.begin(), response_data.end()));
        }
    }

    TcpServer server_;
};

class FrameTestClient {
private:
    TcpClient client_;
    std::atomic<uint32_t> total_responses_{0};
    std::atomic<uint32_t> total_bytes_{0};

public:
    FrameTestClient() {
        client_.setConnectCallback([](bool connected) {
            std::cout << (connected ? "连接到服务器" : "与服务器断开连接") << std::endl;
        });

        client_.setChecksumCallback([](bool valid, uint32_t stream_id, uint32_t sequence) {
            std::cout << "Stream " << stream_id << " 序列号 " << sequence 
                     << " 的校验和验证" << (valid ? "通过" : "失败") << std::endl;
        });
    }

    bool connect(const std::string& ip, int port) {
        client_.setRecvBufferSize(512);
        client_.setReadBufferSize(256);
        return client_.connect(ip, port);
    }

    void sendTestMessages(int thread_id) {
        for (int stream_id = thread_id; stream_id <= thread_id + 2; ++stream_id) {
            for (int seq = 0; seq < 5; ++seq) {
                std::string message = "来自线程 " + std::to_string(thread_id) + 
                                    " Stream " + std::to_string(stream_id) + 
                                    " 的消息 #" + std::to_string(seq);
                std::vector<uint8_t> payload(message.begin(), message.end());

                client_.request(stream_id, seq, payload, 
                    [this, thread_id, stream_id, seq]
                    (uint32_t resp_stream_id, uint32_t resp_seq, const std::vector<uint8_t>& resp_payload) {
                        total_responses_++;
                        total_bytes_ += resp_payload.size();
                        
                        std::cout << "线程 " << thread_id << " 收到来自Stream " << resp_stream_id 
                                << " 序列号 " << resp_seq 
                                << " 的响应，大小: " << resp_payload.size() << " 字节" 
                                << std::endl;

                        uint32_t current_responses = total_responses_.load();
                        if (current_responses % 10 == 0) {
                            std::cout << "\n=== 统计信息 ===" << std::endl;
                            std::cout << "总响应数: " << current_responses << std::endl;
                            std::cout << "总字节数: " << total_bytes_.load() << std::endl;
                            std::cout << "平均响应大小: " << (float)total_bytes_ / current_responses << " 字节" << std::endl;
                            std::cout << "===============" << std::endl;
                        }
                    });
                
                // 添加小延迟避免请求过于密集
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void startMultiThreadTest(int num_threads) {
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
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <server|client> [port] [--checksum]" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    int port = 8080;  // 默认端口
    bool enable_checksum = false;

    // 解析参数
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--checksum") {
            enable_checksum = true;
            Frame::enable_checksum = true;
            std::cout << "启用校验和验证" << std::endl;
        } else {
            try {
                port = std::stoi(arg);
            } catch (const std::exception& e) {
                std::cout << "无效的端口号: " << arg << std::endl;
                return 1;
            }
        }
    }

    if (mode == "server") {
        FrameTestServer server(port);
        server.start();
        std::cout << "服务器启动在端口 " << port << std::endl;
        std::cout << "按回车键退出..." << std::endl;
        std::cin.get();
    } else if (mode == "client") {
        FrameTestClient client;
        if (client.connect("127.0.0.1", port)) {
            std::cout << "开始发送测试消息..." << std::endl;
            client.startMultiThreadTest(3);  // 启动3个线程
            std::cout << "测试完成" << std::endl;
        } else {
            std::cout << "连接服务器失败" << std::endl;
            return 1;
        }
    } else {
        std::cout << "无效的模式，请使用 server 或 client" << std::endl;
        return 1;
    }

    return 0;
}
