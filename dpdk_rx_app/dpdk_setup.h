//
// Created by jamesn on 9/3/26.
//

#ifndef DPDK_RX_APP_DPDK_SETUP_H
#define DPDK_RX_APP_DPDK_SETUP_H
#include <hotpath/spsc_ring.h>
#include <algorithm>
#include <rte_eal.h>
#include <rte_common.h>
#include <cstdlib>
#include <iostream>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>

constexpr unsigned kNbRxDesc = 1024;
constexpr unsigned kNbTxDesc = 0;
constexpr unsigned kBurstSize = 32;
constexpr unsigned kNumLcores = 2;
constexpr unsigned kMempoolCacheSize = 250;
constexpr unsigned kMinMbufs = 8192;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = { .mtu = RTE_ETHER_MTU }
};

inline void port_init(uint16_t port, struct rte_mempool* mempool) {
    const uint16_t rx_rings = 1, tx_rings = 0;
    uint16_t nb_rxd = kNbRxDesc, nb_txd = kNbTxDesc;
    int retval;
    struct rte_eth_conf port_conf = port_conf_default;
    if (!rte_eth_dev_is_valid_port(port))
        rte_exit(EXIT_FAILURE, "Invalid port number\n");
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
       rte_exit(EXIT_FAILURE, "Error configure ether device\n");
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        rte_exit(EXIT_FAILURE, "Error adjust ether device\n");
    for (uint16_t q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, kNbRxDesc, rte_eth_dev_socket_id(port), NULL, mempool); // check if rx_conf here needs anything besides default
        if (retval != 0)
            rte_exit(EXIT_FAILURE, "A queue failed setup\n");
    }
    retval = rte_eth_dev_start(port);
    if (retval != 0)
        rte_exit(EXIT_FAILURE, "Cannot start device\n");
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        rte_exit(EXIT_FAILURE, "Cannot get MAC address\n");

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
               " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            port, RTE_ETHER_ADDR_BYTES(&addr));
}

inline void lcore_main(spsc_ring<rte_mbuf*, 64>* ring) {
   for (;;) {
       struct rte_mbuf *mbufs_[kBurstSize];
       spsc_ring<rte_mbuf *, 64> *ring_ptr = static_cast<spsc_ring<rte_mbuf *, 64> *>(ring);
       uint16_t nb_rx_pkts = rte_eth_rx_burst(0, 0, mbufs_, kBurstSize);
       size_t view_start = 0;
       size_t remaining = nb_rx_pkts;
       while (remaining > 0) {
           std::span<struct rte_mbuf*> mbufs_view(&mbufs_[view_start], remaining);
           size_t pushed = ring_ptr->try_push_batch(mbufs_view);
           if (pushed == 0) {
               rte_pause();
               continue;
           }
           remaining -= pushed;
           view_start += pushed;
       }
   }
}

inline int consumer_main(void* arg) {
     spsc_ring<rte_mbuf *, 64> *ring_ptr = static_cast<spsc_ring<rte_mbuf *, 64> *>(arg);
     struct rte_mbuf *mbufs_[kBurstSize];
    for (;;) {
        size_t popped = ring_ptr->try_pop_batch(mbufs_);
        if (popped == 0) {
            rte_pause();
            continue;
        }
        for (size_t i = 0; i < popped; i++) {
            std::cout << "packet size: " << mbufs_[i]->pkt_len << '\n';
            rte_pktmbuf_free(mbufs_[i]);
        }
    }
    return 0;
}

inline void eth_init(int* argc, char** argv[]) {
    uint16_t nb_ports;
    unsigned num_mbufs;
    int ret = rte_eal_init(*argc, *argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    *argc -= ret;
    *argv += ret;
    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "No ports available\n");
    num_mbufs = std::max<unsigned>(kNbRxDesc + kNbTxDesc + kBurstSize + kNumLcores * kMempoolCacheSize, kMinMbufs);
    rte_mempool *mempool = rte_pktmbuf_pool_create("MBUF_POOL", num_mbufs, kMempoolCacheSize, 0,
                                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mempool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    port_init(0, mempool);
}

inline void run_dpdk(int argc, char *argv[]) {
    eth_init(&argc, &argv);
    spsc_ring<rte_mbuf *, 64> ring;
    rte_eal_remote_launch(consumer_main, &ring, 1);
    lcore_main(&ring);
}


#endif //DPDK_RX_APP_DPDK_SETUP_H
