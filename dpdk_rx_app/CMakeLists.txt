cmake_minimum_required(VERSION 3.16)
project(dpdk_rx_app CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

add_executable(dpdk_rx_app main.cpp)
target_include_directories(dpdk_rx_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(dpdk_rx_app PRIVATE PkgConfig::DPDK)

add_executable(dpdk_rx_app_smoke_test smoke_test.cpp)
target_include_directories(dpdk_rx_app_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(dpdk_rx_app_smoke_test PRIVATE PkgConfig::DPDK)

add_executable(rx_bench rx_bench.cpp)
target_include_directories(rx_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(rx_bench PRIVATE PkgConfig::DPDK)
target_compile_options(rx_bench PRIVATE -O2)
