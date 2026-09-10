#define _GNU_SOURCE

#include "hotpath/spsc_ring.h"

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
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMaxFrameSize = 2048;
constexpr std::size_t kPoolSize = 4096;
constexpr std::size_t kBurstSize = 32;
constexpr std::size_t kRecvVlen = 32;

struct Packet {
    uint64_t ts;
    uint16_t len;
    uint8_t data[kMaxFrameSize];
};

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

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void pin_self_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "g: failed to pin thread to core %d (rc=%d)\n", core, rc);
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

struct Pipeline {
    spsc_ring<Packet*, kPoolSize> rx_ring;
    spsc_ring<Packet*, kPoolSize> free_ring;
    std::vector<Packet> storage{kPoolSize};
};

void receive_loop(int sock, Pipeline* p, uint64_t numIterations) {
    pin_self_to_core(0);
    uint64_t processed = 0;

    Packet* batch[kBurstSize];
    struct mmsghdr msgs[kBurstSize];
    struct iovec iovecs[kBurstSize];

    while (processed < numIterations) {
        std::size_t got = p->free_ring.try_pop_batch(std::span(batch, kRecvVlen));
        if (got == 0) {
            std::this_thread::yield();
            continue;
        }

        std::memset(msgs, 0, got * sizeof(msgs[0]));
        for (std::size_t i = 0; i < got; i++) {
            iovecs[i].iov_base = batch[i]->data;
            iovecs[i].iov_len = kMaxFrameSize;
            msgs[i].msg_hdr.msg_iov = &iovecs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }

        int n = recvmmsg(sock, msgs, static_cast<unsigned int>(got), MSG_WAITFORONE, nullptr);
        if (n <= 0) {
            std::size_t returned = 0;
            while (returned < got) {
                returned += p->free_ring.try_push_batch(std::span(batch).subspan(returned, got - returned));
            }
            continue;
        }

        uint64_t ts = now_ns();
        for (int i = 0; i < n; i++) {
            batch[i]->ts = ts;
            batch[i]->len = static_cast<uint16_t>(msgs[i].msg_len);
        }
        std::size_t pushed = 0;
        std::size_t n_unsigned = static_cast<std::size_t>(n);
        while (pushed < n_unsigned) {
            pushed += p->rx_ring.try_push_batch(std::span(batch).subspan(pushed, n_unsigned - pushed));
        }
        if (n_unsigned < got) {
            std::size_t leftover = got - n_unsigned;
            std::size_t returned = 0;
            while (returned < leftover) {
                returned += p->free_ring.try_push_batch(std::span(batch).subspan(n_unsigned + returned, leftover - returned));
            }
        }
        processed += static_cast<uint64_t>(n);
    }
}

void receive_loop_single(int sock, Pipeline* p, uint64_t numIterations) {
    pin_self_to_core(0);
    uint64_t processed = 0;
    while (processed < numIterations) {
        Packet* pkt;
        while (!p->free_ring.try_pop(pkt)) {
            std::this_thread::yield();
        }
        ssize_t n = recv(sock, pkt->data, kMaxFrameSize, 0);
        if (n <= 0) {
            while (!p->free_ring.try_push(pkt)) {
            }
            continue;
        }
        pkt->ts = now_ns();
        pkt->len = static_cast<uint16_t>(n);
        while (!p->rx_ring.try_push(pkt)) {
            std::this_thread::yield();
        }
        processed++;
    }
}

void consume_loop(Pipeline* p, std::vector<uint64_t>* latencies_ns, uint64_t numIterations,
                   uint64_t warmupIterations) {
    pin_self_to_core(1);
    uint64_t processed = 0;
    while (processed < numIterations) {
        Packet* pkt;
        while (!p->rx_ring.try_pop(pkt)) {
            std::this_thread::yield();
        }
        uint64_t end_time = now_ns();
        if (processed >= warmupIterations) {
            (*latencies_ns)[processed - warmupIterations] = end_time - pkt->ts;
        }
        while (!p->free_ring.try_push(pkt)) {
        }
        processed++;
    }
}

void consume_loop_batch(Pipeline* p, std::vector<uint64_t>* latencies_ns, uint64_t numIterations,
                         uint64_t warmupIterations) {
    pin_self_to_core(1);
    uint64_t processed = 0;
    Packet* batch[kBurstSize];
    while (processed < numIterations) {
        std::size_t popped = p->rx_ring.try_pop_batch(std::span(batch, kBurstSize));
        if (popped == 0) {
            std::this_thread::yield();
            continue;
        }
        uint64_t end_time = now_ns();
        for (std::size_t i = 0; i < popped && processed < numIterations; i++, processed++) {
            if (processed >= warmupIterations) {
                (*latencies_ns)[processed - warmupIterations] = end_time - batch[i]->ts;
            }
        }
        std::size_t pushed = 0;
        while (pushed < popped) {
            pushed += p->free_ring.try_push_batch(std::span(batch).subspan(pushed, popped - pushed));
        }
    }
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

    auto pipeline = std::make_unique<Pipeline>();
    for (auto& pkt : pipeline->storage) {
        while (!pipeline->free_ring.try_push(&pkt)) {
        }
    }

    std::vector<uint64_t> latencies_ns(numIterations - warmupIterations);

    std::thread consumer;
    if (mode == "single") {
        consumer = std::thread(consume_loop, pipeline.get(), &latencies_ns, numIterations, warmupIterations);
        receive_loop_single(sock, pipeline.get(), numIterations);
    } else {
        consumer = std::thread(consume_loop_batch, pipeline.get(), &latencies_ns, numIterations, warmupIterations);
        receive_loop(sock, pipeline.get(), numIterations);
    }
    consumer.join();

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
