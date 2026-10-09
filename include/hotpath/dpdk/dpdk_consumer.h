#ifndef HOTPATH_DPDK_CONSUMER_H
#define HOTPATH_DPDK_CONSUMER_H
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "hotpath/consumer_base.h"
#include "hotpath/rx_handler.h"
#include "hotpath/dpdk/dpdk_common.h"

template <rx_handler<rte_mbuf*> Handler>
class dpdk_consumer : public consumer_base<dpdk_consumer<Handler>, rte_mbuf*, kDpdkBurst> {
    using base = consumer_base<dpdk_consumer, rte_mbuf*, kDpdkBurst>;
    friend base;
public:
    dpdk_consumer(dpdk_ring& ring, int tsc_offset, Handler handler)
        : ring_(ring), tsc_offset_(tsc_offset), handler_(std::move(handler)) {}

private:
    std::size_t pop(std::span<rte_mbuf*> out) {
        return ring_.try_pop_batch(out);
    }
    uint64_t now() {
        return rte_rdtsc_precise();
    }
    void process(rte_mbuf* buf, uint64_t now) {
        auto* metadata = RTE_MBUF_DYNFIELD(buf, tsc_offset_, pkt_details*);
        handler_(buf, now - metadata->ts);
    }
    void release(std::span<rte_mbuf*> bufs) {
        rte_pktmbuf_free_bulk(bufs.data(), bufs.size());
    }
    void idle() {
        rte_pause();
    }

    dpdk_ring& ring_;
    int tsc_offset_;
    Handler handler_;
};

#endif //HOTPATH_DPDK_CONSUMER_H