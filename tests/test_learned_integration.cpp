// test_learned_integration.cpp
// TDD RED phase — Phase 9, Plan 01
// Tests for LEARN-01 through LEARN-06.
//
// All references to has_model, learnedSegment(), learnedSegmentMut(),
// use_learned_index_, pgm_builder_, fallback_count_, and the new
// findSlotInLeaf overload are intentionally unresolved at this stage.
// This file must FAIL to compile before any production code is changed.

#include <gtest/gtest.h>
#include "adapttree/bplus_tree.hpp"
#include "adapttree/wal.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Test WAL spy — records the last page written so LEARN-03 can inspect the
// after-image bytes embedded in the page header (WAL records the full page).
// ─────────────────────────────────────────────────────────────────────────────

struct RecordingWAL {
    uint64_t next_lsn = 1;

    // Last full page bytes seen by an INSERT/UPDATE — used by LEARN-03.
    std::vector<uint8_t> last_redo_bytes;
    uint32_t             last_redo_page_id = 0;

    uint64_t append(WalRecord& r) {
        r.lsn = next_lsn++;
        if (!r.redo_data.empty()) {
            last_redo_bytes   = r.redo_data;
            last_redo_page_id = r.page_id;
        }
        return r.lsn;
    }
    void     flush_to(uint64_t) {}
    uint64_t flushed_lsn() const { return UINT64_MAX; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build an in-memory BPlusTree backed by a tiny DiskManager-free pool.
// We reuse the same pattern as earlier phase tests: NullWAL + in-memory pool.
// ─────────────────────────────────────────────────────────────────────────────

namespace adapttree {

// Explicit instantiation declaration so the linker finds the implementation.
extern template class BPlusTree<NullWAL>;
extern template class BPlusTree<RecordingWAL>;

}  // namespace adapttree

// ─────────────────────────────────────────────────────────────────────────────
// Fixture: insert N sequential keys into a tree, force at least one split.
// ─────────────────────────────────────────────────────────────────────────────

class LearnedIndexTest : public ::testing::Test {
protected:
    // Each test gets a fresh in-memory pool and tree.
    // We allocate the DiskManager to a temp file so the pool can evict if needed.
    // For simplicity we use a pool large enough to hold all pages in memory.

    static constexpr int kPoolSize = 512;

    void SetUp() override {}
    void TearDown() override {}

    // Build a pool + tree, insert `count` sequential keys (key=i, value=i*10).
    // Returns the tree; the caller owns pool/wal via captured unique_ptrs.
    struct Env {
        adapttree::DiskManager                          dm;
        NullWAL                                         wal;
        adapttree::BufferPool<NullWAL>                  pool;
        adapttree::BPlusTree<NullWAL>                   tree;

        explicit Env(const std::string& tmp_path, int pool_size = 512)
            : dm(tmp_path)
            , pool(&dm, &wal, static_cast<size_t>(pool_size))
            , tree(pool, wal)
        {}

        void insert_sequential(uint64_t start, uint64_t count) {
            for (uint64_t i = start; i < start + count; ++i) {
                tree.insert(i, i * 10);
            }
        }
    };

    // Helper: return path to a temp db file unique per test.
    // Use /tmp so tests run on the Linux filesystem under WSL2 (NTFS is too slow).
    static std::string tmp_db(const std::string& suffix) {
        return "/tmp/test_learned_" + suffix + ".db";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TEST 1 — LEARN-01, LEARN-02, LEARN-03
// After a leaf split, both child pages have a fitted LearnedSegment in their
// model area.  has_model == 1, error_bound == 4, inserts_since_model_rebuild == 0.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(LearnedIndexTest, ModelBuiltAfterFirstSplit) {
    // Force exactly one leaf split: insert ORDER + 1 keys.
    auto env = std::make_unique<LearnedIndexTest::Env>(tmp_db("t1"));
    const uint64_t kCount = adapttree::ORDER + 1;  // 201
    env->insert_sequential(1, kCount);

    // After the split the tree has at least 2 leaf pages.
    // Descend to the root's first two children.
    // We need to find the leaf pages by walking the tree's internal state.
    //
    // The simplest approach: after the first split, the root is an internal node
    // with two leaf children (page IDs 1 and 2, or 2 and 3 depending on allocation).
    // We know from Phase 3 code that:
    //   page 0 = meta, page 1 = initial root leaf (becomes left child after split)
    //   page 2 = new root internal,  page 3 = right child leaf
    // After the split: left=page1, right=page3, root=page2.
    // But let's not hardcode — fetch the root and walk to children.

    // For LEARN-01: the left child must have has_model == 1.
    // The left child of the first split is the original leaf (page 1 typically).

    // Fetch meta to find root
    auto meta_g = env->pool.fetchPage(adapttree::META_PAGE_ID);
    ASSERT_TRUE(meta_g.has_value());
    const auto* meta = reinterpret_cast<const adapttree::MetaPage*>(meta_g->data());
    uint32_t root_id = meta->root_page_id;
    // Root must be internal after a split
    ASSERT_GT(root_id, 0u);
    meta_g = std::nullopt;  // unpin

    auto root_g = env->pool.fetchPage(root_id);
    ASSERT_TRUE(root_g.has_value());
    const auto* root = reinterpret_cast<const adapttree::InternalNode*>(root_g->data());
    ASSERT_EQ(root->header.page_type, adapttree::PageType::INTERNAL);
    uint32_t left_id  = root->children[0];
    uint32_t right_id = root->children[1];
    root_g = std::nullopt;  // unpin

    // Fetch left child leaf
    auto left_g = env->pool.fetchPage(left_id);
    ASSERT_TRUE(left_g.has_value());
    const auto* left = reinterpret_cast<const adapttree::LeafNode*>(left_g->data());

    // LEARN-01: has_model field must be set
    EXPECT_EQ(left->model.has_model, uint8_t{1});
    // LEARN-02: error_bound == 4 (epsilon used by pgm_builder_)
    EXPECT_EQ(left->model.learnedSegment().error_bound, uint16_t{4});
    // inserts_since_model_rebuild == 0 immediately after split
    EXPECT_EQ(left->model.inserts_since_model_rebuild, uint16_t{0});
    left_g = std::nullopt;  // unpin

    // Fetch right child leaf
    auto right_g = env->pool.fetchPage(right_id);
    ASSERT_TRUE(right_g.has_value());
    const auto* right = reinterpret_cast<const adapttree::LeafNode*>(right_g->data());

    EXPECT_EQ(right->model.has_model, uint8_t{1});
    EXPECT_EQ(right->model.learnedSegment().error_bound, uint16_t{4});
    EXPECT_EQ(right->model.inserts_since_model_rebuild, uint16_t{0});
    right_g = std::nullopt;  // unpin
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2 — LEARN-04: Model-on search uses bounded window
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(LearnedIndexTest, ModelOnSearchUsesWindow) {
    auto env = std::make_unique<LearnedIndexTest::Env>(tmp_db("t2"));
    // Insert ORDER keys to force one split and build models
    const uint64_t kCount = adapttree::ORDER + 1;
    env->insert_sequential(1, kCount);

    // Enable learned index
    env->tree.use_learned_index_ = true;

    // All inserted keys must be found correctly
    for (uint64_t i = 1; i <= kCount; ++i) {
        auto result = env->tree.get(i);
        ASSERT_TRUE(result.has_value()) << "key " << i << " not found";
        EXPECT_EQ(*result, i * 10) << "wrong value for key " << i;
    }

    // For sequential keys the model should cover all without fallback
    EXPECT_EQ(env->tree.fallback_count_.load(), uint64_t{0})
        << "Sequential keys should not trigger model fallback";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3 — LEARN-04: Miss triggers fallback, fallback_count_ increments
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(LearnedIndexTest, FallbackCountIncrements) {
    auto env = std::make_unique<LearnedIndexTest::Env>(tmp_db("t3"));
    // Insert enough to split and build model
    const uint64_t kCount = adapttree::ORDER + 1;
    env->insert_sequential(1, kCount);

    env->tree.use_learned_index_ = true;

    // Look up a key that is NOT in the tree — the model will predict some slot,
    // boundedBinarySearch will miss, and fallback_count_ must increment.
    // We pick a large key guaranteed to fall outside the training set.
    uint64_t absent_key = 1'000'000'000ULL;
    auto result = env->tree.get(absent_key);
    EXPECT_FALSE(result.has_value()) << "absent key should not be found";

    // fallback_count_ must have incremented at least once
    EXPECT_GE(env->tree.fallback_count_.load(), uint64_t{1})
        << "Searching for absent key should trigger fallback";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 4 — LEARN-05: Stale model triggers full fallback without error
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(LearnedIndexTest, StaleModelFallsBackCorrectly) {
    auto env = std::make_unique<LearnedIndexTest::Env>(tmp_db("t4"));

    // Insert ORDER+1 keys to force exactly one split; both leaves get models.
    const uint64_t kBase = adapttree::ORDER + 1;
    env->insert_sequential(1, kBase);

    env->tree.use_learned_index_ = true;

    // Now insert 51 more keys so that inserts_since_model_rebuild > 50 on
    // at least one leaf.  These keys are inserted in sorted order so they go
    // to the right leaf (keys kBase+1 .. kBase+51).
    const uint64_t kExtra = 51;
    env->insert_sequential(kBase + 1, kExtra);

    // The stale leaf should fall back to full binary search — result must be correct.
    for (uint64_t i = kBase + 1; i <= kBase + kExtra; ++i) {
        auto result = env->tree.get(i);
        ASSERT_TRUE(result.has_value()) << "key " << i << " missing after stale-model insert";
        EXPECT_EQ(*result, i * 10);
    }

    // At least one of those lookups triggered the stale-model fallback path
    EXPECT_GE(env->tree.fallback_count_.load(), uint64_t{1})
        << "Stale leaf (inserts_since_model_rebuild > 50) should use fallback";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 5 — LEARN-06: Toggle produces identical results
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(LearnedIndexTest, ToggleProducesIdenticalResults) {
    auto env = std::make_unique<LearnedIndexTest::Env>(tmp_db("t5"));

    // Insert 400 keys to force 2+ splits and multiple learned models.
    const uint64_t kCount = 400;
    env->insert_sequential(1, kCount);

    std::vector<std::optional<uint64_t>> results_model_off;
    std::vector<std::optional<uint64_t>> results_model_on;

    results_model_off.reserve(kCount);
    results_model_on.reserve(kCount);

    // Collect results with model OFF
    env->tree.use_learned_index_ = false;
    for (uint64_t i = 1; i <= kCount; ++i) {
        results_model_off.push_back(env->tree.get(i));
    }

    // Collect results with model ON
    env->tree.use_learned_index_ = true;
    for (uint64_t i = 1; i <= kCount; ++i) {
        results_model_on.push_back(env->tree.get(i));
    }

    // Every result must match element-by-element
    ASSERT_EQ(results_model_off.size(), results_model_on.size());
    for (uint64_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(results_model_off[i], results_model_on[i])
            << "Mismatch at key " << (i + 1)
            << ": model_off=" << (results_model_off[i].has_value() ? std::to_string(*results_model_off[i]) : "nullopt")
            << " model_on="  << (results_model_on[i].has_value()  ? std::to_string(*results_model_on[i])  : "nullopt");
    }
}
