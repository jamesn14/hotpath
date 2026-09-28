#ifndef HOTPATH_DPDK_CONSUMER_H
#define HOTPATH_DPDK_CONSUMER_H
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hotpath/consumer_base.h"
#include "hotpath/dpdk/dpdk_common.h"

class dpdk_consumer : public consumer_base<dpdk_consumer, rte_mbuf*, kDpdkBurst> {
    using base = consumer_base<dpdk_consumer, rte_mbuf*, kDpdkBurst>;
    friend base;
public:
    dpdk_consumer(dpdk_ring& ring, int tsc_offset, std::vector<uint64_t>& cycles, uint64_t warmup);

private:
    std::size_t pop(std::span<rte_mbuf*> out){

	};
    uint64_t now();
    void process(rte_mbuf* buf, uint64_t now);
    void release(std::span<rte_mbuf*> bufs);
    void idle();

    dpdk_ring& ring_;
    int tsc_offset_;
    std::vector<uint64_t>& cycles_;
    uint64_t warmup_;
    uint64_t count_ = 0;
};

#endif //HOTPATH_DPDK_CONSUMER_H
