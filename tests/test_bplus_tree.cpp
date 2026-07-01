#include <gtest/gtest.h>
#include "adapttree/wal.hpp"
#include "adapttree/mvcc.hpp"
#include "adapttree/bplus_tree.hpp"

#include <filesystem>
#include <algorithm>
#include <random>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Task 1 — NullWAL and NullMvcc unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(NullWALTest, NWAL01_AppendReturnsUINT64MAX) {
    NullWAL wal;
    uint64_t lsn = wal.append(42, nullptr, 0);
    EXPECT_EQ(lsn, UINT64_MAX);
}

TEST(NullWALTest, NWAL02_FlushToIsNoOp) {
    NullWAL wal;
    EXPECT_NO_THROW(wal.flush_to(999));
}

TEST(NullWALTest, NWAL03_FlushedLsnReturnsUINT64MAX) {
    NullWAL wal;
    EXPECT_EQ(wal.flushed_lsn(), UINT64_MAX);
}

TEST(NullMvccTest, NMVCC01_BeginReturnsOne) {
    NullMvcc mvcc;
    EXPECT_EQ(mvcc.begin(), uint64_t{1});
}

TEST(NullMvccTest, NMVCC02_IsVisibleAlwaysTrue) {
    NullMvcc mvcc;
    EXPECT_TRUE(mvcc.is_visible(0, 0));
    EXPECT_TRUE(mvcc.is_visible(100, 50));
    EXPECT_TRUE(mvcc.is_visible(UINT64_MAX, 0));
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 2 — Layout static assertions (compile-time; also runtime-checked here)
// ─────────────────────────────────────────────────────────────────────────────

TEST(BPlusTreeLayoutTest, LAYOUT01_BPHeaderIs32Bytes) {
    EXPECT_EQ(sizeof(adapttree::BPHeader), size_t{32});
}

TEST(BPlusTreeLayoutTest, LAYOUT02_LeafNodeIsPageSize) {
    EXPECT_EQ(sizeof(adapttree::LeafNode), adapttree::PAGE_SIZE);
}

TEST(BPlusTreeLayoutTest, LAYOUT03_InternalNodeIsPageSize) {
    EXPECT_EQ(sizeof(adapttree::InternalNode), adapttree::PAGE_SIZE);
}

TEST(BPlusTreeLayoutTest, LAYOUT04_MetaPageIsPageSize) {
    EXPECT_EQ(sizeof(adapttree::MetaPage), adapttree::PAGE_SIZE);
}

TEST(BPlusTreeLayoutTest, LAYOUT05_LeafEntryIs16Bytes) {
    EXPECT_EQ(sizeof(adapttree::LeafEntry), size_t{16});
}

TEST(BPlusTreeLayoutTest, LAYOUT06_BPHeaderFieldOffsets) {
    EXPECT_EQ(offsetof(adapttree::BPHeader, page_id),      size_t{ 0});
    EXPECT_EQ(offsetof(adapttree::BPHeader, crc32),        size_t{ 4});
    EXPECT_EQ(offsetof(adapttree::BPHeader, num_slots),    size_t{ 8});
    EXPECT_EQ(offsetof(adapttree::BPHeader, free_start),   size_t{10});
    EXPECT_EQ(offsetof(adapttree::BPHeader, free_end),     size_t{12});
    EXPECT_EQ(offsetof(adapttree::BPHeader, page_type),    size_t{14});
    EXPECT_EQ(offsetof(adapttree::BPHeader, flags),        size_t{15});
    EXPECT_EQ(offsetof(adapttree::BPHeader, next_leaf_id), size_t{16});
    EXPECT_EQ(offsetof(adapttree::BPHeader, lsn),         size_t{20});
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixture — all tree-level tests
// ─────────────────────────────────────────────────────────────────────────────

class BPlusTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = "/tmp/adapttree_bptree_test.db";
        std::filesystem::remove(path_);
        dm_   = std::make_unique<adapttree::DiskManager>(path_);
        pool_ = std::make_unique<adapttree::BufferPool<NullWAL>>(dm_.get(), &wal_, 64);
        tree_ = std::make_unique<adapttree::BPlusTree<NullWAL>>(*pool_, wal_);
    }

    void TearDown() override {
        tree_.reset();
        pool_.reset();
        dm_.reset();
        std::filesystem::remove(path_);
    }

    std::string path_;
    NullWAL wal_;
    std::unique_ptr<adapttree::DiskManager>              dm_;
    std::unique_ptr<adapttree::BufferPool<NullWAL>>      pool_;
    std::unique_ptr<adapttree::BPlusTree<NullWAL>>       tree_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Task 3 — Single-leaf insert (no splits)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, INS01_InsertSingleKey) {
    EXPECT_TRUE(tree_->insert(42, 100));
    auto v = tree_->get(42);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, uint64_t{100});
}

TEST_F(BPlusTreeTest, INS02_InsertDuplicateRejected) {
    EXPECT_TRUE(tree_->insert(1, 10));
    EXPECT_FALSE(tree_->insert(1, 20)) << "Duplicate key must be rejected";
    auto v = tree_->get(1);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, uint64_t{10}) << "Original value must be unchanged";
}

TEST_F(BPlusTreeTest, INS03_InsertMultipleKeysSorted) {
    // Insert in reverse order — tree must keep sorted order
    for (int i = 10; i >= 1; --i) {
        EXPECT_TRUE(tree_->insert(static_cast<uint64_t>(i),
                                  static_cast<uint64_t>(i * 10)));
    }
    for (int i = 1; i <= 10; ++i) {
        auto v = tree_->get(static_cast<uint64_t>(i));
        ASSERT_TRUE(v.has_value()) << "key=" << i;
        EXPECT_EQ(*v, static_cast<uint64_t>(i * 10));
    }
}

TEST_F(BPlusTreeTest, INS04_InitialHeightIsOne) {
    EXPECT_EQ(tree_->height(), uint32_t{1});
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 4 — get() edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, GET01_EmptyTreeReturnsNullopt) {
    EXPECT_FALSE(tree_->get(0).has_value());
    EXPECT_FALSE(tree_->get(UINT64_MAX).has_value());
}

TEST_F(BPlusTreeTest, GET02_KeyZeroInsertAndLookup) {
    EXPECT_TRUE(tree_->insert(0, 999));
    auto v = tree_->get(0);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, uint64_t{999});
}

TEST_F(BPlusTreeTest, GET03_MaxKeyInsertAndLookup) {
    uint64_t max_key = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tree_->insert(max_key, 77));
    auto v = tree_->get(max_key);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, uint64_t{77});
}

TEST_F(BPlusTreeTest, GET04_MissingKeyReturnsNullopt) {
    EXPECT_TRUE(tree_->insert(10, 100));
    EXPECT_TRUE(tree_->insert(20, 200));
    EXPECT_FALSE(tree_->get(15).has_value());
    EXPECT_FALSE(tree_->get(0).has_value());
    EXPECT_FALSE(tree_->get(25).has_value());
}

TEST_F(BPlusTreeTest, GET05_BoundaryValues) {
    EXPECT_TRUE(tree_->insert(1, 11));
    EXPECT_TRUE(tree_->insert(UINT64_MAX - 1, 42));
    EXPECT_FALSE(tree_->get(0).has_value());
    EXPECT_FALSE(tree_->get(2).has_value());
    auto v1 = tree_->get(1);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, uint64_t{11});
    auto v2 = tree_->get(UINT64_MAX - 1);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, uint64_t{42});
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 5 — Leaf split (ORDER+1 = 201 inserts triggers first split)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, SPLIT01_LeafSplitOccurs) {
    // Fill leaf completely (ORDER = 200 entries)
    for (uint64_t i = 0; i < adapttree::ORDER; ++i) {
        ASSERT_TRUE(tree_->insert(i, i * 2));
    }
    EXPECT_EQ(tree_->height(), uint32_t{1}) << "No split yet at exactly ORDER entries";

    // One more insert must trigger a leaf split, creating a root internal node
    ASSERT_TRUE(tree_->insert(adapttree::ORDER, adapttree::ORDER * 2));
    EXPECT_EQ(tree_->height(), uint32_t{2}) << "Height must grow to 2 after first split";
}

TEST_F(BPlusTreeTest, SPLIT02_AllKeysAccessibleAfterLeafSplit) {
    for (uint64_t i = 0; i <= adapttree::ORDER; ++i) {
        ASSERT_TRUE(tree_->insert(i, i + 1000));
    }
    for (uint64_t i = 0; i <= adapttree::ORDER; ++i) {
        auto v = tree_->get(i);
        ASSERT_TRUE(v.has_value()) << "key=" << i << " not found after split";
        EXPECT_EQ(*v, i + 1000);
    }
}

TEST_F(BPlusTreeTest, SPLIT03_FenceKeyCorrectAfterSplit) {
    // Insert in descending order to stress fence key capture
    for (int64_t i = static_cast<int64_t>(adapttree::ORDER); i >= 0; --i) {
        ASSERT_TRUE(tree_->insert(static_cast<uint64_t>(i),
                                  static_cast<uint64_t>(i) * 3));
    }
    EXPECT_EQ(tree_->height(), uint32_t{2});
    for (uint64_t i = 0; i <= adapttree::ORDER; ++i) {
        auto v = tree_->get(i);
        ASSERT_TRUE(v.has_value()) << "key=" << i;
        EXPECT_EQ(*v, i * 3);
    }
}

TEST_F(BPlusTreeTest, SPLIT04_DuplicateRejectedAfterSplit) {
    for (uint64_t i = 0; i <= adapttree::ORDER; ++i) {
        ASSERT_TRUE(tree_->insert(i, i));
    }
    // Duplicates into both the left and right leaves
    EXPECT_FALSE(tree_->insert(0, 999));
    EXPECT_FALSE(tree_->insert(adapttree::SPLIT_HALF, 999));
    EXPECT_FALSE(tree_->insert(adapttree::ORDER, 999));
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 6 — Internal node split (enough inserts to overflow an internal node)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, INTSPLIT01_HeightGrowsToThree) {
    // Each leaf holds ORDER entries.  After ORDER leaf-splits the root internal
    // node holds ORDER keys (ORDER+1 children) and itself splits, growing the
    // tree to height 3.
    // Total inserts needed: ORDER * (ORDER+1) + 1
    //   = 200 * 201 + 1 = 40201 inserts
    // Use a pool of 64 frames — should be sufficient with LRU-K eviction.
    const uint64_t N = static_cast<uint64_t>(adapttree::ORDER) *
                       (adapttree::ORDER + 1) + 1;
    for (uint64_t i = 0; i < N; ++i) {
        ASSERT_TRUE(tree_->insert(i, i)) << "insert failed at i=" << i;
    }
    EXPECT_GE(tree_->height(), uint32_t{3})
        << "Height must be at least 3 after " << N << " sequential inserts";
    for (uint64_t i = 0; i < N; ++i) {
        auto v = tree_->get(i);
        ASSERT_TRUE(v.has_value()) << "key=" << i << " missing after internal split";
        EXPECT_EQ(*v, i);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 7 — Bulk insert: 1000 sequential keys + duplicate rejection
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, BULK01_1000SequentialKeys) {
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree_->insert(static_cast<uint64_t>(i),
                                  static_cast<uint64_t>(i) * 7))
            << "insert failed at i=" << i;
    }
    for (int i = 0; i < N; ++i) {
        auto v = tree_->get(static_cast<uint64_t>(i));
        ASSERT_TRUE(v.has_value()) << "key=" << i;
        EXPECT_EQ(*v, static_cast<uint64_t>(i) * 7);
    }
}

TEST_F(BPlusTreeTest, BULK02_DuplicateRejectionIn1000Keys) {
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree_->insert(static_cast<uint64_t>(i),
                                  static_cast<uint64_t>(i)));
    }
    // All duplicates must be rejected and original values preserved
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(tree_->insert(static_cast<uint64_t>(i), 9999))
            << "Duplicate accepted at i=" << i;
        auto v = tree_->get(static_cast<uint64_t>(i));
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, static_cast<uint64_t>(i)) << "Value corrupted at i=" << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 8 — Random key bulk insert (1000 keys) including key=0 edge case
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(BPlusTreeTest, BULK03_1000RandomKeys) {
    const int N = 1000;
    std::mt19937_64 rng(0xDEADBEEF42ULL);
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

    std::vector<uint64_t> keys;
    keys.reserve(N);

    // Generate N unique random keys
    while (static_cast<int>(keys.size()) < N) {
        uint64_t k = dist(rng);
        // Cheap duplicate-in-batch check: sort and unique after generation
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // Ensure we still have enough (very unlikely to collide but handle it)
    while (static_cast<int>(keys.size()) < N) {
        keys.push_back(keys.back() + 1);
    }
    keys.resize(N);

    // Shuffle before inserting
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree_->insert(keys[i], keys[i] ^ 0xCAFEBABE))
            << "insert failed at i=" << i << " key=" << keys[i];
    }
    for (int i = 0; i < N; ++i) {
        auto v = tree_->get(keys[i]);
        ASSERT_TRUE(v.has_value()) << "key=" << keys[i] << " not found";
        EXPECT_EQ(*v, keys[i] ^ 0xCAFEBABE);
    }
}

TEST_F(BPlusTreeTest, BULK04_KeyZeroInRandomBatch) {
    // Ensure key=0 works correctly even in a random context
    const int N = 500;
    std::vector<uint64_t> keys;
    keys.push_back(0);  // force key=0 into the set
    for (int i = 1; i < N; ++i) {
        keys.push_back(static_cast<uint64_t>(i) * 3);
    }
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (auto k : keys) {
        ASSERT_TRUE(tree_->insert(k, k + 1));
    }
    for (auto k : keys) {
        auto v = tree_->get(k);
        ASSERT_TRUE(v.has_value()) << "key=" << k;
        EXPECT_EQ(*v, k + 1);
    }
    // Specific check for key=0
    auto v0 = tree_->get(0);
    ASSERT_TRUE(v0.has_value());
    EXPECT_EQ(*v0, uint64_t{1});
}
