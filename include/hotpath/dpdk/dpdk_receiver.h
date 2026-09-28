#ifndef HOTPATH_DPDK_RECEIVER_H
#define HOTPATH_DPDK_RECEIVER_H
#include <cstddef>
#include <cstdint>
#include <span>

#include "dpdk_common.h"
#include "hotpath/receiver_base.h"

class dpdk_receiver : public receiver_base<dpdk_receiver, rte_mbuf*, kDpdkBurst> {
    using base = receiver_base<dpdk_receiver, rte_mbuf*, kDpdkBurst>;
    friend base;
public:
    dpdk_receiver(uint16_t port, uint16_t queue, dpdk_ring& ring, int tsc_offset) : port_(port),
    queue_(queue), ring_(ring), tsc_offset_(tsc_offset) {}

private:
    std::size_t acquire(std::span<rte_mbuf*> out){
        return out.size();
    }
    std::size_t receive(std::span<rte_mbuf*> bufs){
        uint16_t pkts_received = rte_eth_rx_burst(port_, queue_, bufs.data(), kDpdkBurst);
        return pkts_received;
    }
    void release(std::span<rte_mbuf*>) {};
    void stamp(std::span<rte_mbuf*> bufs) {
        uint64_t ts = rte_rdtsc_precise();
        for (rte_mbuf* buf : bufs) {
            auto* metadata = (RTE_MBUF_DYNFIELD(bufs[p], tsc_offset_, pkt_details*));
            metadata->ts = ts;
        }
    }
    void publish(std::span<rte_mbuf*> bufs) {
        size_t _index = 0;
        size_t remaining = bufs.size();
        while (remaining > 0) {
            size_t pushed = ring_.try_push_batch(bufs.subspan(_index, remaining));
            if (pushed == 0) {
                rte_pause();
                continue;
            }
            remaining -= pushed;
            _index += pushed;
        }
    }
    void idle() {
        rte_pause();
    }

    uint16_t port_;
    uint16_t queue_;
    dpdk_ring& ring_;
    int tsc_offset_;
};

#endif //HOTPATH_DPDK_RECEIVER_H
