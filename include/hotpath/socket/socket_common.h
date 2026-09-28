#ifndef HOTPATH_SOCKET_COMMON_H
#define HOTPATH_SOCKET_COMMON_H
#include <cstddef>
#include <cstdint>
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

struct socket_pipeline {
    spsc_ring<Packet*, kPoolSize> rx_ring;
    spsc_ring<Packet*, kPoolSize> free_ring;
    std::vector<Packet> storage = std::vector<Packet>(kPoolSize);
};

#endif //HOTPATH_SOCKET_COMMON_H
