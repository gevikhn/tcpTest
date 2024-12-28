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
        cleanup_thread_ = std::thread([this]() { cleanupInactiveStreams(); });
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

        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }

        // 清理所有流处理器
        {
            std::lock_guard<std::mutex> lock(stream_processors_mutex_);
            stream_processors_.clear();
        }

        // 清理所有客户端
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
        ClientHandler(TcpServer* server, int fd, int id)
            : server_(server), client_fd_(fd), id_(id), running_(true) {
            start();
        }

        void start() {
            if (read_thread_.joinable()) return;
            read_thread_ = std::thread([this]() { handleRead(); });
            write_thread_ = std::thread([this]() { handleWrite(); });
        }

        void stop() {
            running_ = false;
            
            int fd = client_fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }

            write_cv_.notify_all();
            
            if (read_thread_.joinable()) {
                read_thread_.join();
            }

            if (write_thread_.joinable()) {
                write_thread_.join();
            }
        }

        bool write(const std::vector<uint8_t>& data) {
            std::lock_guard<std::mutex> lock(write_mutex_);
            outgoing_queue_.push(data);
            write_cv_.notify_one();
            return true;
        }

        int getId() const { return id_; }
        int getSocket() const { return client_fd_.load(); }

    private:
        void handleRead() {
            std::vector<uint8_t> buffer(server_->read_buffer_size_);
            size_t total_received = 0;

            while (running_) {
                ssize_t bytes_read = recv(client_fd_.load(), buffer.data(), buffer.size(), 0);
                if (bytes_read <= 0) {
                    std::cerr << "客户端 " << id_ << " 接收数据失败或连接关闭" << std::endl;
                    break;
                }

                total_received += bytes_read;
                std::cout << "客户端 " << id_ << " 累计接收 " << total_received << " 字节数据" << std::endl;
                
                frame_buffer_.insert(frame_buffer_.end(), 
                                   buffer.begin(), buffer.begin() + bytes_read);
                
                size_t bytes_consumed;
                std::vector<Frame> frames = server_->processFrames(frame_buffer_, bytes_consumed);
                
                if (bytes_consumed > 0) {
                    frame_buffer_.erase(frame_buffer_.begin(), 
                                      frame_buffer_.begin() + bytes_consumed);
                }

                for (auto& frame : frames) {
                    frame.client_id = id_;
                    if (server_->request_callback_) {
                        server_->request_callback_(id_, frame.stream_id, frame.sequence, frame.payload);
                    }
                }
            }

            server_->handleClientDisconnect(id_);
        }

        void handleWrite() {
            while (running_) {
                std::vector<std::vector<uint8_t>> pending_data;
                {
                    std::unique_lock<std::mutex> lock(write_mutex_);
                    while (running_ && outgoing_queue_.empty()) {
                        write_cv_.wait(lock);
                    }
                    
                    if (!running_) break;

                    while (!outgoing_queue_.empty()) {
                        pending_data.push_back(std::move(outgoing_queue_.front()));
                        outgoing_queue_.pop();
                    }
                }

                for (const auto& data : pending_data) {
                    ssize_t total_sent = 0;
                    while (total_sent < data.size()) {
                        ssize_t sent = ::send(client_fd_.load(), data.data() + total_sent, 
                                            data.size() - total_sent, MSG_NOSIGNAL);
                        if (sent <= 0) {
                            std::cerr << "向客户端 " << id_ << " 发送数据失败" << std::endl;
                            running_ = false;
                            return;
                        }
                        total_sent += sent;
                    }
                    std::cout << "向客户端 " << id_ << " 成功发送 " << total_sent << " 字节数据" << std::endl;
                }
            }
        }

        TcpServer* server_;
        std::atomic<int> client_fd_;
        int id_;
        std::atomic<bool> running_;
        std::thread read_thread_;
        std::thread write_thread_;
        std::mutex write_mutex_;
        std::condition_variable write_cv_;
        std::queue<std::vector<uint8_t>> outgoing_queue_;
        std::vector<uint8_t> frame_buffer_;
    };

    std::vector<Frame> processFrames(std::vector<uint8_t>& buffer, size_t& bytes_consumed) {
        return frame_parser_.processData(buffer, bytes_consumed);
    }

    struct StreamProcessor {
        std::thread worker_thread;
        std::queue<Frame> frame_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::atomic<bool> running{true};
        TcpServer* server;
        uint32_t stream_id;

        StreamProcessor(TcpServer* srv, uint32_t sid) 
            : server(srv), stream_id(sid) {
            worker_thread = std::thread([this]() { processLoop(); });
        }

        ~StreamProcessor() {
            running = false;
            queue_cv.notify_one();
            if (worker_thread.joinable()) {
                worker_thread.join();
            }
        }

        void addFrame(Frame frame) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                frame_queue.push(std::move(frame));
            }
            queue_cv.notify_one();
        }

    private:
        void processLoop() {
            while (running) {
                Frame frame;
                bool has_frame = false;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    if (frame_queue.empty()) {
                        queue_cv.wait_for(lock, std::chrono::milliseconds(100));
                        if (frame_queue.empty()) continue;
                    }
                    frame = std::move(frame_queue.front());
                    frame_queue.pop();
                    has_frame = true;
                }

                if (!has_frame) continue;

                try {
                    bool checksum_valid = frame.verifyChecksum();
                    
                    {
                        std::lock_guard<std::mutex> lock(server->callback_mutex_);
                        if (server->checksum_callback_) {
                            server->checksum_callback_(checksum_valid, frame.stream_id, frame.sequence);
                        }
                        
                        if (!checksum_valid) continue;

                        if (server->request_callback_ && frame.client_id >= 0) {
                            server->request_callback_(frame.client_id, frame.stream_id, 
                                                   frame.sequence, frame.payload);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "处理帧时发生错误: " << e.what() << std::endl;
                }
            }
        }
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

        for (auto& frame : frames) {
            frame.client_id = client_id;  // 设置客户端ID
            uint32_t stream_id = frame.stream_id;
            
            std::shared_ptr<StreamProcessor> processor;
            {
                std::lock_guard<std::mutex> lock(stream_processors_mutex_);
                auto it = stream_processors_.find(stream_id);
                if (it == stream_processors_.end()) {
                    processor = std::make_shared<StreamProcessor>(this, stream_id);
                    stream_processors_[stream_id] = processor;
                } else {
                    processor = it->second;
                }
            }
            
            processor->addFrame(std::move(frame));
        }
    }

    void cleanupInactiveStreams() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(60));  // 每分钟检查一次
            std::vector<uint32_t> streams_to_remove;
            
            {
                std::lock_guard<std::mutex> lock(stream_processors_mutex_);
                for (const auto& pair : stream_processors_) {
                    if (pair.second.use_count() == 1) {  // 只有 map 持有引用
                        streams_to_remove.push_back(pair.first);
                    }
                }
                
                for (uint32_t stream_id : streams_to_remove) {
                    stream_processors_.erase(stream_id);
                }
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
                    handler = std::make_unique<ClientHandler>(this, client_fd, client_fd);
                    clients_[client_fd] = std::move(handler);
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
    std::thread cleanup_thread_;
    std::mutex clients_mutex_;
    std::mutex callback_mutex_;
    std::mutex disconnect_mutex_;
    std::mutex stream_processors_mutex_;
    std::condition_variable disconnect_cv_;
    std::queue<ClientId> disconnect_queue_;
    std::map<ClientId, std::unique_ptr<ClientHandler>> clients_;
    std::map<ClientId, std::vector<uint8_t>> client_buffers_;
    std::map<uint32_t, std::shared_ptr<StreamProcessor>> stream_processors_;
    RequestCallback request_callback_;
    ConnectCallback connect_callback_;
    ChecksumCallback checksum_callback_;
    size_t read_buffer_size_ = 1024;
};
