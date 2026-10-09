#ifndef HOTPATH_SOCKET_CONSUMER_H
#define HOTPATH_SOCKET_CONSUMER_H
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>

#include "hotpath/consumer_base.h"
#include "hotpath/rx_handler.h"
#include "hotpath/rx_mode.h"
#include "hotpath/socket/socket_common.h"

template <rx_mode Mode, rx_handler<Packet*> Handler>
class socket_consumer
    : public consumer_base<socket_consumer<Mode, Handler>, Packet*, burst_for(Mode, kSocketBurst)> {
    using base = consumer_base<socket_consumer, Packet*, burst_for(Mode, kSocketBurst)>;
    friend base;
public:
    socket_consumer(socket_pipeline& pipeline, Handler handler)
        : pipeline_(pipeline), handler_(std::move(handler)) {}

private:
    std::size_t pop(std::span<Packet*> out) {
        return pipeline_.rx_ring.try_pop_batch(out);
    }
    uint64_t now() {
        return now_ns();
    }
    void process(Packet* pkt, uint64_t now) {
        handler_(pkt, now - pkt->ts);
    }
    // The consumer is free_ring's only producer (the receiver stashes its
    // leftovers instead of pushing them back), which keeps free_ring SPSC.
    void release(std::span<Packet*> bufs) {
        std::size_t pushed = 0;
        while (pushed < bufs.size()) {
            pushed += pipeline_.free_ring.try_push_batch(bufs.subspan(pushed));
        }
    }
    void idle() {
        std::this_thread::yield();
    }

    socket_pipeline& pipeline_;
    Handler handler_;
};

#endif //HOTPATH_SOCKET_CONSUMER_H