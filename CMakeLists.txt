cmake_minimum_required(VERSION 4.1)
project(hotpath)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)
find_package(Threads REQUIRED)

add_executable(hotpath main.cpp)

# Interface library so the include path flows to all consumers.
add_library(spsc_ring INTERFACE)
target_include_directories(spsc_ring INTERFACE ${CMAKE_SOURCE_DIR}/include)

# GTest - pulled at configure time, no system package needed.
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(spsc_ring_test test/spsc_ring_test.cpp)
target_link_libraries(spsc_ring_test PRIVATE spsc_ring GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(spsc_ring_test)

# Benchmark binary - MUST be built Release or the numbers are meaningless.
add_executable(ring_bench bench/ring_bench.cpp)
target_link_libraries(ring_bench PRIVATE spsc_ring Threads::Threads)
target_compile_options(ring_bench PRIVATE -O3 -march=native -funroll-loops)

# Real two-thread producer/consumer correctness stress test - the one meant
# to be run under TSan (see HOTPATH_TSAN below), not just on the dev machine.
add_executable(spsc_ring_stress_test test/spsc_ring_stress_test.cpp)
target_link_libraries(spsc_ring_stress_test PRIVATE spsc_ring Threads::Threads)
add_test(NAME spsc_ring_stress_test_single COMMAND spsc_ring_stress_test --single)
add_test(NAME spsc_ring_stress_test_batch COMMAND spsc_ring_stress_test --batch)
set_tests_properties(spsc_ring_stress_test_single spsc_ring_stress_test_batch PROPERTIES TIMEOUT 180)

option(HOTPATH_TSAN "Build ring_bench and spsc_ring_stress_test with ThreadSanitizer" OFF)
if (HOTPATH_TSAN)
    target_compile_options(ring_bench PRIVATE -fsanitize=thread -g -O1)
    target_link_options(ring_bench PRIVATE -fsanitize=thread)
    target_compile_options(spsc_ring_stress_test PRIVATE -fsanitize=thread -g -O1)
    target_link_options(spsc_ring_stress_test PRIVATE -fsanitize=thread)
endif ()
