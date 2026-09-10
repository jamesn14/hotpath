// Concurrent producer/consumer correctness stress test for spsc_ring.
//
// Unlike test/spsc_ring_test.cpp (single-threaded, GTest, fast/deterministic)
// this drives the ring with two real OS threads racing on the atomics, and
// is meant to be run under ThreadSanitizer (-DHOTPATH_TSAN=ON). TSan checks
// the actual happens-before edges implied by your memory_order arguments
// against the C++ abstract machine -- it will flag a broken acquire/release
// pairing even on x86, where the hardware itself never would. A clean manual
// run without TSan is weak evidence on its own; this is what makes it strong.
//
// Two modes, selected with --single (default) or --batch:
//   --single  drives try_push/try_pop, one item at a time.
//   --batch   drives try_push_batch/try_pop_batch, exercising the two-chunk
//             wraparound memcpy path and the single-release-per-call publish
//             under real concurrency instead of just single-threaded GTest.
//
// In both modes: producer pushes the sequence [0, N) in order, consumer
// writes each popped value into its own preallocated slot. After both
// threads join, every slot must hold exactly the value that belongs there --
// no drops, no duplicates, no reordering.
//
// Both threads occasionally sleep for a few hundred microseconds so the
// other side is forced to hit the ring's full/empty boundary (and
// spin-retry) instead of just running through the comfortably-half-full
// common case.

#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "hotpath/spsc_ring.h"

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr std::size_t kDefaultIterations = 300'000;
#else
constexpr std::size_t kDefaultIterations = 5'000'000;
#endif

// Small on purpose: at similar producer/consumer speed, a large capacity
// rarely actually fills over millions of iterations. Keeping it small forces
// wraparound and full/empty transitions constantly, stalls or not.
constexpr std::size_t kCapacity = 64;

// Batch mode's chunk size. Smaller than kCapacity so several batches are
// needed to fill or drain the ring, exercising the partial-push/partial-pop
// clamping under real concurrency (not just the single-threaded GTest cases).
constexpr std::size_t kBatchSize = 8;

// How often (in iterations) each side rolls the dice on stalling, and how
// long it stalls for when it does -- long enough to let the other side
// either drain the ring to empty or fill it to capacity.
constexpr std::size_t kStallCheckPeriod = 2048;
constexpr std::size_t kBatchStallCheckPeriod = kStallCheckPeriod / kBatchSize;
constexpr int kStallChancePercent = 10;
constexpr auto kStallDuration = std::chrono::microseconds(300);

enum class Mode { kSingle, kBatch };

struct StageStats {
    std::size_t retries = 0;  // times a push/pop call made zero progress and had to retry
};

void producer_thread_single(spsc_ring<uint64_t, kCapacity>& ring, std::size_t n, StageStats& stats) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> chance(1, 100);

    for (std::size_t i = 0; i < n; ++i) {
        while (!ring.try_push(static_cast<uint64_t>(i))) {
            ++stats.retries;
            _mm_pause();
        }
        if (i % kStallCheckPeriod == 0 && chance(rng) <= kStallChancePercent) {
            std::this_thread::sleep_for(kStallDuration);
        }
    }
}

void consumer_thread_single(spsc_ring<uint64_t, kCapacity>& ring, std::size_t n,
                             std::vector<uint64_t>& collected, StageStats& stats) {
    std::mt19937 rng(std::random_device{}() ^ 0x9e3779b9u);
    std::uniform_int_distribution<int> chance(1, 100);

    for (std::size_t i = 0; i < n; ++i) {
        uint64_t out = 0;
        while (!ring.try_pop(out)) {
            ++stats.retries;
            _mm_pause();
        }
        collected[i] = out;
        if (i % kStallCheckPeriod == 0 && chance(rng) <= kStallChancePercent) {
            std::this_thread::sleep_for(kStallDuration);
        }
    }
}

void producer_thread_batch(spsc_ring<uint64_t, kCapacity>& ring, std::size_t n, StageStats& stats) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> chance(1, 100);

    std::array<uint64_t, kBatchSize> chunk{};
    std::size_t next = 0;
    std::size_t batch_count = 0;

    while (next < n) {
        std::size_t chunk_len = std::min(kBatchSize, n - next);
        for (std::size_t k = 0; k < chunk_len; ++k) chunk[k] = static_cast<uint64_t>(next + k);
        // try_push_batch may only take part of the chunk if the ring doesn't
        // have enough free space yet -- keep retrying just the leftover tail.
        std::size_t offset = 0;
        while (offset < chunk_len) {
            std::size_t pushed =
                ring.try_push_batch(std::span<const uint64_t>(chunk.data() + offset, chunk_len - offset));
            if (pushed == 0) {
                ++stats.retries;
                _mm_pause();
                continue;
            }
            offset += pushed;
        }
        next += chunk_len;

        if (batch_count++ % kBatchStallCheckPeriod == 0 && chance(rng) <= kStallChancePercent) {
            std::this_thread::sleep_for(kStallDuration);
        }
    }
}

void consumer_thread_batch(spsc_ring<uint64_t, kCapacity>& ring, std::size_t n,
                            std::vector<uint64_t>& collected, StageStats& stats) {
    std::mt19937 rng(std::random_device{}() ^ 0x9e3779b9u);
    std::uniform_int_distribution<int> chance(1, 100);

    std::array<uint64_t, kBatchSize> chunk{};
    std::size_t next = 0;
    std::size_t batch_count = 0;

    while (next < n) {
        std::size_t want = std::min(kBatchSize, n - next);
        std::size_t got = ring.try_pop_batch(std::span<uint64_t>(chunk.data(), want));
        if (got == 0) {
            ++stats.retries;
            _mm_pause();
            continue;
        }
        for (std::size_t k = 0; k < got; ++k) collected[next + k] = chunk[k];
        next += got;

        if (batch_count++ % kBatchStallCheckPeriod == 0 && chance(rng) <= kStallChancePercent) {
            std::this_thread::sleep_for(kStallDuration);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = kDefaultIterations;
    Mode mode = Mode::kSingle;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--batch") {
            mode = Mode::kBatch;
        } else if (arg == "--single") {
            mode = Mode::kSingle;
        } else {
            n = std::strtoull(argv[i], nullptr, 10);
        }
    }

    std::printf("spsc_ring stress test: mode=%s capacity=%zu iterations=%zu%s\n",
                mode == Mode::kBatch ? "batch" : "single", kCapacity, n,
#if defined(__SANITIZE_THREAD__)
                " [TSan build]"
#else
                ""
#endif
    );

    spsc_ring<uint64_t, kCapacity> ring;
    std::vector<uint64_t> collected(n, std::numeric_limits<uint64_t>::max());
    StageStats producer_stats;
    StageStats consumer_stats;

    auto start = std::chrono::steady_clock::now();

    std::thread producer;
    std::thread consumer;
    if (mode == Mode::kBatch) {
        producer = std::thread(producer_thread_batch, std::ref(ring), n, std::ref(producer_stats));
        consumer =
            std::thread(consumer_thread_batch, std::ref(ring), n, std::ref(collected), std::ref(consumer_stats));
    } else {
        producer = std::thread(producer_thread_single, std::ref(ring), n, std::ref(producer_stats));
        consumer =
            std::thread(consumer_thread_single, std::ref(ring), n, std::ref(collected), std::ref(consumer_stats));
    }

    producer.join();
    consumer.join();

    double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("done in %.2fs (producer full-retries=%zu, consumer empty-retries=%zu)\n", elapsed_s,
                producer_stats.retries, consumer_stats.retries);

    if (producer_stats.retries == 0 || consumer_stats.retries == 0) {
        std::printf(
            "warning: one side never had to retry -- the full/empty boundary was never "
            "exercised. Consider raising kStallChancePercent/kStallDuration or lowering "
            "kCapacity.\n");
    }

    std::size_t mismatches = 0;
    std::size_t first_bad_index = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (collected[i] != i) {
            if (mismatches == 0) first_bad_index = i;
            ++mismatches;
        }
    }

    if (mismatches != 0) {
        std::fprintf(stderr, "FAIL: %zu/%zu slots wrong (first bad index %zu: expected %zu, got %zu)\n",
                     mismatches, n, first_bad_index, first_bad_index, collected[first_bad_index]);
        return 1;
    }

    std::printf("PASS: all %zu values received in order, no drops or duplicates\n", n);
    return 0;
}