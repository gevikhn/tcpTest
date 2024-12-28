#pragma once
#include "frame.h"
#include <functional>
#include <map>

class FrameHandler {
public:
    using FrameCallback = std::function<void(const Frame&)>;
    using ResponseCallback = std::function<void(uint32_t stream_id, uint32_t sequence, const std::vector<uint8_t>&)>;

protected:
    void processIncomingData(const std::vector<uint8_t>& data) {
        frame_buffer_.insert(frame_buffer_.end(), data.begin(), data.end());
        
        size_t bytes_consumed;
        std::vector<Frame> frames = frame_parser_.processData(frame_buffer_, bytes_consumed);
        
        if (bytes_consumed > 0) {
            frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + bytes_consumed);
        }

        for (const auto& frame : frames) {
            bool checksum_valid = frame.verifyChecksum();
            if (!checksum_valid) {
                continue;
            }

            if (frame_callback_) {
                frame_callback_(frame);
            }
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
