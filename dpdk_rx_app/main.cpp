#include <hotpath/spsc_ring.h>
#include <algorithm>
#include <rte_eal.h>
#include <rte_common.h>
#include <cstdlib>
#include <iostream>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include "dpdk_setup.h"

// constexpr unsigned kNbRxDesc = 1024;
// constexpr unsigned kNbTxDesc = 0;
// constexpr unsigned kBurstSize = 32;
// constexpr unsigned kNumLcores = 2;
// constexpr unsigned kMempoolCacheSize = 250;
// constexpr unsigned kMinMbufs = 8192;
//
// static const struct rte_eth_conf port_conf_default = {
//         .rxmode = { .mtu = RTE_ETHER_MTU }
// };

int main(int argc, char *argv[]) {
    run_dpdk(argc, argv);
}
