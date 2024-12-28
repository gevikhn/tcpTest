#include <gtest/gtest.h>
#include "../include/frame.h"
#include "../include/tcp_client.h"
#include <thread>
#include <chrono>

class FrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 设置测试数据
        test_payload = {'H', 'e', 'l', 'l', 'o'};
        test_stream_id = 1;
        test_sequence = 1;
    }

    std::vector<uint8_t> test_payload;
    uint32_t test_stream_id;
    uint32_t test_sequence;
};

TEST_F(FrameTest, SerializeDeserialize) {
    // 创建并序列化帧
    Frame frame;
    frame.stream_id = test_stream_id;
    frame.sequence = test_sequence;
    frame.payload = test_payload;
    frame.length = test_payload.size();
    frame.calculateChecksum();

    std::vector<uint8_t> serialized = frame.serialize();

    // 反序列化并验证
    Frame deserialized = Frame::deserialize(serialized);
    
    EXPECT_EQ(deserialized.stream_id, test_stream_id);
    EXPECT_EQ(deserialized.sequence, test_sequence);
    EXPECT_EQ(deserialized.payload, test_payload);
    EXPECT_EQ(deserialized.length, test_payload.size());
    EXPECT_TRUE(deserialized.verifyChecksum());
}

TEST_F(FrameTest, ChecksumVerification) {
    Frame frame;
    frame.stream_id = test_stream_id;
    frame.sequence = test_sequence;
    frame.payload = test_payload;
    frame.length = test_payload.size();
    frame.calculateChecksum();

    EXPECT_TRUE(frame.verifyChecksum());

    // 修改数据后校验和应该失败
    frame.payload[0] = 'X';
    EXPECT_FALSE(frame.verifyChecksum());
}

class ClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        connected_ = false;
        received_response_ = false;
    }

    void TearDown() override {
        client_.disconnect();
    }

    void runTest() {
        ASSERT_TRUE(client_.connect("127.0.0.1", 8080));

        // 发送测试消息
        std::string message = "Test Message";
        std::vector<uint8_t> payload(message.begin(), message.end());
        int stream_id;
        std::vector<uint8_t> response;

        ASSERT_TRUE(client_.sendAndWaitResponse(payload, stream_id, response, 5000));
        
        // 验证响应
        std::string response_str(response.begin(), response.end());
        EXPECT_FALSE(response_str.empty());
        received_response_ = true;
    }

    TcpClient client_;
    bool connected_;
    bool received_response_;
};

TEST_F(ClientTest, SendReceiveMessage) {
    runTest();
    EXPECT_TRUE(received_response_);
}

TEST_F(ClientTest, MultipleMessages) {
    ASSERT_TRUE(client_.connect("127.0.0.1", 8080));

    for (int i = 0; i < 3; ++i) {
        std::string message = "Test Message " + std::to_string(i);
        std::vector<uint8_t> payload(message.begin(), message.end());
        int stream_id;
        std::vector<uint8_t> response;

        ASSERT_TRUE(client_.sendAndWaitResponse(payload, stream_id, response, 5000));
        
        std::string response_str(response.begin(), response.end());
        EXPECT_FALSE(response_str.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
