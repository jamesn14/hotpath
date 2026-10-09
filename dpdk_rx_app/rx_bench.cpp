//
// Created by jamesn on 9/3/26.
//

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dpdk_setup.h"
#include "hotpath/bench/latency_recorder.h"
#include "hotpath/dpdk/dpdk_common.h"
#include "hotpath/dpdk/dpdk_consumer.h"
#include "hotpath/dpdk/dpdk_receiver.h"

namespace {

constexpr unsigned kConsumerLcore = 1;

using recorder = latency_recorder<rte_mbuf*>;
using bench_consumer = dpdk_consumer<recorder>;

struct consumer_launch {
    bench_consumer* cx;
    uint64_t iterations;
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
        .size = sizeof(pkt_details),
        .align = alignof(pkt_details),
        .flags = 0
    };
    return rte_mbuf_dynfield_register(&bench_dsc);
}

}  // namespace

int main(int argc, char** argv) {
    eth_init(&argc, &argv);
    const double ns_per_cycle = 1e9 / rte_get_tsc_hz();
    if (argc < 3) {
        rte_exit(EXIT_FAILURE, "Usage: <warmup> <iterations>\n");
    }
    uint64_t warmupIterations = std::strtoull(argv[1], nullptr, 10);
    uint64_t numIterations = std::strtoull(argv[2], nullptr, 10);

    int tsc_offset = add_pkt_metadata();
    if (tsc_offset < 0) {
        rte_exit(EXIT_FAILURE, "Cannot register mbuf dynfield\n");
    }

    std::vector<uint64_t> cycles;
    cycles.resize(numIterations - warmupIterations);
    cycles.clear();

    dpdk_ring ring;
    dpdk_receiver rx(0, 0, ring, tsc_offset);
    bench_consumer cx(ring, tsc_offset, recorder{&cycles, warmupIterations});

    consumer_launch launch{&cx, numIterations};
    rte_eal_remote_launch(+[](void* arg) -> int {
        auto* l = static_cast<consumer_launch*>(arg);
        l->cx->run(l->iterations);
        return 0;
    }, &launch, kConsumerLcore);
    rx.run(numIterations);
    rte_eal_wait_lcore(kConsumerLcore);

    struct rte_eth_stats stats;
    rte_eth_stats_get(0, &stats);
    std::printf("DROP_STATS received=%lu dropped=%lu (imissed=%lu rx_nombuf=%lu ierrors=%lu)\n",
                stats.ipackets, stats.imissed + stats.rx_nombuf, stats.imissed, stats.rx_nombuf, stats.ierrors);

    print_percentiles("rx_bench round-trip", cycles, ns_per_cycle);
    export_csv("rx_bench_latencies.csv", cycles, ns_per_cycle);
}
