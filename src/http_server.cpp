#include <nghttp2/nghttp2.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <string>
#include <map>
#include <memory>
#include <functional>

const int PORT = 8080;
const int BACKLOG = 5;

// HTTP2会话数据结构
struct Http2Session {
    nghttp2_session* session;
    int fd;
    std::map<int32_t, std::string> stream_data;  // 每个流的数据
};

// 发送回调
static ssize_t send_callback(nghttp2_session* session, const uint8_t* data,
                           size_t length, int flags, void* user_data) {
    Http2Session* http2_session = static_cast<Http2Session*>(user_data);
    ssize_t rv = write(http2_session->fd, data, length);
    if (rv < 0) {
        std::cerr << "发送数据失败: " << strerror(errno) << std::endl;
    }
    std::cout << "发送数据长度: " << rv << std::endl;
    return rv;
}

// 处理POST请求的回调函数
static int handle_post_data(nghttp2_session* session, int32_t stream_id,
                          const std::string& data, void* user_data) {
    std::cout << "收到POST数据: " << data << std::endl;
    std::string response = "{\"status\":\"success\",\"message\":\"数据已接收\"}";
    
    // 创建响应头
    std::string content_length = std::to_string(response.length());
    nghttp2_nv response_headers[] = {
        {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-type", (uint8_t*)"application/json", 12, 16, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-length", (uint8_t*)content_length.c_str(),
         14, content_length.length(), NGHTTP2_NV_FLAG_NONE}
    };

    // 创建数据提供者
    auto* buffer = new std::string(response);
    nghttp2_data_provider data_provider;
    data_provider.source.ptr = buffer;
    data_provider.read_callback = [](nghttp2_session* session,
                                   int32_t stream_id,
                                   uint8_t* buf,
                                   size_t length,
                                   uint32_t* data_flags,
                                   nghttp2_data_source* source,
                                   void* user_data) -> ssize_t {
        auto* response_data = static_cast<std::string*>(source->ptr);
        if (!response_data || response_data->empty()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            if (response_data) {
                delete response_data;
            }
            return 0;
        }

        size_t copy_len = std::min(length, response_data->length());
        memcpy(buf, response_data->data(), copy_len);
        *response_data = response_data->substr(copy_len);

        if (response_data->empty()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            delete response_data;
        }

        return copy_len;
    };

    // 提交响应头和数据
    int rv = nghttp2_submit_response(session, stream_id, response_headers, 3, &data_provider);
    if (rv != 0) {
        std::cerr << "提交响应失败: " << nghttp2_strerror(rv) << std::endl;
        delete buffer;
        return rv;
    }

    // 立即发送数据
    rv = nghttp2_session_send(session);
    if (rv != 0) {
        std::cerr << "发送响应失败: " << nghttp2_strerror(rv) << std::endl;
        return rv;
    }

    return 0;
}

// 数据帧回调
static int on_data_chunk_recv_callback(nghttp2_session* session,
                                     uint8_t flags,
                                     int32_t stream_id,
                                     const uint8_t* data,
                                     size_t len,
                                     void* user_data) {
    Http2Session* http2_session = static_cast<Http2Session*>(user_data);
    http2_session->stream_data[stream_id].append(reinterpret_cast<const char*>(data), len);
    std::cout << "接收到数据块，长度: " << len << std::endl;
    return 0;
}

// 帧接收完成回调
static int on_frame_recv_callback(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data) {
    Http2Session* http2_session = static_cast<Http2Session*>(user_data);
    
    switch (frame->hd.type) {
        case NGHTTP2_DATA:
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                std::cout << "数据帧结束，处理请求" << std::endl;
                auto it = http2_session->stream_data.find(frame->hd.stream_id);
                if (it != http2_session->stream_data.end()) {
                    handle_post_data(session, frame->hd.stream_id, it->second, user_data);
                    http2_session->stream_data.erase(it);
                }
            }
            break;
        case NGHTTP2_HEADERS:
            if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
                std::cout << "收到请求头" << std::endl;
            }
            break;
    }
    return 0;
}

// 头部帧回调
static int on_header_callback(nghttp2_session* session,
                            const nghttp2_frame* frame,
                            const uint8_t* name,
                            size_t namelen,
                            const uint8_t* value,
                            size_t valuelen,
                            uint8_t flags,
                            void* user_data) {
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    std::string name_str(reinterpret_cast<const char*>(name), namelen);
    std::string value_str(reinterpret_cast<const char*>(value), valuelen);
    std::cout << "收到头部: " << name_str << ": " << value_str << std::endl;
    return 0;
}

// 初始化HTTP2会话
Http2Session* create_http2_session(int fd) {
    Http2Session* http2_session = new Http2Session();
    http2_session->fd = fd;

    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);

    nghttp2_session_server_new(&http2_session->session, callbacks, http2_session);
    nghttp2_session_callbacks_del(callbacks);

    // 发送服务器连接设置
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535}
    };
    nghttp2_submit_settings(http2_session->session, NGHTTP2_FLAG_NONE, iv, 2);

    return http2_session;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "创建socket失败" << std::endl;
        return 1;
    }

    // 设置socket选项
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        std::cerr << "设置socket选项失败" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "绑定端口失败" << std::endl;
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        std::cerr << "监听失败" << std::endl;
        return 1;
    }

    std::cout << "HTTP/2服务器启动，监听端口 " << PORT << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            std::cerr << "接受连接失败" << std::endl;
            continue;
        }

        std::cout << "接受新连接" << std::endl;

        Http2Session* session = create_http2_session(client_fd);
        
        // 发送初始设置
        int rv = nghttp2_session_send(session->session);
        if (rv != 0) {
            std::cerr << "发送初始设置失败: " << nghttp2_strerror(rv) << std::endl;
            nghttp2_session_del(session->session);
            close(client_fd);
            delete session;
            continue;
        }

        // 主事件循环
        while (true) {
            uint8_t buf[4096];
            ssize_t read_length = read(client_fd, buf, sizeof(buf));
            if (read_length <= 0) {
                if (read_length < 0) {
                    std::cerr << "读取数据失败: " << strerror(errno) << std::endl;
                }
                break;
            }

            // 处理接收到的数据
            ssize_t rv = nghttp2_session_mem_recv(session->session, buf, read_length);
            if (rv < 0) {
                std::cerr << "处理接收数据失败: " << nghttp2_strerror(rv) << std::endl;
                break;
            }

            // 发送待发送的数据
            rv = nghttp2_session_send(session->session);
            if (rv != 0) {
                std::cerr << "发送数据失败: " << nghttp2_strerror(rv) << std::endl;
                break;
            }
        }

        // 清理会话
        nghttp2_session_del(session->session);
        close(client_fd);
        delete session;

        std::cout << "连接关闭" << std::endl;
    }

    close(server_fd);
    return 0;
}
