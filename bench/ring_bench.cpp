// rdtsc-based latency benchmark for spsc_ring<T, Capacity>.
//
// Correctness is covered by test/spsc_ring_test.cpp (GTest) - this binary
// measures push/pop latency only, plus a lightweight sequence-number check
// as a sanity guard against races that only show up under real threading.
//
// MUST be built Release (-O3). Run pinned: taskset -c 0,1 ./cmake-build-release/ring_bench

#include <x86intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <thread>
#include <vector>

#include "hotpath/spsc_ring.h"

namespace {

struct alignas(64) Msg {
    uint64_t sequence;
    char _pad[56];
};

constexpr std::size_t kCapacity = 4096;
constexpr std::size_t kWarmup = 10'000;
constexpr std::size_t kIterations = 1'000'000;

void pin_self_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "warning: failed to pin thread to core %d (rc=%d)\n", core, rc);
    }
}

void print_percentiles(const char* label, std::vector<uint64_t>& cycles, double ns_per_cycle) {
    std::sort(cycles.begin(), cycles.end());
    const std::size_t n = cycles.size();
    auto at = [&](double frac) { return cycles[static_cast<std::size_t>(frac * (n - 1))]; };

    std::printf("%s (%zu samples):\n", label, n);
    std::printf("  min:    %6lu cycles  (%7.1f ns)\n", cycles.front(), cycles.front() * ns_per_cycle);
    std::printf("  p50:    %6lu cycles  (%7.1f ns)\n", at(0.50), at(0.50) * ns_per_cycle);
    std::printf("  p99:    %6lu cycles  (%7.1f ns)\n", at(0.99), at(0.99) * ns_per_cycle);
    std::printf("  p99.9:  %6lu cycles  (%7.1f ns)\n", at(0.999), at(0.999) * ns_per_cycle);
    std::printf("  max:    %6lu cycles  (%7.1f ns)\n", cycles.back(), cycles.back() * ns_per_cycle);
}

double measure_ns_per_cycle() {
    unsigned aux;
    struct timespec ts_start{}, ts_end{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
    uint64_t tsc_start = __rdtscp(&aux);

    // Busy-wait ~100ms to get a stable calibration window.
    while (true) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
        double elapsed_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1e9 +
                             (ts_end.tv_nsec - ts_start.tv_nsec);
        if (elapsed_ns >= 100'000'000.0) break;
    }
    uint64_t tsc_end = __rdtscp(&aux);
    double elapsed_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1e9 +
                         (ts_end.tv_nsec - ts_start.tv_nsec);
    return elapsed_ns / static_cast<double>(tsc_end - tsc_start);
}

// Mode A: single-threaded push+pop round trip. No cross-core coherency
// traffic - this is the overhead floor of the ring buffer's own logic.
void run_single_threaded(double ns_per_cycle) {
    spsc_ring<Msg, kCapacity> ring;
    unsigned aux;
    std::vector<uint64_t> cycles;
    cycles.reserve(kIterations);

    for (std::size_t i = 0; i < kWarmup + kIterations; ++i) {
        Msg in{.sequence = i, ._pad = {}};
        Msg out{};

        uint64_t t0 = __rdtscp(&aux);
        bool pushed = ring.try_push(in);
        bool popped = pushed && ring.try_pop(out);
        uint64_t t1 = __rdtscp(&aux);

        if (!pushed || !popped) {
            std::fprintf(stderr, "single-threaded: push/pop failed at iter %zu\n", i);
            std::exit(1);
        }
        if (out.sequence != i) {
            std::fprintf(stderr, "single-threaded: sequence mismatch at iter %zu (got %lu)\n", i,
                          out.sequence);
            std::exit(1);
        }
        if (i >= kWarmup) cycles.push_back(t1 - t0);
    }

    print_percentiles("spsc_ring<Msg,4096> single-threaded push+pop", cycles, ns_per_cycle);
}

// Mode B: real producer/consumer threads pinned to separate physical cores.
// Timestamps live in a side array (indexed by sequence) so the measured
// path never reads the TSC out of the buf_ slot itself.
void run_threaded(double ns_per_cycle) {
    spsc_ring<Msg, kCapacity> ring;
    const std::size_t total = kWarmup + kIterations;
    std::vector<uint64_t> tsc_before(total);
    std::vector<uint64_t> latency(kIterations);

    std::thread producer([&] {
        pin_self_to_core(0);
        unsigned aux;
        for (std::size_t i = 0; i < total; ++i) {
            tsc_before[i] = __rdtscp(&aux);
            Msg msg{.sequence = i, ._pad = {}};
            while (!ring.try_push(msg)) {
                _mm_pause();
            }
        }
    });

    std::thread consumer([&] {
        pin_self_to_core(1);
        unsigned aux;
        for (std::size_t i = 0; i < total; ++i) {
            Msg out{};
            while (!ring.try_pop(out)) {
                _mm_pause();
            }
            uint64_t t1 = __rdtscp(&aux);
            if (out.sequence != i) {
                std::fprintf(stderr, "threaded: sequence mismatch at iter %zu (got %lu)\n", i,
                              out.sequence);
                std::exit(1);
            }
            if (i >= kWarmup) {
                latency[i - kWarmup] = t1 - tsc_before[i];
            }
        }
    });

    producer.join();
    consumer.join();

    print_percentiles("spsc_ring<Msg,4096> threaded round-trip (cores 0->1)", latency, ns_per_cycle);
}

}  // namespace

int main() {
    std::printf("Calibrating TSC frequency...\n");
    double ns_per_cycle = measure_ns_per_cycle();
    std::printf("TSC: ~%.3f GHz\n\n", 1.0 / ns_per_cycle);

    run_single_threaded(ns_per_cycle);
    std::printf("\n");
    run_threaded(ns_per_cycle);

    return 0;
}