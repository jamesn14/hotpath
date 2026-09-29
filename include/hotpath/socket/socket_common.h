#ifndef HOTPATH_SOCKET_COMMON_H
#define HOTPATH_SOCKET_COMMON_H
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

#include "hotpath/spsc_ring.h"

inline constexpr std::size_t kMaxFrameSize = 2048;
inline constexpr std::size_t kPoolSize = 4096;
inline constexpr std::size_t kSocketBurst = 32;

struct Packet {
    uint64_t ts;
    uint16_t len;
    uint8_t data[kMaxFrameSize];
};

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

struct socket_pipeline {
    spsc_ring<Packet*, kPoolSize> rx_ring;
    spsc_ring<Packet*, kPoolSize> free_ring;
    std::vector<Packet> storage = std::vector<Packet>(kPoolSize);
};

#endif //HOTPATH_SOCKET_COMMON_H
