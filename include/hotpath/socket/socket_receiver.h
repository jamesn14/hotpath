#ifndef HOTPATH_SOCKET_RECEIVER_H
#define HOTPATH_SOCKET_RECEIVER_H
#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <span>
#include <thread>
#include <type_traits>

#include "hotpath/receiver_base.h"
#include "hotpath/rx_mode.h"
#include "hotpath/socket/socket_common.h"

inline constexpr std::size_t kTsCtrlSize = CMSG_SPACE(sizeof(timespec));

struct ts_ctrl {
    alignas(cmsghdr) unsigned char buf[kTsCtrlSize];
};

inline uint64_t kernel_rx_ts(msghdr& hdr) {
    for (cmsghdr* c = CMSG_FIRSTHDR(&hdr); c != nullptr; c = CMSG_NXTHDR(&hdr, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
            timespec ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
        }
    }
    return 0;
}

struct socket_batch_state {
    mmsghdr msgs[kSocketBurst] = {};
    iovec iovecs[kSocketBurst] = {};
    ts_ctrl ctrls[kSocketBurst] = {};
    uint64_t n_histogram[kSocketBurst + 1] = {};
    uint64_t recvmmsg_calls = 0;
};
struct socket_single_state {
    msghdr hdr = {};
    iovec iov = {};
    ts_ctrl ctrl = {};
};

template <rx_mode Mode>
class socket_receiver
    : public receiver_base<socket_receiver<Mode>, Packet*, burst_for(Mode, kSocketBurst)> {
    using base = receiver_base<socket_receiver<Mode>, Packet*, burst_for(Mode, kSocketBurst)>;
    friend base;
    static constexpr std::size_t kBurst = burst_for(Mode, kSocketBurst);
    using mode_state = std::conditional_t<Mode == rx_mode::batch, socket_batch_state, socket_single_state>;
public:
    socket_receiver(int fd, socket_pipeline& pipeline) : fd_(fd), pipeline_(pipeline) {}

private:
    std::size_t acquire(std::span<Packet*> out) {
        const std::size_t stashed = spare_count_;
        std::copy_n(spare_, stashed, out.begin());
        spare_count_ = 0;
        return stashed + pipeline_.free_ring.try_pop_batch(out.subspan(stashed));
    }
    std::size_t receive(std::span<Packet*> bufs) {
        if constexpr (Mode == rx_mode::single) {
            state_.iov.iov_base = bufs[0]->data;
            state_.hdr.msg_controllen = kTsCtrlSize;
            ssize_t n = recvmsg(fd_, &state_.hdr, 0);
            if (n <= 0)
                return 0;
            bufs[0]->len = static_cast<uint16_t>(n);
            bufs[0]->ts = rx_ts(state_.hdr);
            return 1;
        } else {
            for (std::size_t i = 0; i < bufs.size(); i++) {
                state_.iovecs[i].iov_base = bufs[i]->data;
                state_.msgs[i].msg_hdr.msg_controllen = kTsCtrlSize;
            }
            int n = recvmmsg(fd_, state_.msgs, static_cast<unsigned int>(bufs.size()), MSG_WAITFORONE, nullptr);
            if (n <= 0)
                return 0;
            ++state_.recvmmsg_calls;
            ++state_.n_histogram[n];
            for (int i = 0; i < n; i++) {
                bufs[i]->len = static_cast<uint16_t>(state_.msgs[i].msg_len);
                bufs[i]->ts = rx_ts(state_.msgs[i].msg_hdr);
            }
            return n;
        }
    }
    void release(std::span<Packet*> bufs) {
        // bufs.size() <= kBurst always holds; the min only lets GCC prove it
        // (otherwise -Wstringop-overread fires for the single-mode instantiation).
        const std::size_t n = std::min(bufs.size(), kBurst);
        std::copy_n(bufs.begin(), n, spare_);
        spare_count_ = n;
    }
    void stamp(std::span<Packet*>) {}
    uint64_t rx_ts(msghdr& hdr) {
        const uint64_t ts = kernel_rx_ts(hdr);
        if (ts != 0)
            return ts;
        ++missing_ts_;
        return now_ns();
    }
    void publish(std::span<Packet*> bufs) {
        std::size_t pushed = 0;
        std::size_t n_unsigned = bufs.size();
        while (pushed < n_unsigned) {
            pushed += pipeline_.rx_ring.try_push_batch(bufs.subspan(pushed, n_unsigned - pushed));
        }
        max_rx_ring_occupancy_ = std::max(max_rx_ring_occupancy_, pipeline_.rx_ring.size());
    }
    void idle() {
        std::this_thread::yield();
    }
    void on_start() {
        int one = 1;
        if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) < 0) {
            std::perror("setsockopt(SO_TIMESTAMPNS)");
        }
        if constexpr (Mode == rx_mode::single) {
            state_.iov.iov_len = kMaxFrameSize;
            state_.hdr.msg_iov = &state_.iov;
            state_.hdr.msg_iovlen = 1;
            state_.hdr.msg_control = state_.ctrl.buf;
        } else {
            for (std::size_t i = 0; i < kSocketBurst; i++) {
                state_.iovecs[i].iov_len = kMaxFrameSize;
                state_.msgs[i].msg_hdr.msg_iovlen = 1;
                state_.msgs[i].msg_hdr.msg_iov = &state_.iovecs[i];
                state_.msgs[i].msg_hdr.msg_control = state_.ctrls[i].buf;
            }
        }
    }
    void on_stop() {
        if constexpr (Mode == rx_mode::batch) {
            std::printf("N_DIST calls=%lu", state_.recvmmsg_calls);
            for (std::size_t i = 1; i <= kSocketBurst; i++) {
                if (state_.n_histogram[i] > 0) {
                    std::printf(" n%zu=%lu", i, state_.n_histogram[i]);
                }
            }
            std::printf("\n");
        }
        std::printf("RING_STATS max_rx_ring_occupancy=%zu capacity=%zu\n",
                    max_rx_ring_occupancy_, pipeline_.rx_ring.capacity());
        std::printf("TS_STATS source=SO_TIMESTAMPNS missing=%lu\n", missing_ts_);
    }

    int fd_;
    socket_pipeline& pipeline_;
    std::size_t max_rx_ring_occupancy_ = 0;
    [[no_unique_address]] mode_state state_;
    Packet* spare_[kBurst];
    std::size_t spare_count_ = 0;
    uint64_t missing_ts_ = 0;
};

#endif //HOTPATH_SOCKET_RECEIVER_H