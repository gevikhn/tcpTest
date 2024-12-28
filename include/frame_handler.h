#pragma once
#include "frame.h"
#include <functional>
#include <map>
#include <iostream>

class FrameHandler {
public:
    using FrameCallback = std::function<void(const Frame&)>;
    using ResponseCallback = std::function<void(uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>&)>;

protected:
    void processIncomingData(const std::vector<uint8_t>& data) {
        if (data.empty()) {
            std::cout << "收到空数据包" << std::endl;
            return;
        }
        
        std::cout << "收到数据包大小: " << data.size() << " 字节" << std::endl;
        
        // 限制缓冲区大小，防止内存耗尽
        static const size_t MAX_BUFFER_SIZE = 10 * 1024 * 1024; // 10MB
        if (frame_buffer_.size() + data.size() > MAX_BUFFER_SIZE) {
            std::cerr << "警告：帧缓冲区超过最大限制，清空缓冲区" << std::endl;
            frame_buffer_.clear();
            return;
        }
        
        frame_buffer_.insert(frame_buffer_.end(), data.begin(), data.end());
        std::cout << "当前帧缓冲区大小: " << frame_buffer_.size() << " 字节" << std::endl;
        
        size_t bytes_consumed = 0;
        size_t iteration_count = 0;
        const size_t MAX_ITERATIONS = 1000; // 防止无限循环
        
        do {
            if (iteration_count++ > MAX_ITERATIONS) {
                std::cerr << "警告：处理循环次数过多，中断处理" << std::endl;
                break;
            }
            
            size_t current_consumed;
            std::vector<Frame> frames = frame_parser_.processData(frame_buffer_, current_consumed);
            
            std::cout << "本次处理消耗字节数: " << current_consumed 
                      << ", 解析到帧数: " << frames.size() << std::endl;
            
            if (current_consumed == 0) {
                std::cout << "没有新的数据可以处理" << std::endl;
                break;
            }
            
            bytes_consumed += current_consumed;
            
            for (const auto& frame : frames) {
                if (!frame.verifyChecksum()) {
                    std::cerr << "警告：帧校验和无效" << std::endl;
                    continue;
                }

                std::cout << "处理帧: stream_id=" << frame.stream_id 
                          << ", sequence=" << frame.sequence 
                          << ", payload size=" << frame.payload.size() << std::endl;

                if (frame_callback_) {
                    frame_callback_(frame);
                }
            }
        } while (bytes_consumed < frame_buffer_.size());

        if (bytes_consumed > 0) {
            frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + bytes_consumed);
            std::cout << "清理已处理数据，剩余缓冲区大小: " << frame_buffer_.size() << " 字节" << std::endl;
        }
    }

    Frame createFrame(uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>& payload) {
        Frame frame;
        frame.stream_id = stream_id;
        frame.sequence = sequence;
        frame.payload = payload;
        frame.length = payload.size();
        return frame;
    }

    void setFrameCallback(FrameCallback cb) {
        frame_callback_ = std::move(cb);
    }

    std::vector<uint8_t> frame_buffer_;
    FrameParser frame_parser_;
    FrameCallback frame_callback_;
};
