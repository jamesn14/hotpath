#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

#include "hotpath/spsc_ring.h"

TEST(SpscRing, FillToCapacityThenRejects) {
    spsc_ring<uint64_t, 16> ring;
    for (uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(ring.try_push(i)) << "push " << i << " should have succeeded";
    }
    uint64_t overflow = 999;
    EXPECT_FALSE(ring.try_push(overflow));
}

TEST(SpscRing, DrainIsFifoOrder) {
    spsc_ring<uint64_t, 16> ring;
    for (uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    for (uint64_t i = 0; i < 16; ++i) {
        uint64_t out = 0;
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    uint64_t out = 0;
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, WraparoundEveryOffset) {
    spsc_ring<uint64_t, 16> ring;
    uint64_t next_pushed = 0;
    uint64_t next_expected_pop = 0;

    // Push/pop in blocks of 5 across many cycles so head/tail walk through
    // every possible masked offset (0..15), not just a fresh-buffer start.
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(ring.try_push(next_pushed)) << "cycle " << cycle;
            ++next_pushed;
        }
        for (int i = 0; i < 5; ++i) {
            uint64_t out = 0;
            ASSERT_TRUE(ring.try_pop(out)) << "cycle " << cycle;
            EXPECT_EQ(out, next_expected_pop);
            ++next_expected_pop;
        }
    }
}

TEST(SpscRing, BatchRoundTripForcesWrap) {
    spsc_ring<uint64_t, 16> ring;

    // Push 14 to land head near the end of the array, then pop 14 so tail_
    // trails head_ by exactly the batch size we're about to push - this
    // forces the upcoming push_batch to wrap mid-copy.
    for (uint64_t i = 0; i < 14; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    for (int i = 0; i < 14; ++i) {
        uint64_t out = 0;
        ASSERT_TRUE(ring.try_pop(out));
    }

    std::array<uint64_t, 8> batch{};
    std::iota(batch.begin(), batch.end(), 1000);

    std::size_t pushed = ring.try_push_batch(batch);
    ASSERT_EQ(pushed, batch.size());

    std::array<uint64_t, 8> popped{};
    std::size_t popped_count = ring.try_pop_batch(popped);
    ASSERT_EQ(popped_count, batch.size());

    for (std::size_t i = 0; i < batch.size(); ++i) {
        EXPECT_EQ(popped[i], batch[i]) << "mismatch at index " << i;
    }
}

TEST(SpscRing, PushBatchClampsWhenOversized) {
    spsc_ring<uint64_t, 16> ring;

    std::vector<uint64_t> items(32, 42);  // twice the capacity
    std::size_t pushed = ring.try_push_batch(items);

    EXPECT_EQ(pushed, 16u);
    // Buffer should now be completely full - one more push must fail.
    EXPECT_FALSE(ring.try_push(uint64_t{1}));
}

TEST(SpscRing, PushBatchClampsToFreeSpaceWhenPartiallyFull) {
    spsc_ring<uint64_t, 16> ring;
    for (uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }

    std::vector<uint64_t> items(16, 7);  // more than the 6 remaining slots
    std::size_t pushed = ring.try_push_batch(items);

    EXPECT_EQ(pushed, 6u);
    EXPECT_EQ(ring.size(), 16u);
}

TEST(SpscRing, EmptySpanBatchesAreNoOps) {
    spsc_ring<uint64_t, 16> ring;

    std::vector<uint64_t> empty_items;
    EXPECT_EQ(ring.try_push_batch(empty_items), 0u);

    std::vector<uint64_t> empty_output;
    EXPECT_EQ(ring.try_pop_batch(empty_output), 0u);

    ASSERT_TRUE(ring.try_push(uint64_t{5}));
    std::vector<uint64_t> zero_capacity_output;  // still empty
    EXPECT_EQ(ring.try_pop_batch(zero_capacity_output), 0u);
    EXPECT_EQ(ring.size(), 1u);
}

TEST(SpscRing, SizeTracksPushesAndPops) {
    spsc_ring<uint64_t, 16> ring;
    EXPECT_EQ(ring.size(), 0u);

    for (uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    EXPECT_EQ(ring.size(), 5u);

    uint64_t out = 0;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(ring.size(), 4u);

    std::array<uint64_t, 4> drained{};
    std::size_t popped = ring.try_pop_batch(drained);
    EXPECT_EQ(popped, 4u);
    EXPECT_EQ(ring.size(), 0u);
}

TEST(SpscRing, CapacityReflectsTemplateParameter) {
    spsc_ring<uint64_t, 1024> ring;
    EXPECT_EQ(ring.capacity(), 1024u);
}
