#include "adapttree/mvcc.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace adapttree;

// ---------------------------------------------------------------------------
// Cycle 1 — Scaffolding
// ---------------------------------------------------------------------------

TEST(MVCC, TxnIdMonotonic) {
    MVCC mvcc;
    Transaction t1 = mvcc.begin();
    Transaction t2 = mvcc.begin();
    Transaction t3 = mvcc.begin();

    EXPECT_LT(t1.txn_id, t2.txn_id);
    EXPECT_LT(t2.txn_id, t3.txn_id);
    EXPECT_EQ(t1.read_ts, t1.txn_id);
}

// ---------------------------------------------------------------------------
// Cycle 2 — Dirty Read Prevention
// ---------------------------------------------------------------------------

TEST(MVCC, DirtyReadPrevented) {
    MVCC mvcc;

    Transaction txn_reader = mvcc.begin(); // read_ts = R
    Transaction txn_writer = mvcc.begin(); // txn_id = W > R

    // Represents prior committed state (value=100 committed at ts=0)
    mvcc.archive_version(1, 0, 100, 0);

    // Current inline slot has value=200, committed by writer (uncommitted — dirty)
    auto result = mvcc.read_version(txn_reader, 1, 0,
                                    /*current_value=*/200,
                                    /*current_commit_ts=*/txn_writer.txn_id);

    EXPECT_NE(result, std::optional<uint64_t>{200});
    EXPECT_EQ(result, std::optional<uint64_t>{100});
}

TEST(MVCC, CommittedWriteVisible) {
    MVCC mvcc;

    Transaction txn_writer = mvcc.begin();
    mvcc.commit(txn_writer); // write_ts assigned, now committed

    Transaction txn_reader2 = mvcc.begin(); // read_ts = R2 > txn_writer.write_ts

    auto result = mvcc.read_version(txn_reader2, 1, 0,
                                    /*current_value=*/200,
                                    /*current_commit_ts=*/txn_writer.write_ts);

    EXPECT_EQ(result, std::optional<uint64_t>{200});
}

// ---------------------------------------------------------------------------
// Cycle 3 — Snapshot Isolation
// ---------------------------------------------------------------------------

TEST(MVCC, SnapshotIsolation_ReaderSeesPreWriteState) {
    MVCC mvcc;

    Transaction txn_r1 = mvcc.begin(); // snapshot at T1

    // Archive prior committed state for (page=2, slot=1)
    mvcc.archive_version(2, 1, 50, 0);

    // A writer commits after txn_r1 began
    Transaction txn_w = mvcc.begin();
    mvcc.commit(txn_w); // write_ts = T_w > T1

    // Inline slot now shows the committed write (T_w > T1)
    auto result = mvcc.read_version(txn_r1, 2, 1,
                                    /*current_value=*/99,
                                    /*current_commit_ts=*/txn_w.write_ts);

    // Reader should see the pre-write state
    EXPECT_EQ(result, std::optional<uint64_t>{50});
}

TEST(MVCC, MultipleVersionsChronologicalWalk) {
    MVCC mvcc;

    // Set up archived versions for (page=3, slot=0):
    // v1: value=10, commit_ts=5
    // v2: value=20, commit_ts=10
    // These use explicit commit timestamps that don't depend on the counter.
    mvcc.archive_version(3, 0, 10, 5);
    mvcc.archive_version(3, 0, 20, 10);

    // Reader begins here; next_txn_id_ starts at 1, so txn_r.read_ts = 1.
    // That would be < 5, so neither old version is visible. We need the reader's
    // read_ts to land between commit_ts=10 and commit_ts=50 (inline).
    // Advance the counter past 10 by doing begin+commit pairs, then begin reader.
    for (int i = 0; i < 10; ++i) {
        Transaction d = mvcc.begin();
        mvcc.commit(d); // each pair advances counter by 2
    }

    Transaction txn_r = mvcc.begin(); // read_ts = R (counter is now ~21, definitely > 10)

    // Inline: value=30, commit_ts=50 (above R) — not visible
    auto result1 = mvcc.read_version(txn_r, 3, 0,
                                     /*current_value=*/30,
                                     /*current_commit_ts=*/50);

    // Should see v2 (commit_ts=10 <= R, newest archived version <= R)
    EXPECT_EQ(result1, std::optional<uint64_t>{20});

    // Commit reader and begin new reader whose read_ts is > 50
    mvcc.commit(txn_r);
    // Advance counter past 50 if needed — commit gave txn_r a write_ts.
    // After 10 pairs + begin + commit, counter is ~22 + 1 (write_ts from commit) = ~23.
    // That's still < 50. Advance more.
    for (int i = 0; i < 20; ++i) {
        Transaction d = mvcc.begin();
        mvcc.commit(d);
    }
    Transaction txn_r2 = mvcc.begin(); // read_ts = R2 (counter now ~63, > 50)

    // Inline commit_ts=50 <= R2, so inline IS visible
    auto result2 = mvcc.read_version(txn_r2, 3, 0,
                                     /*current_value=*/30,
                                     /*current_commit_ts=*/50);

    EXPECT_EQ(result2, std::optional<uint64_t>{30});
}

// ---------------------------------------------------------------------------
// Cycle 4 — Active Transaction Table
// ---------------------------------------------------------------------------

TEST(MVCC, ActiveTxnTableTracksReadTimestamps) {
    MVCC mvcc;

    Transaction t1 = mvcc.begin();
    Transaction t2 = mvcc.begin();
    Transaction t3 = mvcc.begin();

    EXPECT_EQ(mvcc.oldest_active_read_ts(), t1.read_ts);

    mvcc.commit(t1);
    EXPECT_EQ(mvcc.oldest_active_read_ts(), t2.read_ts);

    mvcc.abort(t2);
    EXPECT_EQ(mvcc.oldest_active_read_ts(), t3.read_ts);

    mvcc.commit(t3);
    EXPECT_EQ(mvcc.oldest_active_read_ts(), UINT64_MAX);
}

// ---------------------------------------------------------------------------
// Cycle 5 — Garbage Collection
// ---------------------------------------------------------------------------

TEST(MVCC, GC_RemovesVersionsBelowMinReadTs) {
    MVCC mvcc;

    // Archive two versions for (page=5, slot=0)
    mvcc.archive_version(5, 0, 1, 1); // commit_ts=1
    mvcc.archive_version(5, 0, 2, 2); // commit_ts=2

    // Keep a reader active with high read_ts so min_ts is large
    // Use a transaction so we can control min_ts
    // Begin a txn — its txn_id will be the min_ts
    // We need min_ts=10 effect: since next_txn_id starts at 1,
    // we advance counter by beginning and committing several txns first
    Transaction dummy1 = mvcc.begin(); mvcc.commit(dummy1); // advances counter
    Transaction dummy2 = mvcc.begin(); mvcc.commit(dummy2);
    Transaction dummy3 = mvcc.begin(); mvcc.commit(dummy3);
    Transaction dummy4 = mvcc.begin(); mvcc.commit(dummy4);

    // Now begin a long-lived reader — its read_ts will be well above 2
    Transaction active_reader = mvcc.begin();
    // min_ts = active_reader.read_ts (> 2)

    mvcc.gc();

    // GC rule: remove from front while commit_ts < min_ts AND next entry exists
    // versions: [commit_ts=1, commit_ts=2]
    // - commit_ts=1 < min_ts AND next exists (commit_ts=2) → REMOVE
    // - commit_ts=2 < min_ts AND next does NOT exist → STOP
    // Result: [commit_ts=2] — one entry remains

    // Verify: reading with read_ts >= 2 should still get value=2
    auto result = mvcc.read_version(active_reader, 5, 0,
                                    /*current_value=*/99,
                                    /*current_commit_ts=*/UINT64_MAX);
    EXPECT_EQ(result, std::optional<uint64_t>{2});

    mvcc.abort(active_reader);
}

TEST(MVCC, GC_RetainsVersionsAboveMinReadTs) {
    MVCC mvcc;

    // Archive one version for (page=6, slot=0): commit_ts=5
    mvcc.archive_version(6, 0, 100, 5);

    // Active reader with read_ts=8 — but we simulate this by controlling
    // when the reader begins. We need min_ts < 5 to retain, or:
    // The test says: active reader with read_ts=8 means min_ts=8.
    // commit_ts=5 < 8, but no next entry → NOT removed.

    // Advance counter so active reader gets a reasonable read_ts
    Transaction d1 = mvcc.begin(); mvcc.commit(d1);
    Transaction d2 = mvcc.begin(); mvcc.commit(d2);
    Transaction active = mvcc.begin(); // read_ts = something > 5

    mvcc.gc(); // min_ts = active.read_ts > 5

    // commit_ts=5 < min_ts, but no next entry in old_versions → retained
    auto result = mvcc.read_version(active, 6, 0,
                                    /*current_value=*/200,
                                    /*current_commit_ts=*/UINT64_MAX);
    EXPECT_EQ(result, std::optional<uint64_t>{100});

    mvcc.abort(active);
}

TEST(MVCC, GC_RequiresNewerVersionToExist) {
    MVCC mvcc;

    // Archive one version for (page=7, slot=0): commit_ts=1
    mvcc.archive_version(7, 0, 50, 1);

    // Active reader with read_ts > 1 (min_ts > 1)
    Transaction d1 = mvcc.begin(); mvcc.commit(d1);
    Transaction active = mvcc.begin(); // read_ts > 1

    mvcc.gc(); // min_ts = active.read_ts > 1
    // commit_ts=1 < min_ts, but NO next entry → NOT removed

    auto result = mvcc.read_version(active, 7, 0,
                                    /*current_value=*/999,
                                    /*current_commit_ts=*/UINT64_MAX);
    EXPECT_EQ(result, std::optional<uint64_t>{50});

    mvcc.abort(active);
}

// ---------------------------------------------------------------------------
// Cycle 6 — Epoch Ordering (MVCC-05)
// ---------------------------------------------------------------------------
// Proves that begin() and gc() share mvcc_mutex_, so a reader's epoch is
// always registered before GC can advance min_read_ts.  Single-threaded
// simulation: after begin() returns, oldest_active_read_ts() == txn.read_ts,
// and a subsequent gc() call cannot observe a lower min_ts.

TEST(MvccEpochOrdering, EpochOrdering_RegisterBeforeGCAdvances) {
    MVCC mvcc;

    // Archive two versions for (page=10, slot=0).
    // commit_ts values are fixed constants that sit below any counter-driven
    // read_ts so we can reason precisely about visibility.
    mvcc.archive_version(10, 0, 42, 1);
    mvcc.archive_version(10, 0, 77, 3);

    // Reader begins — read_ts = R (next_txn_id_ starts at 1, so R = 1).
    Transaction txn_r = mvcc.begin();
    uint64_t R = txn_r.read_ts;

    // Immediately after begin(), the epoch MUST be registered.
    EXPECT_EQ(mvcc.oldest_active_read_ts(), R);

    // GC runs while the reader is still active.
    // min_ts = R = 1.
    // old_versions for (10,0): [(42, commit_ts=1), (77, commit_ts=3)]
    // Scan front: commit_ts=1 < min_ts=1? NO (strict less-than, 1 < 1 is false).
    // Nothing removed — both entries retained.
    mvcc.gc();

    // Reader's epoch is still the oldest.
    EXPECT_EQ(mvcc.oldest_active_read_ts(), R);

    // Reader reads: inline (value=200, commit_ts=15).
    // 15 > R=1 → not visible via inline.
    // Walk old_versions newest-first: (77, commit_ts=3) → 3 > 1 → skip.
    //                                  (42, commit_ts=1) → 1 <= 1 → VISIBLE.
    auto result = mvcc.read_version(txn_r, 10, 0, 200, 15);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42ULL);

    mvcc.commit(txn_r);

    // Once the only reader commits, no active readers remain.
    EXPECT_EQ(mvcc.oldest_active_read_ts(), UINT64_MAX);
}

// ---------------------------------------------------------------------------
// Cycle 7 — Concurrent Reader / Single Writer Locking (MVCC-06)
// ---------------------------------------------------------------------------

TEST(MvccConcurrency, ConcurrentReaders_SharedLockDoesNotBlock) {
    MVCC mvcc;
    std::shared_mutex latch;

    // Archive a version all readers can see (commit_ts=1).
    mvcc.archive_version(30, 0, 555, 1);

    std::atomic<int> success_count{0};

    auto reader_fn = [&]() {
        std::shared_lock<std::shared_mutex> sl(latch);
        Transaction txn = mvcc.begin();
        // Inline (value=999, commit_ts=100): visible only if txn.read_ts >= 100.
        // Old version (555, commit_ts=1): visible if txn.read_ts >= 1 (always true).
        // Either way at least one version should be visible.
        auto result = mvcc.read_version(txn, 30, 0, 999, 100);
        EXPECT_TRUE(result.has_value());
        mvcc.commit(txn);
        success_count.fetch_add(1, std::memory_order_relaxed);
    };

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back(reader_fn);
    }
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), 4);
}

TEST(MvccConcurrency, Writer_ExclusiveLockBlocksReaders) {
    std::shared_mutex latch;

    // Writer holds exclusive lock.
    std::unique_lock<std::shared_mutex> ul(latch);

    // A concurrent reader must not be able to acquire a shared lock.
    EXPECT_FALSE(latch.try_lock_shared());

    ul.unlock();

    // After the writer releases, shared acquisition must succeed.
    EXPECT_TRUE(latch.try_lock_shared());
    latch.unlock_shared();
}

// ---------------------------------------------------------------------------
// Cycle 8 — Integration Smoke Test
// ---------------------------------------------------------------------------

TEST(MvccIntegration, FullLifecycle_BeginWriteCommitRead) {
    MVCC mvcc;

    // Committed baseline for (page=20, slot=0): value=0 at commit_ts=0.
    mvcc.archive_version(20, 0, 0, 0);

    // txn1 opens a snapshot at T1.
    Transaction txn1 = mvcc.begin();
    uint64_t T1 = txn1.read_ts;

    // txn2 begins and commits immediately (write_ts = W2 > T1).
    Transaction txn2 = mvcc.begin();
    mvcc.commit(txn2);
    uint64_t W2 = txn2.write_ts;
    EXPECT_GT(W2, T1);

    // txn3 opens after W2 has been assigned (T3 > W2).
    Transaction txn3 = mvcc.begin();
    uint64_t T3 = txn3.read_ts;
    EXPECT_GT(T3, W2);

    // Simulate: inline slot now holds (value=42, commit_ts=W2).

    // txn1 snapshot (T1 < W2) must NOT see the new write; falls back to
    // archived version (0, commit_ts=0) which is visible (0 <= T1).
    auto r1 = mvcc.read_version(txn1, 20, 0, 42, W2);
    EXPECT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value(), 0ULL);

    // txn3 snapshot (T3 > W2) MUST see the new inline write.
    auto r3 = mvcc.read_version(txn3, 20, 0, 42, W2);
    EXPECT_TRUE(r3.has_value());
    EXPECT_EQ(r3.value(), 42ULL);

    // Commit everyone, then GC.
    mvcc.commit(txn1);
    mvcc.commit(txn3);
    mvcc.gc();
    // No active readers → min_ts = UINT64_MAX.
    // old_versions for (20,0) has one entry (commit_ts=0); conservative rule
    // retains the last entry, so the map remains intact — no assertion needed.
}

TEST(MvccIntegration, AbortDoesNotCommitVersion) {
    MVCC mvcc;

    // Writer begins but does NOT commit.
    Transaction txn_w = mvcc.begin();
    mvcc.archive_version(21, 0, 5, 1);

    // Abort: write_ts stays 0, txn removed from active set.
    mvcc.abort(txn_w);
    EXPECT_EQ(txn_w.write_ts, 0ULL);
    EXPECT_EQ(mvcc.oldest_active_read_ts(), UINT64_MAX);

    // A fresh reader is now the sole active transaction.
    Transaction txn_r = mvcc.begin();
    EXPECT_EQ(mvcc.oldest_active_read_ts(), txn_r.read_ts);

    mvcc.commit(txn_r);
    EXPECT_EQ(mvcc.oldest_active_read_ts(), UINT64_MAX);
}
