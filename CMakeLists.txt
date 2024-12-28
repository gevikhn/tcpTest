cmake_minimum_required(VERSION 3.10)
project(tcpTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找nghttp2库
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

include_directories(${PROJECT_SOURCE_DIR}/include ${NGHTTP2_INCLUDE_DIRS})

# 原有的client和server目标
add_executable(client client.cpp)
add_executable(server server.cpp)

# 添加frame测试程序
# add_executable(frame_test tests/frame_test.cpp)

target_link_libraries(server pthread)
target_link_libraries(client pthread)

# HTTP服务器目标
add_executable(http_server src/http_server.cpp)
target_link_libraries(http_server ${NGHTTP2_LIBRARIES} pthread)

add_executable(rpcServer src/rpc_server.cpp)
add_executable(rpcClient src/rpc_client.cpp)
target_link_libraries(rpcServer pthread)
target_link_libraries(rpcClient pthread)
