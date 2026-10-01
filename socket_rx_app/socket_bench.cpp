#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hotpath/bench/latency_recorder.h"
#include "hotpath/rx_mode.h"
#include "hotpath/socket/socket_common.h"
#include "hotpath/socket/socket_consumer.h"
#include "hotpath/socket/socket_receiver.h"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_percentiles(const char* label, std::vector<uint64_t>& latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    const std::size_t n = latencies_ns.size();
    auto at = [&](double frac) { return latencies_ns[static_cast<std::size_t>(frac * (n - 1))]; };

    std::printf("%s (%zu samples):\n", label, n);
    std::printf("  min:    %8lu ns\n", latencies_ns.front());
    std::printf("  p50:    %8lu ns\n", at(0.50));
    std::printf("  p99:    %8lu ns\n", at(0.99));
    std::printf("  p99.9:  %8lu ns\n", at(0.999));
    std::printf("  max:    %8lu ns\n", latencies_ns.back());
}

void export_csv(const char* path, std::vector<uint64_t>& latencies_ns) {
    FILE* f = std::fopen(path, "w");
    if (!f) {
        perror("fopen");
        return;
    }
    for (uint64_t v : latencies_ns) {
        std::fprintf(f, "%lu\n", v);
    }
    std::fclose(f);
}

void pin_self_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "failed to pin thread to core %d (rc=%d)\n", core, rc);
    }
}

int open_raw_socket(const char* ifname) {
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("socket");
        std::exit(EXIT_FAILURE);
    }

    int rcvbuf_size = 8 * 1024 * 1024;  // 8 MB, well above the observed flood rate
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        perror("setsockopt(SO_RCVBUFFORCE)");
    }

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        std::exit(EXIT_FAILURE);
    }

    struct sockaddr_ll addr {};
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = ifr.ifr_ifindex;
    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        std::exit(EXIT_FAILURE);
    }

    return sock;
}

template <rx_mode Mode>
void run(int sock, socket_pipeline& pipeline, std::vector<uint64_t>& latencies_ns,
         uint64_t warmup, uint64_t iterations) {
    using recorder = latency_recorder<Packet*>;
    socket_receiver<Mode> rx(sock, pipeline);
    socket_consumer<Mode, recorder> cx(pipeline, recorder{&latencies_ns, warmup});

    std::thread consumer([&] {
        pin_self_to_core(1);
        cx.run(iterations);
    });
    pin_self_to_core(0);
    rx.run(iterations);
    consumer.join();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "Usage: %s <ifname> <warmup> <iterations> <single|batch>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char* ifname = argv[1];
    uint64_t warmupIterations = std::strtoull(argv[2], nullptr, 10);
    uint64_t numIterations = std::strtoull(argv[3], nullptr, 10);
    std::string mode = argv[4];
    if (mode != "single" && mode != "batch") {
        std::fprintf(stderr, "mode must be 'single' or 'batch'\n");
        return EXIT_FAILURE;
    }

    int sock = open_raw_socket(ifname);

    // ~8 MB of packet storage, so it lives on the heap. Every packet starts
    // out free; the receiver pops from free_ring, the consumer pushes back.
    auto pipeline = std::make_unique<socket_pipeline>();
    for (auto& pkt : pipeline->storage) {
        while (!pipeline->free_ring.try_push(&pkt)) {
        }
    }

    std::vector<uint64_t> latencies_ns;
    latencies_ns.resize(numIterations - warmupIterations);
    latencies_ns.clear();

    uint64_t run_start = now_ns();
    if (mode == "single") {
        run<rx_mode::single>(sock, *pipeline, latencies_ns, warmupIterations, numIterations);
    } else {
        run<rx_mode::batch>(sock, *pipeline, latencies_ns, warmupIterations, numIterations);
    }
    uint64_t run_end = now_ns();
    std::printf("RUN_DURATION ns=%lu seconds=%.3f\n", run_end - run_start, (run_end - run_start) / 1e9);

    struct tpacket_stats pkt_stats {};
    socklen_t stats_len = sizeof(pkt_stats);
    if (getsockopt(sock, SOL_PACKET, PACKET_STATISTICS, &pkt_stats, &stats_len) == 0) {
        std::printf("DROP_STATS received=%u dropped=%u\n", pkt_stats.tp_packets, pkt_stats.tp_drops);
    } else {
        perror("getsockopt(PACKET_STATISTICS)");
    }

    close(sock);
    std::string label = "socket round-trip (" + mode + ")";
    std::string csv_path = "socket_bench_" + mode + "_latencies.csv";
    print_percentiles(label.c_str(), latencies_ns);
    export_csv(csv_path.c_str(), latencies_ns);
    return 0;
}
