cmake_minimum_required(VERSION 3.16)
project(dpdk_rx_app CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

add_executable(dpdk_rx_app main.cpp)
target_link_libraries(dpdk_rx_app PRIVATE PkgConfig::DPDK)
