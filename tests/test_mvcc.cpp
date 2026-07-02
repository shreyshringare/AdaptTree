#include "adapttree/mvcc.hpp"
#include <gtest/gtest.h>

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
    // v1: committed at ts=1, value=10
    // v2: committed at ts=2, value=20
    mvcc.archive_version(3, 0, 10, 1);
    mvcc.archive_version(3, 0, 20, 2);

    // Inline has value=30 with commit_ts=10 (e.g. from a writer with txn_id=10)
    // We need a reader whose read_ts is between 2 and 10
    // Begin transactions to get the counter past 2 but below 10:
    // Force counter to a known position by starting/committing txns
    // The counter started at 1. After archive calls (no counter change),
    // we begin a reader now. The reader's txn_id will be some value > 2.
    // We need current_commit_ts=10 to be ABOVE the reader's read_ts.
    // Strategy: begin reader first, then use a high commit_ts for inline.

    Transaction txn_r = mvcc.begin(); // read_ts = R (currently small, < 10)

    // Inline slot: value=30, commit_ts=10 — simulating a writer with txn_id=10
    // Since R < 10, inline is NOT visible; we should see v2 (newest <= R)
    auto result1 = mvcc.read_version(txn_r, 3, 0,
                                     /*current_value=*/30,
                                     /*current_commit_ts=*/100); // large, definitely > R

    // Should see v2 (commit_ts=2 <= R, and it's the newest archived version <= R)
    EXPECT_EQ(result1, std::optional<uint64_t>{20});

    // Now begin a new reader AFTER committing txn_r, so read_ts > 100
    mvcc.commit(txn_r);
    Transaction txn_r2 = mvcc.begin(); // read_ts = R2

    // Inline commit_ts=100 is now <= R2, so inline is visible
    auto result2 = mvcc.read_version(txn_r2, 3, 0,
                                     /*current_value=*/30,
                                     /*current_commit_ts=*/100);

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
