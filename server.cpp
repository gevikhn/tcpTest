#include <iostream>
#include <thread>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "frame.h"

class StreamManager {
public:
    void sendFrame(const Frame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        outgoing_frames_.push(frame);
        cv_.notify_one();
    }

    Frame receiveFrame() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (incoming_frames_.empty()) {
            cv_.wait(lock);
        }
        Frame frame = incoming_frames_.front();
        incoming_frames_.pop();
        return frame;
    }

    void addIncomingFrame(const Frame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 将frame添加到对应stream的接收队列
        auto& stream_queue = stream_frames_[frame.stream_id];
        stream_queue.insert({frame.sequence, frame});
        
        // 检查是否可以按序处理frame
        while (!stream_queue.empty()) {
            auto it = stream_queue.begin();
            if (it->first == next_sequence_[frame.stream_id]) {
                incoming_frames_.push(it->second);
                stream_queue.erase(it);
                next_sequence_[frame.stream_id]++;
                cv_.notify_one();
            } else {
                break;
            }
        }
    }

    bool hasOutgoingFrames() {
        std::unique_lock<std::mutex> lock(mutex_);
        return !outgoing_frames_.empty();
    }

    Frame getNextOutgoingFrame() {
        std::unique_lock<std::mutex> lock(mutex_);
        Frame frame = outgoing_frames_.front();
        outgoing_frames_.pop();
        return frame;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Frame> incoming_frames_;    // 已排序的frame队列
    std::queue<Frame> outgoing_frames_;    // 待发送的frame队列
    std::map<uint32_t, std::map<uint32_t, Frame>> stream_frames_;  // stream_id -> (sequence -> Frame)
    std::map<uint32_t, uint32_t> next_sequence_;  // 每个stream期望的下一个序列号
};

class Server {
public:
    Server(int port) : port_(port), running_(false) {}

    void start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "Socket创建失败" << std::endl;
            return;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // 设置较小的发送缓冲区
        int send_buf_size = 1024;  // 1KB的发送缓冲区
        if (setsockopt(server_fd_, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size)) < 0) {
            std::cerr << "设置发送缓冲区大小失败" << std::endl;
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Bind失败" << std::endl;
            return;
        }

        if (listen(server_fd_, 3) < 0) {
            std::cerr << "Listen失败" << std::endl;
            return;
        }

        running_ = true;
        std::cout << "服务器启动在端口 " << port_ << std::endl;

        acceptConnections();
    }

private:
    void acceptConnections() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_fd < 0) {
                std::cerr << "Accept失败" << std::endl;
                continue;
            }

            std::cout << "新客户端连接: " << inet_ntoa(client_addr.sin_addr) << std::endl;

            auto stream_manager = std::make_shared<StreamManager>();
            
            // 启动读写线程
            std::thread read_thread(&Server::handleRead, this, client_fd, stream_manager);
            std::thread write_thread(&Server::handleWrite, this, client_fd, stream_manager);
            
            read_thread.detach();
            write_thread.detach();
        }
    }

    void handleRead(int client_fd, std::shared_ptr<StreamManager> stream_manager) {
        std::vector<uint8_t> buffer(1024);
        std::vector<uint8_t> frame_buffer;

        while (running_) {
            ssize_t bytes_read = recv(client_fd, buffer.data(), buffer.size(), 0);
            if (bytes_read <= 0) {
                break;
            }

            frame_buffer.insert(frame_buffer.end(), buffer.begin(), buffer.begin() + bytes_read);

            while (frame_buffer.size() >= Frame::HEADER_SIZE) {
                uint32_t frame_length = (frame_buffer[0] << 24) | (frame_buffer[1] << 16) |
                                      (frame_buffer[2] << 8) | frame_buffer[3];

                if (frame_buffer.size() >= Frame::HEADER_SIZE + frame_length) {
                    std::vector<uint8_t> frame_data(frame_buffer.begin(),
                                                  frame_buffer.begin() + Frame::HEADER_SIZE + frame_length);
                    Frame frame = Frame::deserialize(frame_data);
                    
                    // 处理接收到的frame
                    handleFrame(frame, stream_manager);

                    frame_buffer.erase(frame_buffer.begin(),
                                     frame_buffer.begin() + Frame::HEADER_SIZE + frame_length);
                } else {
                    break;
                }
            }
        }
        close(client_fd);
    }

    void handleWrite(int client_fd, std::shared_ptr<StreamManager> stream_manager) {
        while (running_) {
            if (stream_manager->hasOutgoingFrames()) {
                Frame frame = stream_manager->getNextOutgoingFrame();
                std::vector<uint8_t> data = frame.serialize();
                send(client_fd, data.data(), data.size(), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void handleFrame(const Frame& frame, std::shared_ptr<StreamManager> stream_manager) {
        // 生成一个大的响应数据
        std::string response_data(4096, 'A');  // 4KB的数据
        for (int i = 0; i < 5; ++i) {  // 发送5个大帧
            Frame response;
            response.stream_id = frame.stream_id;
            response.sequence = frame.sequence * 5 + i;  // 使用不同的序列号
            response.payload = std::vector<uint8_t>(response_data.begin(), response_data.end());
            response.length = response.payload.size();
            
            // 添加一些随机延迟，增加乱序的可能性
            if (i % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            stream_manager->sendFrame(response);
        }
    }

    int port_;
    int server_fd_;
    bool running_;
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

    Server server(8080);
    server.start();
    return 0;
}
