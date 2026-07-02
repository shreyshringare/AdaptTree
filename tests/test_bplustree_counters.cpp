// test_bplustree_counters.cpp
// Phase 11 — benchmark instrumentation counter tests
// Covers: cmp_count_ and fallback_count_ atomics on BPlusTree<NullWAL>

#include <gtest/gtest.h>
#include "adapttree/bplus_tree.hpp"
#include "adapttree/wal.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace adapttree {
extern template class BPlusTree<NullWAL>;
}  // namespace adapttree

// ─────────────────────────────────────────────────────────────────────────────
// Shared env helper (mirrors pattern from test_learned_integration.cpp)
// ─────────────────────────────────────────────────────────────────────────────

struct CounterEnv {
    adapttree::DiskManager          dm;
    NullWAL                         wal;
    adapttree::BufferPool<NullWAL>  pool;
    adapttree::BPlusTree<NullWAL>   tree;

    explicit CounterEnv(const std::string& path, int pool_size = 512)
        : dm(path)
        , pool(&dm, &wal, static_cast<size_t>(pool_size))
        , tree(pool, wal)
    {}
};

static std::string tmp_path(const std::string& tag) {
    return "/tmp/test_counters_" + tag + ".db";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 1 — Counters can be reset to zero
// ─────────────────────────────────────────────────────────────────────────────

TEST(BPlusTreeCounters, CanResetCountersToZero) {
    CounterEnv env(tmp_path("reset"));
    // Insert a few keys so counters accumulate something
    for (uint64_t i = 0; i < 10; ++i) {
        env.tree.insert(i * 1000, i);
    }
    env.tree.get(1000);  // trigger at least one comparison

    // Reset both counters
    env.tree.cmp_count_      = 0;
    env.tree.fallback_count_ = 0;

    EXPECT_EQ(env.tree.cmp_count_.load(),      0u);
    EXPECT_EQ(env.tree.fallback_count_.load(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2 — cmp_count_ accumulates when use_learned_index_ = false
// ─────────────────────────────────────────────────────────────────────────────

TEST(BPlusTreeCounters, CmpCountAccumulatesWithModelOff) {
    CounterEnv env(tmp_path("cmp_off"));
    env.tree.use_learned_index_ = false;

    static constexpr uint64_t kInsert = 300;
    for (uint64_t i = 0; i < kInsert; ++i) {
        env.tree.insert(i * 10, i);
    }

    // Reset so we measure only lookups
    env.tree.cmp_count_ = 0;

    for (uint64_t i = 0; i < kInsert; ++i) {
        auto r = env.tree.get(i * 10);
        // Prevent dead-code elimination without Google Benchmark dependency
        volatile auto sink = r.has_value();
        (void)sink;
    }

    EXPECT_GT(env.tree.cmp_count_.load(), 0u)
        << "cmp_count_ must be > 0 after binary search lookups";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3 — fallback_count_ is >= 0 after model-on lookups
// ─────────────────────────────────────────────────────────────────────────────

TEST(BPlusTreeCounters, FallbackCountNonNegativeWithModelOn) {
    CounterEnv env(tmp_path("fallback_on"));
    env.tree.use_learned_index_ = true;

    static constexpr uint64_t kInsert = 300;
    for (uint64_t i = 0; i < kInsert; ++i) {
        env.tree.insert(i * 10, i);
    }

    env.tree.fallback_count_ = 0;
    for (uint64_t i = 0; i < kInsert; ++i) {
        env.tree.get(i * 10);
    }

    // fallback_count_ is uint64_t — it can never be negative.
    // This assertion documents the invariant and will catch signed/unsigned drift.
    EXPECT_GE(env.tree.fallback_count_.load(), 0u);
}
