//
// Created by jamesn on 9/3/26.
//


#include <vector>

#include "dpdk_setup.h"

struct pkt_details {
       uint64_t ts;
};

struct ring_consumer_arg {
    spsc_ring<rte_mbuf *, 4096> *ring_ptr;
    std::vector<uint64_t>* cycles;
    size_t numIterations;
    size_t warmupIterations;
    int tsc_offset;
};

void print_percentiles(const char* label, std::vector<uint64_t>& cycles, double ns_per_cycle) {
    std::sort(cycles.begin(), cycles.end());
    const std::size_t n = cycles.size();
    auto at = [&](double frac) { return cycles[static_cast<std::size_t>(frac * (n - 1))]; };

    std::printf("%s (%zu samples):\n", label, n);
    std::printf("  min:    %6lu cycles  (%7.1f ns)\n", cycles.front(), cycles.front() * ns_per_cycle);
    std::printf("  p50:    %6lu cycles  (%7.1f ns)\n", at(0.50), at(0.50) * ns_per_cycle);
    std::printf("  p99:    %6lu cycles  (%7.1f ns)\n", at(0.99), at(0.99) * ns_per_cycle);
    std::printf("  p99.9:  %6lu cycles  (%7.1f ns)\n", at(0.999), at(0.999) * ns_per_cycle);
    std::printf("  max:    %6lu cycles  (%7.1f ns)\n", cycles.back(), cycles.back() * ns_per_cycle);
}

double measure_ns_per_cycle() {
    return 1e9 / (rte_get_tsc_hz());
}

void export_csv(const char* path, std::vector<uint64_t>& cycles, double ns_per_cycle) {
    FILE* f = std::fopen(path, "w");
    if (!f) {
        std::perror("fopen");
        return;
    }
    for (uint64_t c : cycles) {
        std::fprintf(f, "%.1f\n", c * ns_per_cycle);
    }
    std::fclose(f);
}

int add_pkt_metadata() {
    constexpr rte_mbuf_dynfield bench_dsc = {
        .name = "packet_details",
        .size = sizeof(struct pkt_details),
        .align = alignof(struct pkt_details),
        .flags = 0
    };
    int tsc_offset = rte_mbuf_dynfield_register(&bench_dsc);
    return tsc_offset;
}

void read_from_nic(spsc_ring<rte_mbuf*, 4096>* ring_ptr, uint64_t numIterations, int tsc_offset) {
    rte_mbuf *rx_pkts[kBurstSize];
    uint64_t processed = 0;
    uint64_t next_print = 0;
    while (processed < numIterations) {
        uint16_t pkts_received = rte_eth_rx_burst(0, 0, rx_pkts, kBurstSize);
        if (pkts_received == 0) {
            // struct rte_eth_stats stats;
            // rte_eth_stats_get(0, &stats);
            // std::printf("ipackets=%lu imissed=%lu ierrors=%lu rx_nombuf=%lu\n",
            // stats.ipackets, stats.imissed, stats.ierrors, stats.rx_nombuf);
            // std::printf("NIC: 0 packets this burst\n");
            rte_pause();
            continue;
        }
        size_t remaining = pkts_received;
        uint64_t ts = rte_rdtsc_precise();
        for (uint16_t p = 0; p < pkts_received; p++) {
            auto* metadata = reinterpret_cast<pkt_details*>(RTE_MBUF_DYNFIELD(rx_pkts[p], tsc_offset, void*));
            metadata->ts = ts;
        }
        size_t view_start = 0;
        while (remaining > 0) {
            std::span rx_pkts_view(&rx_pkts[view_start], remaining);
            size_t pushed = ring_ptr->try_push_batch(rx_pkts_view);
            if (pushed == 0) {
                std::printf("RING: push failed, ring full\n");
                rte_pause();
                continue;
            }
            remaining -= pushed;
            view_start += pushed;
        }
        processed += pkts_received;
        //         if (processed >= next_print) {
        //     std::printf("PRODUCER processed: %lu / %lu\n", processed, numIterations);
        //     next_print += 10000;
        // }
    }
}

int read_from_ring(void* arg) {
    auto* args = static_cast<ring_consumer_arg*>(arg);
    auto* ring_ptr = args->ring_ptr;
    auto& cycles = *args->cycles;
    auto warmupIterations = args->warmupIterations;
    auto numIterations = args->numIterations;
    auto tsc_offset = args->tsc_offset;
    rte_mbuf *rx_pkts[kBurstSize];
    uint64_t processed = 0;
    while (processed < numIterations) {
        size_t popped = ring_ptr->try_pop_batch(rx_pkts);
        if (popped == 0) {
            rte_pause();
            continue;
        }
        uint64_t end_time = rte_rdtsc_precise();
        for (size_t p = 0; p < popped && processed < numIterations; p++, processed++) {
            auto* metadata = reinterpret_cast<pkt_details*>(RTE_MBUF_DYNFIELD(rx_pkts[p], tsc_offset, void*));
            uint64_t start_time = metadata->ts;
            if (processed >= warmupIterations)
                cycles[processed - warmupIterations] = end_time - start_time;
        }
        rte_pktmbuf_free_bulk(rx_pkts, popped);
        // static uint64_t next_print = 10000;
        // if (processed >= next_print) {
        //     std::printf("processed: %lu / %lu\n", processed, numIterations);
        //     next_print += 10000;
        // }
    }
    return 0;
}

int main(int argc, char** argv) {
    eth_init(&argc, &argv);
    std::printf("Calibrating TSC frequency...\n");
    double ns_per_cycle = measure_ns_per_cycle();
    if (argc < 3) {
        rte_exit(EXIT_FAILURE, "Usage: <warmup> <iterations>\n");
    }
    uint64_t warmupIterations = std::strtoull(argv[1], nullptr, 10);
    uint64_t numIterations = std::strtoull(argv[2], nullptr, 10);
    std::printf("DEBUG: argv[1]=%s argv[2]=%s warmup=%lu iterations=%lu\n", argv[1], argv[2], warmupIterations, numIterations);
    spsc_ring<rte_mbuf *, 4096> ring;
    std::vector<uint64_t> cycles(numIterations-warmupIterations);
    int tsc_offset = add_pkt_metadata();
    ring_consumer_arg consumer_arg{
        .ring_ptr = &ring,
        .cycles = &cycles,
        .numIterations = numIterations,
        .warmupIterations = warmupIterations,
        .tsc_offset = tsc_offset
    };
    rte_eal_remote_launch(read_from_ring, &consumer_arg, 1);
    read_from_nic(&ring, numIterations, tsc_offset);
    rte_eal_wait_lcore(1);
    print_percentiles("rx_bench round-trip",cycles, ns_per_cycle);
    export_csv("rx_bench_latencies.csv", cycles, ns_per_cycle);
}