#pragma once
#include "tcp_connection.h"
#include "frame_handler.h"
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>
#include <future>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "frame.h"

class TcpClient {
public:
    using ResponseCallback = std::function<void(int stream_id, int sequence, const std::vector<uint8_t>& data)>;

    TcpClient() : socket_(-1), running_(false), next_stream_id_(0) {}

    ~TcpClient() {
        disconnect();
    }

    bool connect(const std::string& host, int port) {
        if (socket_ >= 0) {
            return false;
        }

        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) {
            std::cerr << "创建socket失败" << std::endl;
            return false;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
            std::cerr << "地址转换失败" << std::endl;
            close(socket_);
            socket_ = -1;
            return false;
        }

        std::cout << "正在连接服务器..." << std::endl;
        if (::connect(socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "连接服务器失败" << std::endl;
            close(socket_);
            socket_ = -1;
            return false;
        }

        std::cout << "连接到服务器" << std::endl;
        running_ = true;
        read_thread_ = std::thread(&TcpClient::handleRead, this);
        return true;
    }

    void disconnect() {
        running_ = false;
        
        if (socket_ >= 0) {
            shutdown(socket_, SHUT_RDWR);
            close(socket_);
            socket_ = -1;
        }

        if (read_thread_.joinable()) {
            read_thread_.join();
        }

        // 清理所有等待的响应
        {
            std::lock_guard<std::mutex> lock(response_handlers_mutex_);
            response_handlers_.clear();
        }
    }

    // 发送消息并等待响应
    bool sendAndWaitResponse(const std::vector<uint8_t>& payload, int& stream_id,
                           std::vector<uint8_t>& response, int timeout_ms = 5000) {
        stream_id = getNextStreamId();
        
        // 创建promise和future用于等待响应
        auto promise_ptr = std::make_shared<std::promise<std::vector<uint8_t>>>();
        auto future = promise_ptr->get_future();
        
        // 注册响应处理器
        {
            std::lock_guard<std::mutex> lock(response_handlers_mutex_);
            response_handlers_[stream_id] = [promise_ptr](const std::vector<uint8_t>& data) {
                promise_ptr->set_value(data);
            };
        }
        
        // 发送消息
        Frame frame;
        frame.stream_id = stream_id;
        frame.sequence = 1;
        frame.payload = payload;
        frame.length = payload.size();
        frame.calculateChecksum();
        
        std::vector<uint8_t> data = frame.serialize();
        if (!write(data)) {
            std::lock_guard<std::mutex> lock(response_handlers_mutex_);
            response_handlers_.erase(stream_id);
            return false;
        }
        
        // 等待响应
        auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
        if (status == std::future_status::timeout) {
            std::lock_guard<std::mutex> lock(response_handlers_mutex_);
            response_handlers_.erase(stream_id);
            return false;
        }
        
        response = future.get();
        return true;
    }

protected:
    bool write(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            ssize_t sent = ::send(socket_, data.data() + total_sent, 
                                data.size() - total_sent, MSG_NOSIGNAL);
            if (sent <= 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total_sent += sent;
        }
        
        std::cout << "成功发送 " << total_sent << " 字节数据" << std::endl;
        return true;
    }

    void handleRead() {
        std::vector<uint8_t> buffer(4096);
        size_t total_received = 0;
        std::vector<uint8_t> frame_buffer;
        FrameParser frame_parser;

        while (running_) {
            ssize_t bytes_read = recv(socket_, buffer.data(), buffer.size(), 0);
            if (bytes_read <= 0) {
                if (errno == EINTR) continue;
                break;
            }

            total_received += bytes_read;
            std::cout << "累计接收 " << total_received << " 字节数据" << std::endl;

            frame_buffer.insert(frame_buffer.end(), buffer.begin(), buffer.begin() + bytes_read);

            size_t bytes_consumed;
            auto frames = frame_parser.processData(frame_buffer, bytes_consumed);

            if (bytes_consumed > 0) {
                frame_buffer.erase(frame_buffer.begin(), frame_buffer.begin() + bytes_consumed);
            }

            for (const auto& frame : frames) {
                std::lock_guard<std::mutex> lock(response_handlers_mutex_);
                auto it = response_handlers_.find(frame.stream_id);
                if (it != response_handlers_.end()) {
                    it->second(frame.payload);
                    response_handlers_.erase(it);
                }
            }
        }
    }

    int getNextStreamId() {
        return next_stream_id_++;
    }

private:
    std::atomic<int> socket_;
    std::atomic<bool> running_;
    std::thread read_thread_;
    std::mutex write_mutex_;
    std::mutex response_handlers_mutex_;
    std::map<int, std::function<void(const std::vector<uint8_t>&)>> response_handlers_;
    std::atomic<int> next_stream_id_;
};
