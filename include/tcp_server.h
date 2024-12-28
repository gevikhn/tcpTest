#pragma once
#include "tcp_connection.h"
#include "frame_handler.h"
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <iostream>
#include <condition_variable>
#include <queue>

class TcpServer : public TcpConnection, public FrameHandler {
public:
    using ClientId = int;
    using RequestCallback = std::function<void(ClientId client_id, uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>&)>;
    using ConnectCallback = std::function<void(ClientId, bool)>;
    using ChecksumCallback = std::function<void(bool valid, uint32_t stream_id, uint32_t sequence)>;

    explicit TcpServer(int port) : port_(port), sock_fd_(-1), running_(false) {
        setFrameCallback([this](const Frame& frame) {});
    }

    ~TcpServer() {
        stop();
    }

    bool start() {
        if (running_) return false;
        
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }

        sock_fd_.store(fd);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_);

        if (::bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(fd);
            sock_fd_.store(-1);
            return false;
        }

        if (listen(fd, 5) < 0) {
            close(fd);
            sock_fd_.store(-1);
            return false;
        }

        running_ = true;
        disconnect_thread_ = std::thread([this]() { disconnectWorker(); });
        accept_thread_ = std::thread([this]() { acceptLoop(); });
        return true;
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }

        int fd = sock_fd_.exchange(-1);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }

        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        disconnect_cv_.notify_all();
        if (disconnect_thread_.joinable()) {
            disconnect_thread_.join();
        }

        std::vector<std::unique_ptr<ClientHandler>> clients_to_close;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto& pair : clients_) {
                clients_to_close.push_back(std::move(pair.second));
            }
            clients_.clear();
            client_buffers_.clear();
        }
        clients_to_close.clear();
    }

    void response(ClientId client_id, uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>& payload) {
        auto frame = createFrame(stream_id, sequence, payload);
        frame.calculateChecksum();
        auto frame_data = frame.serialize();

        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            it->second->write(frame_data);
        }
    }

    void setRequestCallback(RequestCallback cb) {
        request_callback_ = std::move(cb);
    }

    void setConnectCallback(ConnectCallback cb) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connect_callback_ = std::move(cb);
    }

    void setChecksumCallback(ChecksumCallback cb) {
        checksum_callback_ = std::move(cb);
    }

    void setReadBufferSize(size_t size) {
        read_buffer_size_ = size;
    }

protected:
    class ClientHandler {
    public:
        ClientHandler(int client_fd, TcpServer* server) 
            : client_fd_(client_fd), server_(server), running_(true) {}

        ~ClientHandler() {
            stop();
        }

        void start() {
            if (read_thread_.joinable()) return;
            read_thread_ = std::thread([this]() { readLoop(); });
        }

        void stop() {
            bool expected = true;
            if (!running_.compare_exchange_strong(expected, false)) {
                return;
            }

            int fd = client_fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }

            if (read_thread_.joinable()) {
                read_thread_.join();
            }
        }

        bool write(const std::vector<uint8_t>& data) {
            int fd = client_fd_.load();
            if (fd < 0 || !running_) return false;

            std::lock_guard<std::mutex> lock(write_mutex_);
            size_t total_sent = 0;
            while (total_sent < data.size() && running_) {
                ssize_t sent = ::send(fd, data.data() + total_sent, 
                                    data.size() - total_sent, MSG_NOSIGNAL);
                if (sent <= 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    return false;
                }
                total_sent += sent;
            }
            return total_sent == data.size();
        }

    private:
        void readLoop() {
            std::vector<uint8_t> buffer(server_->read_buffer_size_);
            
            while (running_) {
                int fd = client_fd_.load();
                if (fd < 0) break;

                ssize_t bytes_read = recv(fd, buffer.data(), buffer.size(), 0);
                if (bytes_read <= 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    break;
                }

                if (server_ && running_) {
                    try {
                        std::vector<uint8_t> data(buffer.begin(), buffer.begin() + bytes_read);
                        server_->processIncomingData(fd, std::move(data));
                    } catch (const std::exception& e) {
                        std::cerr << "处理数据时发生错误: " << e.what() << std::endl;
                        break;
                    }
                }
            }

            if (server_) {
                int fd = client_fd_.load();
                if (fd >= 0) {
                    server_->scheduleClientDisconnect(fd);
                }
            }
        }

        std::atomic<int> client_fd_;
        TcpServer* server_;
        std::thread read_thread_;
        std::atomic<bool> running_;
        std::mutex write_mutex_;
    };

    void disconnectWorker() {
        while (running_) {
            ClientId client_id = 0;
            {
                std::unique_lock<std::mutex> lock(disconnect_mutex_);
                if (disconnect_queue_.empty()) {
                    disconnect_cv_.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                client_id = disconnect_queue_.front();
                disconnect_queue_.pop();
            }

            ConnectCallback callback_copy;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback_copy = connect_callback_;
            }

            if (callback_copy) {
                callback_copy(client_id, false);
            }

            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.erase(client_id);
                client_buffers_.erase(client_id);
            }
        }
    }

    void scheduleClientDisconnect(ClientId client_id) {
        std::lock_guard<std::mutex> lock(disconnect_mutex_);
        disconnect_queue_.push(client_id);
        disconnect_cv_.notify_one();
    }

    void handleClientDisconnect(ClientId client_id) {
        ConnectCallback callback_copy;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_copy = connect_callback_;
        }
        
        if (callback_copy) {
            callback_copy(client_id, false);
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.erase(client_id);
            client_buffers_.erase(client_id);
        }
    }

    void processIncomingData(ClientId client_id, std::vector<uint8_t> data) {
        std::vector<Frame> frames;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (clients_.find(client_id) == clients_.end()) {
                return;
            }
            
            auto& client_buffer = client_buffers_[client_id];
            client_buffer.insert(client_buffer.end(), data.begin(), data.end());
            
            size_t bytes_consumed;
            frames = frame_parser_.processData(client_buffer, bytes_consumed);
            
            if (bytes_consumed > 0) {
                client_buffer.erase(client_buffer.begin(), client_buffer.begin() + bytes_consumed);
            }
        }

        for (const auto& frame : frames) {
            try {
                bool checksum_valid = frame.verifyChecksum();
                
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (checksum_callback_) {
                        checksum_callback_(checksum_valid, frame.stream_id, frame.sequence);
                    }
                    
                    if (!checksum_valid) continue;

                    if (request_callback_) {
                        request_callback_(client_id, frame.stream_id, frame.sequence, frame.payload);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "处理帧时发生错误: " << e.what() << std::endl;
            }
        }
    }

    void acceptLoop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            
            int server_fd = sock_fd_.load();
            if (server_fd < 0) break;

            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                break;
            }

            try {
                std::unique_ptr<ClientHandler> handler;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    handler = std::make_unique<ClientHandler>(client_fd, this);
                    clients_[client_fd] = std::move(handler);
                    clients_[client_fd]->start();
                }

                ConnectCallback callback_copy;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback_copy = connect_callback_;
                }
                
                if (callback_copy) {
                    callback_copy(client_fd, true);
                }
            } catch (const std::exception& e) {
                std::cerr << "创建客户端处理器时发生错误: " << e.what() << std::endl;
                close(client_fd);
            }
        }
    }

private:
    int port_;
    std::atomic<int> sock_fd_;
    std::atomic<bool> running_;
    std::thread accept_thread_;
    std::thread disconnect_thread_;
    std::mutex clients_mutex_;
    std::mutex callback_mutex_;
    std::mutex disconnect_mutex_;
    std::condition_variable disconnect_cv_;
    std::queue<ClientId> disconnect_queue_;
    std::map<ClientId, std::unique_ptr<ClientHandler>> clients_;
    std::map<ClientId, std::vector<uint8_t>> client_buffers_;
    RequestCallback request_callback_;
    ConnectCallback connect_callback_;
    ChecksumCallback checksum_callback_;
    size_t read_buffer_size_ = 1024;
};
