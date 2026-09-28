cmake_minimum_required(VERSION 3.16)
project(socket_rx_app CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)

add_executable(socket_bench socket_bench.cpp)
target_include_directories(socket_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(socket_bench PRIVATE Threads::Threads)
target_compile_options(socket_bench PRIVATE -O2)