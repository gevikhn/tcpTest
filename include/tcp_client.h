#pragma once
#include "tcp_connection.h"
#include "frame_handler.h"
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <map>

class TcpClient : public TcpConnection, public FrameHandler {
public:
    using ResponseCallback = std::function<void(uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>&)>;
    using ConnectCallback = std::function<void(bool)>;
    using ChecksumCallback = std::function<void(bool valid, uint32_t stream_id, uint32_t sequence)>;

    TcpClient() {
        setFrameCallback([this](const Frame& frame) {
            auto it = pending_requests_.find(frame.stream_id);
            if (it != pending_requests_.end()) {
                auto& callbacks = it->second;
                auto callback_it = callbacks.find(frame.sequence / 5);
                if (callback_it != callbacks.end()) {
                    callback_it->second(frame.stream_id, frame.sequence, frame.payload);
                }
            }
        });
    }

    ~TcpClient() {
        stop();
    }

    bool connect(const std::string& ip, int port) {
        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) {
            return false;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
            close(sock_fd_);
            return false;
        }

        if (::connect(sock_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock_fd_);
            return false;
        }

        running_ = true;
        read_thread_ = std::thread(&TcpClient::handleRead, this);
        write_thread_ = std::thread(&TcpClient::handleWrite, this);

        if (connect_callback_) {
            connect_callback_(true);
        }

        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (sock_fd_ >= 0) {
                close(sock_fd_);
                sock_fd_ = -1;
            }
            
            cv_.notify_all();
            
            if (read_thread_.joinable()) {
                read_thread_.join();
            }
            if (write_thread_.joinable()) {
                write_thread_.join();
            }
        }
    }

    // 发送请求并设置回调处理响应
    void request(uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>& payload, 
                ResponseCallback callback) {
        auto frame = createFrame(stream_id, sequence, payload);
        frame.calculateChecksum();
        
        std::vector<uint8_t> frame_data = frame.serialize();
        
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pending_requests_[stream_id][sequence] = std::move(callback);
            outgoing_queue_.push(std::move(frame_data));
        }
        cv_.notify_one();
    }

    void setConnectCallback(ConnectCallback cb) {
        connect_callback_ = std::move(cb);
    }

    void setChecksumCallback(ChecksumCallback cb) {
        checksum_callback_ = std::move(cb);
    }

    void setReadBufferSize(size_t size) {
        read_buffer_size_ = size;
    }

private:
    void handleRead() {
        std::vector<uint8_t> buffer(read_buffer_size_);

        while (running_) {
            ssize_t bytes_read = recv(sock_fd_, buffer.data(), buffer.size(), 0);
            if (bytes_read <= 0) {
                if (connect_callback_) {
                    connect_callback_(false);
                }
                break;
            }

            processIncomingData({buffer.begin(), buffer.begin() + bytes_read});
        }
    }

    void handleWrite() {
        while (running_) {
            std::unique_lock<std::mutex> lock(mutex_);
            while (running_ && outgoing_queue_.empty()) {
                cv_.wait(lock);
            }
            
            if (!running_) break;

            auto data = std::move(outgoing_queue_.front());
            outgoing_queue_.pop();
            lock.unlock();

            ::send(sock_fd_, data.data(), data.size(), 0);
        }
    }

    void processIncomingData(const std::vector<uint8_t>& data) {
        frame_buffer_.insert(frame_buffer_.end(), data.begin(), data.end());
        
        size_t bytes_consumed;
        std::vector<Frame> frames = frame_parser_.processData(frame_buffer_, bytes_consumed);
        
        if (bytes_consumed > 0) {
            frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + bytes_consumed);
        }

        for (const auto& frame : frames) {
            bool checksum_valid = frame.verifyChecksum();
            if (checksum_callback_) {
                checksum_callback_(checksum_valid, frame.stream_id, frame.sequence);
            }
            if (!checksum_valid) {
                continue;
            }

            auto it = pending_requests_.find(frame.stream_id);
            if (it != pending_requests_.end()) {
                auto& callbacks = it->second;
                auto callback_it = callbacks.find(frame.sequence / 5);
                if (callback_it != callbacks.end()) {
                    callback_it->second(frame.stream_id, frame.sequence, frame.payload);
                }
            }
        }
    }

    std::thread read_thread_;
    std::thread write_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::vector<uint8_t>> outgoing_queue_;
    std::map<uint32_t, std::map<uint32_t, ResponseCallback>> pending_requests_;
    ConnectCallback connect_callback_;
    ChecksumCallback checksum_callback_;
    size_t read_buffer_size_ = 1024;
};
