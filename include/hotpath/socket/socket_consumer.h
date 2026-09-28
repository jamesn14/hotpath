#ifndef HOTPATH_SOCKET_CONSUMER_H
#define HOTPATH_SOCKET_CONSUMER_H
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hotpath/consumer_base.h"
#include "hotpath/rx_mode.h"
#include "hotpath/socket/socket_common.h"

template <rx_mode Mode>
class socket_consumer
    : public consumer_base<socket_consumer<Mode>, Packet*, burst_for(Mode, kSocketBurst)> {
    using base = consumer_base<socket_consumer<Mode>, Packet*, burst_for(Mode, kSocketBurst)>;
    friend base;
public:
    socket_consumer(socket_pipeline& pipeline, std::vector<uint64_t>& latencies_ns, uint64_t warmup, int core);

private:
    std::size_t pop(std::span<Packet*> out);
    uint64_t now();
    void process(Packet* pkt, uint64_t now);
    void release(std::span<Packet*> bufs);
    void idle();
    void on_start();

    socket_pipeline& pipeline_;
    std::vector<uint64_t>& latencies_ns_;
    uint64_t warmup_;
    int core_;
    uint64_t count_ = 0;
};

#endif //HOTPATH_SOCKET_CONSUMER_H
