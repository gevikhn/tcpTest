#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <numeric>
#include <iostream>

// Frame格式:
// | Frame Length (4 bytes) | Stream ID (4 bytes) | Sequence (4 bytes) | Checksum (4 bytes) | Payload |
struct Frame {
    static constexpr size_t HEADER_SIZE = 16;  // 4(length) + 4(stream_id) + 4(sequence) + 4(checksum)
    
    uint32_t length;     // payload长度
    uint32_t stream_id;  // stream标识符
    uint32_t sequence;   // 序列号
    uint32_t checksum;   // 校验和
    std::vector<uint8_t> payload;
    int client_id{-1};   // 客户端ID，不参与序列化

    static bool enable_checksum;  // 是否启用校验和

    // 计算校验和
    uint32_t calculateChecksum() const {
        if (!enable_checksum) {
            return 0;
        }

        // 计算头部字段的校验和
        uint32_t sum = length + stream_id + sequence;
        
        // 计算payload的校验和
        if (!payload.empty()) {
            sum += std::accumulate(payload.begin(), payload.end(), 0u);
        }
        
        return sum;
    }

    // 验证校验和
    bool verifyChecksum() const {
        if (!enable_checksum) {
            return true;
        }
        return checksum == calculateChecksum();
    }

    // 序列化frame到二进制
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> data(HEADER_SIZE + payload.size());
        
        // 写入length (网络字节序)
        data[0] = (length >> 24) & 0xFF;
        data[1] = (length >> 16) & 0xFF;
        data[2] = (length >> 8) & 0xFF;
        data[3] = length & 0xFF;

        // 写入stream_id (网络字节序)
        data[4] = (stream_id >> 24) & 0xFF;
        data[5] = (stream_id >> 16) & 0xFF;
        data[6] = (stream_id >> 8) & 0xFF;
        data[7] = stream_id & 0xFF;

        // 写入sequence (网络字节序)
        data[8] = (sequence >> 24) & 0xFF;
        data[9] = (sequence >> 16) & 0xFF;
        data[10] = (sequence >> 8) & 0xFF;
        data[11] = sequence & 0xFF;

        // 计算并写入checksum (网络字节序)
        uint32_t current_checksum = calculateChecksum();
        data[12] = (current_checksum >> 24) & 0xFF;
        data[13] = (current_checksum >> 16) & 0xFF;
        data[14] = (current_checksum >> 8) & 0xFF;
        data[15] = current_checksum & 0xFF;

        // 写入payload
        if (!payload.empty()) {
            std::copy(payload.begin(), payload.end(), data.begin() + HEADER_SIZE);
        }

        return data;
    }

    // 从二进制数据解析frame
    static Frame deserialize(const std::vector<uint8_t>& data) {
        Frame frame;
        
        if (data.size() < HEADER_SIZE) {
            throw std::runtime_error("数据太短，无法解析帧头");
        }
        
        // 解析length
        frame.length = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
        
        // 解析stream_id
        frame.stream_id = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        
        // 解析sequence
        frame.sequence = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

        // 解析checksum
        frame.checksum = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
        
        // 验证payload长度
        if (data.size() < HEADER_SIZE + frame.length) {
            throw std::runtime_error("数据太短，无法解析完整payload");
        }
        
        // 解析payload
        frame.payload.assign(data.begin() + HEADER_SIZE, data.begin() + HEADER_SIZE + frame.length);

        return frame;
    }

    // 添加新的静态方法用于解析头部
    static bool parseHeader(const std::vector<uint8_t>& data, uint32_t& length, uint32_t& stream_id, uint32_t& sequence, uint32_t& checksum) {
        if (data.size() < HEADER_SIZE) {
            return false;
        }
        
        length = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
        stream_id = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        sequence = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
        checksum = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
        
        return true;
    }
};

// 定义静态成员变量
bool Frame::enable_checksum = false;

// 修改帧解析状态机
class FrameParser {
public:
    enum class State {
        READING_HEADER,
        READING_PAYLOAD
    };

    struct FrameParseState {
        State state;
        size_t header_bytes_read;
        size_t payload_bytes_read;
        std::vector<uint8_t> header_buffer;
        Frame frame;
        size_t total_frame_size;  // 帧的总大小（头部+消息体）
        size_t start_position;    // 帧在原始数据中的起始位置

        FrameParseState(size_t pos) 
            : state(State::READING_HEADER), 
              header_bytes_read(0), 
              payload_bytes_read(0),
              total_frame_size(0),
              start_position(pos) {
            header_buffer.resize(Frame::HEADER_SIZE);
        }
    };

    // 处理接收到的数据
    std::vector<Frame> processData(const std::vector<uint8_t>& data, size_t& bytes_consumed) {
        std::vector<Frame> completed_frames;
        bytes_consumed = 0;

        try {
            while (bytes_consumed < data.size()) {
                // 尝试解析一个完整的帧
                if (data.size() - bytes_consumed >= Frame::HEADER_SIZE) {
                    // 先读取帧头，获取长度
                    uint32_t frame_length = (data[bytes_consumed] << 24) | 
                                          (data[bytes_consumed + 1] << 16) | 
                                          (data[bytes_consumed + 2] << 8) | 
                                          data[bytes_consumed + 3];

                    // 检查是否有完整的帧
                    if (data.size() - bytes_consumed >= Frame::HEADER_SIZE + frame_length) {
                        // 解析完整帧
                        Frame frame = Frame::deserialize(
                            std::vector<uint8_t>(
                                data.begin() + bytes_consumed,
                                data.begin() + bytes_consumed + Frame::HEADER_SIZE + frame_length
                            )
                        );
                        
                        completed_frames.push_back(std::move(frame));
                        bytes_consumed += Frame::HEADER_SIZE + frame_length;
                        
                        std::cout << "成功解析帧: stream_id=" << frame.stream_id 
                                  << ", sequence=" << frame.sequence 
                                  << ", length=" << frame_length << std::endl;
                    } else {
                        // 数据不完整，等待更多数据
                        std::cout << "帧数据不完整，等待更多数据. 需要: " 
                                  << (Frame::HEADER_SIZE + frame_length) 
                                  << " 字节, 当前有: " 
                                  << (data.size() - bytes_consumed) << " 字节" << std::endl;
                        break;
                    }
                } else {
                    // 头部数据不完整，等待更多数据
                    std::cout << "帧头不完整，等待更多数据" << std::endl;
                    break;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "解析帧时发生错误: " << e.what() << std::endl;
            // 发生错误时，跳过当前字节，继续尝试解析
            bytes_consumed++;
        }

        return completed_frames;
    }

private:
    std::list<FrameParseState> active_frames_;  // 使用list避免迭代器失效
};
