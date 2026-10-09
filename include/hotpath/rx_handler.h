//
// Created by jamesn on 9/28/26.
//

#ifndef HOTPATH_RX_HANDLER_H
#define HOTPATH_RX_HANDLER_H
#include <concepts>
#include <cstdint>

template <typename H, typename Buffer>
concept rx_handler = requires (H& h, Buffer buf, std::uint64_t latency)
{
    {h(buf, latency)} -> std::same_as<void>;
};
#endif //HOTPATH_RX_HANDLER_H
