#include "adapttree/bplus_tree.hpp"
#include "adapttree/mvcc.hpp"
#include "adapttree/buffer_pool.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <limits>

using namespace adapttree;

// ── MVCC_BTREE_03 ─────────────────────────────────────────────────────────────
// A snapshot taken BEFORE an insert must NOT see that insert.
TEST(MvccFullIntegration, MVCC_BTREE_03_SnapshotDoesNotSeeInsertAfter) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_full_03.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        MVCC mvcc;
        BPlusTree<NullWAL, MVCC> tree(pool, wal, mvcc);

        // Establish a snapshot before the insert.
        auto txn_reader = mvcc.begin();  // read_ts < insert commit_ts

        // Insert after the reader's snapshot.
        ASSERT_TRUE(tree.insert(42, 100));

        // The reader's snapshot predates the insert — key must not be visible.
        auto result = tree.get(42, txn_reader.read_ts);
        EXPECT_FALSE(result.has_value());

        mvcc.commit(txn_reader);
    }
    std::filesystem::remove(tmp);
}

// ── MVCC_BTREE_04 ─────────────────────────────────────────────────────────────
// A snapshot taken AFTER an insert (snapshot_ts = UINT64_MAX) MUST see the insert.
TEST(MvccFullIntegration, MVCC_BTREE_04_SnapshotSeesInsertBefore) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_full_04.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        MVCC mvcc;
        BPlusTree<NullWAL, MVCC> tree(pool, wal, mvcc);

        ASSERT_TRUE(tree.insert(42, 100));

        // UINT64_MAX sees all committed data.
        auto result = tree.get(42, std::numeric_limits<uint64_t>::max());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 100u);
    }
    std::filesystem::remove(tmp);
}

// ── MVCC_BTREE_05 ─────────────────────────────────────────────────────────────
// A snapshot taken BETWEEN insert and update must see the old value via version chain.
TEST(MvccFullIntegration, MVCC_BTREE_05_OldSnapshotSeesOldValueViaChain) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_full_05.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        MVCC mvcc;
        BPlusTree<NullWAL, MVCC> tree(pool, wal, mvcc);

        // W1: insert key 42 with value 100.
        ASSERT_TRUE(tree.insert(42, 100));

        // Snapshot after W1 but before W2.
        auto txn_old = mvcc.begin();

        // W2: update key 42 to value 200 (commit_ts > txn_old.read_ts).
        ASSERT_TRUE(tree.update(42, 200));

        // No inserts between update() and get() — prevents slot-index drift.

        // Old snapshot must return the pre-update value via version chain.
        auto result = tree.get(42, txn_old.read_ts);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 100u);

        mvcc.commit(txn_old);
    }
    std::filesystem::remove(tmp);
}

// ── MVCC_BTREE_06 ─────────────────────────────────────────────────────────────
// A snapshot taken AFTER both insert and update must see the new value inline.
TEST(MvccFullIntegration, MVCC_BTREE_06_NewSnapshotSeesNewValueInline) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_full_06.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        MVCC mvcc;
        BPlusTree<NullWAL, MVCC> tree(pool, wal, mvcc);

        ASSERT_TRUE(tree.insert(42, 100));
        ASSERT_TRUE(tree.update(42, 200));

        // UINT64_MAX snapshot sees the latest committed value.
        auto result = tree.get(42, std::numeric_limits<uint64_t>::max());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 200u);
    }
    std::filesystem::remove(tmp);
}
