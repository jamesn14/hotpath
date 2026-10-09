#ifndef HOTPATH_CONSUMER_BASE_H
#define HOTPATH_CONSUMER_BASE_H
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

template <typename Derived, typename Buffer, std::size_t Burst>
class consumer_base {
    static_assert(Burst > 0);
public:
    using buffer_type = Buffer;
    static constexpr std::size_t burst_size = Burst;

    void run(std::uint64_t max_messages) {
        static_assert(requires(Derived& d, std::span<Buffer> s, Buffer b, std::uint64_t t) {
            { d.pop(s) } -> std::convertible_to<std::size_t>;
            { d.now() } -> std::convertible_to<std::uint64_t>;
            d.process(b, t);
            d.release(s);
            d.idle();
        }, "Derived consumer is missing a required hook (pop/now/process/release/idle)");

        Derived& d = self();
        d.on_start();

        Buffer bufs[Burst];
        const std::span<Buffer> all(bufs, Burst);
        std::uint64_t processed = 0;
        while (processed < max_messages) {
            const std::size_t popped = d.pop(all);
            if (popped == 0) {
                d.idle();
                continue;
            }

            const std::uint64_t ts = d.now();
            for (std::size_t i = 0; i < popped && processed < max_messages; i++, processed++) {
                d.process(bufs[i], ts);
            }
            d.release(all.first(popped));
        }

        d.on_stop();
    }

protected:
    consumer_base() = default;
    ~consumer_base() = default;
    consumer_base(const consumer_base&) = delete;
    consumer_base& operator=(const consumer_base&) = delete;

    void on_start() noexcept {}
    void on_stop() noexcept {}

private:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
};

#endif //HOTPATH_CONSUMER_BASE_H
