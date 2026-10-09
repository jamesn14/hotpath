#ifndef HOTPATH_DPDK_COMMON_H
#define HOTPATH_DPDK_COMMON_H
#include <cstddef>
#include <cstdint>

#include <rte_mbuf.h>
#include <rte_mbuf_dyn.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_pause.h>
#include "hotpath/spsc_ring.h"

inline constexpr std::size_t kDpdkBurst = 32;
inline constexpr std::size_t kDpdkRingSize = 4096;

using dpdk_ring = spsc_ring<rte_mbuf*, kDpdkRingSize>;

struct pkt_details {
    uint64_t ts;
};

#endif //HOTPATH_DPDK_COMMON_H
