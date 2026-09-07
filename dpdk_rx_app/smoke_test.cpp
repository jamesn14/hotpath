// Smoke test for dpdk_rx_app: verifies EAL init, mempool creation, port
// configure/start, and a synthetic spsc_ring<rte_mbuf*, N> round-trip all
// still work after a code change - no real network traffic required, so
// it's deterministic and safe to run in isolation.

#include "hotpath/spsc_ring.h"

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr unsigned kNbRxDesc = 1024;
constexpr unsigned kNbTxDesc = 0;
constexpr unsigned kBurstSize = 32;
constexpr unsigned kNumLcores = 2;
constexpr unsigned kMempoolCacheSize = 250;
constexpr unsigned kMinMbufs = 8192;
constexpr std::size_t kRingCapacity = 64;
constexpr std::size_t kRoundTripCount = 16;

const struct rte_eth_conf kPortConfDefault = {
    .rxmode = { .mtu = RTE_ETHER_MTU },
};

void check_port_setup(uint16_t port, struct rte_mempool* mempool) {
    struct rte_eth_conf port_conf = kPortConfDefault;
    uint16_t nb_rxd = kNbRxDesc, nb_txd = kNbTxDesc;

    if (!rte_eth_dev_is_valid_port(port))
        rte_exit(EXIT_FAILURE, "smoke test: invalid port\n");

    if (rte_eth_dev_configure(port, 1, 0, &port_conf) != 0)
        rte_exit(EXIT_FAILURE, "smoke test: dev configure failed\n");

    if (rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd) != 0)
        rte_exit(EXIT_FAILURE, "smoke test: adjust desc failed\n");

    if (rte_eth_rx_queue_setup(port, 0, kNbRxDesc, rte_eth_dev_socket_id(port), nullptr, mempool) != 0)
        rte_exit(EXIT_FAILURE, "smoke test: rx queue setup failed\n");

    if (rte_eth_dev_start(port) != 0)
        rte_exit(EXIT_FAILURE, "smoke test: dev start failed\n");

    std::printf("smoke test: port %u configured and started OK\n", port);
}

void check_ring_roundtrip(struct rte_mempool* mempool) {
    spsc_ring<rte_mbuf*, kRingCapacity> ring;
    struct rte_mbuf* sent[kRoundTripCount];

    for (std::size_t i = 0; i < kRoundTripCount; ++i) {
        sent[i] = rte_pktmbuf_alloc(mempool);
        if (sent[i] == nullptr)
            rte_exit(EXIT_FAILURE, "smoke test: mbuf alloc failed at index %zu\n", i);
        if (!ring.try_push(sent[i]))
            rte_exit(EXIT_FAILURE, "smoke test: ring push failed at index %zu\n", i);
    }

    for (std::size_t i = 0; i < kRoundTripCount; ++i) {
        struct rte_mbuf* out = nullptr;
        if (!ring.try_pop(out))
            rte_exit(EXIT_FAILURE, "smoke test: ring pop failed at index %zu\n", i);
        if (out != sent[i])
            rte_exit(EXIT_FAILURE, "smoke test: ring returned wrong pointer at index %zu\n", i);
        rte_pktmbuf_free(out);
    }

    std::printf("smoke test: ring round-trip of %zu mbufs OK\n", kRoundTripCount);
}

}  // namespace

int main(int argc, char* argv[]) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "smoke test: EAL init failed\n");
    argc -= ret;
    argv += ret;

    if (rte_eth_dev_count_avail() < 1)
        rte_exit(EXIT_FAILURE, "smoke test: no ports available\n");

    unsigned num_mbufs = std::max<unsigned>(
        kNbRxDesc + kNbTxDesc + kBurstSize + kNumLcores * kMempoolCacheSize, kMinMbufs);
    struct rte_mempool* mempool = rte_pktmbuf_pool_create(
        "SMOKE_MBUF_POOL", num_mbufs, kMempoolCacheSize, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mempool == nullptr)
        rte_exit(EXIT_FAILURE, "smoke test: mempool create failed\n");

    check_port_setup(0, mempool);
    check_ring_roundtrip(mempool);

    std::printf("SMOKE TEST PASSED\n");
    return 0;
}
