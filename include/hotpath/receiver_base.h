#ifndef HOTPATH_RECEIVER_BASE_H
#define HOTPATH_RECEIVER_BASE_H
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

template <typename Derived, typename Buffer, std::size_t Burst>
class receiver_base {
    static_assert(Burst > 0);
public:
    using buffer_type = Buffer;
    static constexpr std::size_t burst_size = Burst;

    void run(std::uint64_t max_messages) {
        static_assert(requires(Derived& d, std::span<Buffer> s) {
            { d.acquire(s) } -> std::convertible_to<std::size_t>;
            { d.receive(s) } -> std::convertible_to<std::size_t>;
            d.release(s);
            d.stamp(s);
            d.publish(s);
            d.idle();
        }, "Derived receiver is missing a required hook (acquire/receive/release/stamp/publish/idle)");

        Derived& d = self();
        d.on_start();

        Buffer bufs[Burst];
        const std::span<Buffer> all(bufs, Burst);
        std::uint64_t processed = 0;
        while (processed < max_messages) {
            const std::size_t avail = d.acquire(all);
            if (avail == 0) {
                d.idle();
                continue;
            }

            const std::size_t got = d.receive(all.first(avail));
            if (got < avail) {
                d.release(all.subspan(got, avail - got));
            }
            if (got == 0) {
                d.idle();
                continue;
            }

            const std::span<Buffer> filled = all.first(got);
            d.stamp(filled);
            d.publish(filled);
            processed += got;
        }

        d.on_stop();
    }

protected:
    receiver_base() = default;
    ~receiver_base() = default;
    receiver_base(const receiver_base&) = delete;
    receiver_base& operator=(const receiver_base&) = delete;

    void on_start() noexcept {}
    void on_stop() noexcept {}

private:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
};

#endif //HOTPATH_RECEIVER_BASE_H
