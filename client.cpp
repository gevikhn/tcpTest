#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>
#include "frame.h"

class Client {
public:
    Client(const std::string& ip, int port) : ip_(ip), port_(port), running_(false) {}

    bool connect() {
        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) {
            std::cerr << "Socket创建失败" << std::endl;
            return false;
        }

        // 设置较小的接收缓冲区
        int recv_buf_size = 512;  // 512B的接收缓冲区
        if (setsockopt(sock_fd_, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size)) < 0) {
            std::cerr << "设置接收缓冲区大小失败" << std::endl;
            return false;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        
        if (inet_pton(AF_INET, ip_.c_str(), &server_addr.sin_addr) <= 0) {
            std::cerr << "无效的IP地址" << std::endl;
            return false;
        }

        if (::connect(sock_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "连接失败" << std::endl;
            return false;
        }

        running_ = true;
        
        // 启动读写线程
        read_thread_ = std::thread(&Client::handleRead, this);
        write_thread_ = std::thread(&Client::handleWrite, this);

        return true;
    }

    void sendMessage(const std::string& message, uint32_t stream_id) {
        Frame frame;
        frame.stream_id = stream_id;
        frame.payload = std::vector<uint8_t>(message.begin(), message.end());
        frame.length = frame.payload.size();
        
        // 为每个stream分配序列号
        {
            std::unique_lock<std::mutex> lock(sequence_mutex_);
            frame.sequence = next_sequence_[stream_id]++;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        outgoing_frames_.push(frame);
        cv_.notify_one();
    }

    void stop() {
        running_ = false;
        if (read_thread_.joinable()) read_thread_.join();
        if (write_thread_.joinable()) write_thread_.join();
        close(sock_fd_);
    }

private:
    void handleRead() {
        std::vector<uint8_t> buffer(256);  // 使用更小的读取缓冲区
        std::vector<uint8_t> frame_buffer;
        std::map<uint32_t, std::map<uint32_t, Frame>> stream_frames;
        std::map<uint32_t, uint32_t> next_sequence;
        FrameParser parser;
        uint32_t total_frames = 0;
        uint32_t valid_frames = 0;
        uint32_t invalid_frames = 0;

        while (running_) {
            ssize_t bytes_read = recv(sock_fd_, buffer.data(), buffer.size(), 0);
            if (bytes_read <= 0) {
                break;
            }

            // std::cout << "收到 " << bytes_read << " 字节的数据" << std::endl;

            frame_buffer.insert(frame_buffer.end(), buffer.begin(), buffer.begin() + bytes_read);

            size_t bytes_consumed;
            std::vector<Frame> completed_frames = parser.processData(frame_buffer, bytes_consumed);

            if (bytes_consumed > 0) {
                frame_buffer.erase(frame_buffer.begin(), frame_buffer.begin() + bytes_consumed);
            }

            for (const auto& frame : completed_frames) {
                total_frames++;
                
                // 验证校验和
                bool checksum_valid = frame.verifyChecksum();
                if (checksum_valid) {
                    valid_frames++;
                } else {
                    invalid_frames++;
                    std::cout << "警告: Stream " << frame.stream_id 
                             << " 序列号 " << frame.sequence 
                             << " 校验和验证失败" << std::endl;
                    continue;  // 跳过无效帧
                }

                // 将frame添加到对应stream的队列
                auto& stream_queue = stream_frames[frame.stream_id];
                stream_queue.insert({frame.sequence, frame});
                
                // 按序输出消息
                while (!stream_queue.empty()) {
                    auto it = stream_queue.begin();
                    if (it->first == next_sequence[frame.stream_id]) {
                        std::string message(it->second.payload.begin(), it->second.payload.end());
                        std::cout << "收到来自Stream " << it->second.stream_id 
                                << " 序列号 " << it->second.sequence 
                                << " 的消息，大小: " << message.size() << " 字节"
                                << " [校验和: " << (checksum_valid ? "通过" : "失败") << "]" 
                                << std::endl;
                        stream_queue.erase(it);
                        next_sequence[frame.stream_id]++;
                    } else {
                        std::cout << "Stream " << frame.stream_id 
                                << " 等待序列号 " << next_sequence[frame.stream_id] 
                                << "，当前收到 " << it->first << std::endl;
                        break;
                    }
                }
            }
        }

        // 打印统计信息
        std::cout << "\n=== 统计信息 ===" << std::endl;
        std::cout << "总帧数: " << total_frames << std::endl;
        std::cout << "有效帧: " << valid_frames << std::endl;
        std::cout << "无效帧: " << invalid_frames << std::endl;
        if (total_frames > 0) {
            float valid_rate = (float)valid_frames / total_frames * 100;
            std::cout << "有效率: " << valid_rate << "%" << std::endl;
        }
        std::cout << "===============" << std::endl;
    }

    void handleWrite() {
        while (running_) {
            std::unique_lock<std::mutex> lock(mutex_);
            while (outgoing_frames_.empty() && running_) {
                cv_.wait(lock);
            }
            
            if (!running_) break;

            Frame frame = outgoing_frames_.front();
            outgoing_frames_.pop();
            lock.unlock();

            std::vector<uint8_t> data = frame.serialize();
            send(sock_fd_, data.data(), data.size(), 0);
        }
    }

    std::string ip_;
    int port_;
    int sock_fd_;
    bool running_;
    std::thread read_thread_;
    std::thread write_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Frame> outgoing_frames_;
    std::mutex sequence_mutex_;
    std::map<uint32_t, uint32_t> next_sequence_;  // 每个stream的下一个序列号
};

int main(int argc, char* argv[]) {
    bool enable_checksum = false;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--checksum") {
            enable_checksum = true;
            Frame::enable_checksum = true;
            std::cout << "启用校验和验证" << std::endl;
        }
    }

    Client client("127.0.0.1", 8080);
    
    if (!client.connect()) {
        return 1;
    }

    // 创建多个线程，每个线程使用不同的stream ID发送消息
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&client, i]() {
            uint32_t stream_id = i + 1;
            for (int j = 0; j < 5; ++j) {
                std::string message = "来自Stream " + std::to_string(stream_id) + 
                                    " 的消息 #" + std::to_string(j);
                client.sendMessage(message, stream_id);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    // 等待一段时间以确保所有消息都被处理
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    client.stop();
    return 0;
}
