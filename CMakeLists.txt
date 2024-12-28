cmake_minimum_required(VERSION 3.10)
project(tcpTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/include)

# 原有的client和server目标
add_executable(client client.cpp)
add_executable(server server.cpp)

# 添加frame测试程序
add_executable(frame_test tests/frame_test.cpp)

target_link_libraries(server pthread)
target_link_libraries(client pthread)
