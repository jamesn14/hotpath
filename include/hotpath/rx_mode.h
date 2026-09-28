#ifndef HOTPATH_RX_MODE_H
#define HOTPATH_RX_MODE_H
#include <cstddef>

enum class rx_mode { single, batch };

constexpr std::size_t burst_for(rx_mode mode, std::size_t batch_burst) noexcept {
    return mode == rx_mode::single ? 1 : batch_burst;
}

#endif //HOTPATH_RX_MODE_H
