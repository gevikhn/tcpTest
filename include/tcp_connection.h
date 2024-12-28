#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

class TcpConnection {
public:
    TcpConnection() : sock_fd_(-1), running_(false) {}
    virtual ~TcpConnection() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false)) {
            int fd = sock_fd_.exchange(-1);
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    // 设置socket选项
    bool setSockOpt(int level, int optname, const void* optval, socklen_t optlen) {
        int fd = sock_fd_.load();
        if (fd < 0) return false;
        return setsockopt(fd, level, optname, optval, optlen) == 0;
    }

    // 获取socket选项
    bool getSockOpt(int level, int optname, void* optval, socklen_t* optlen) {
        int fd = sock_fd_.load();
        if (fd < 0) return false;
        return getsockopt(fd, level, optname, optval, optlen) == 0;
    }

    // 设置发送缓冲区大小
    bool setSendBufferSize(int size) {
        return setSockOpt(SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }

    // 设置接收缓冲区大小
    bool setRecvBufferSize(int size) {
        return setSockOpt(SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }

    // 获取发送缓冲区大小
    int getSendBufferSize() {
        int size;
        socklen_t len = sizeof(size);
        if (getSockOpt(SOL_SOCKET, SO_SNDBUF, &size, &len)) {
            return size;
        }
        return -1;
    }

    // 获取接收缓冲区大小
    int getRecvBufferSize() {
        int size;
        socklen_t len = sizeof(size);
        if (getSockOpt(SOL_SOCKET, SO_RCVBUF, &size, &len)) {
            return size;
        }
        return -1;
    }

protected:
    std::atomic<int> sock_fd_;
    std::atomic<bool> running_;
    mutable std::mutex mutex_;  // 用于保护其他可能的共享资源
};
