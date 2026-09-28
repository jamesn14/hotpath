#ifndef HOTPATH_SOCKET_RECEIVER_H
#define HOTPATH_SOCKET_RECEIVER_H
#include <sys/socket.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "hotpath/receiver_base.h"
#include "hotpath/rx_mode.h"
#include "hotpath/socket/socket_common.h"

struct socket_batch_state {
    mmsghdr msgs[kSocketBurst];
    iovec iovecs[kSocketBurst];
    uint64_t n_histogram[kSocketBurst + 1] = {};
    uint64_t recvmmsg_calls = 0;
};
struct socket_single_state {};

template <rx_mode Mode>
class socket_receiver
    : public receiver_base<socket_receiver<Mode>, Packet*, burst_for(Mode, kSocketBurst)> {
    using base = receiver_base<socket_receiver<Mode>, Packet*, burst_for(Mode, kSocketBurst)>;
    friend base;
    using mode_state = std::conditional_t<Mode == rx_mode::batch, socket_batch_state, socket_single_state>;
public:
    socket_receiver(int fd, socket_pipeline& pipeline, int core);

private:
    std::size_t acquire(std::span<Packet*> out);
    std::size_t receive(std::span<Packet*> bufs);
    void release(std::span<Packet*> bufs);
    void stamp(std::span<Packet*> bufs);
    void publish(std::span<Packet*> bufs);
    void idle();
    void on_start();
    void on_stop();

    int fd_;
    socket_pipeline& pipeline_;
    int core_;
    std::size_t max_rx_ring_occupancy_ = 0;
    uint16_t last_len_ = 0;
    [[no_unique_address]] mode_state state_;
};

#endif //HOTPATH_SOCKET_RECEIVER_H
