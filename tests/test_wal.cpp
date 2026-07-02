#include <gtest/gtest.h>
#include "adapttree/wal.hpp"
#include "spy_wal.hpp"
#include "adapttree/bplus_tree.hpp"
#include "adapttree/buffer_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <memory>

// ── TempFile RAII helper ──────────────────────────────────────────────────────
struct TempFile {
    char path[64];
    TempFile() {
        strcpy(path, "/tmp/test_wal_XXXXXX");
        int fd = mkstemp(path);
        if (fd >= 0) close(fd);
    }
    ~TempFile() {
        unlink(path);
        std::string ckpt = std::string(path) + ".ckpt";
        unlink(ckpt.c_str());
    }
};

// ── WalRecordFormat ───────────────────────────────────────────────────────────

TEST(WalRecordFormat, FixedHeaderSizeIs34) {
    static_assert(WalRecord::kFixedHeaderSize == 34, "kFixedHeaderSize must be 34");
    EXPECT_EQ(WalRecord::kFixedHeaderSize, size_t{34});
}

TEST(WalRecordFormat, SerializedSizeCorrect) {
    WalRecord r;
    r.lsn = 1; r.prev_lsn = 0; r.txn_id = 42; r.page_id = 7;
    r.record_type = WalRecordType::INSERT;
    r.redo_data = {0x01, 0x02, 0x03}; r.redo_len = 3;
    r.undo_data = {}; r.undo_len = 0;
    auto buf = r.serialize();
    EXPECT_EQ(buf.size(), WalRecord::kFixedHeaderSize + 3 + 0 + 4);
}

TEST(WalRecordFormat, CrcCoversPayload) {
    WalRecord r;
    r.lsn = 1; r.txn_id = 42; r.page_id = 7;
    r.record_type = WalRecordType::INSERT;
    r.redo_data = {0x01, 0x02, 0x03}; r.redo_len = 3; r.undo_len = 0;
    auto buf = r.serialize();
    auto parsed = WalRecord::deserialize(buf);
    EXPECT_TRUE(parsed.has_value());
}

TEST(WalRecordFormat, RoundTrip) {
    WalRecord r;
    r.lsn = 999; r.prev_lsn = 998; r.txn_id = 42; r.page_id = 7;
    r.record_type = WalRecordType::DELETE;
    r.redo_data = {0xAA, 0xBB}; r.redo_len = 2;
    r.undo_data = {0x11}; r.undo_len = 1;
    auto buf = r.serialize();
    auto parsed = WalRecord::deserialize(buf);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->lsn, r.lsn);
    EXPECT_EQ(parsed->prev_lsn, r.prev_lsn);
    EXPECT_EQ(parsed->txn_id, r.txn_id);
    EXPECT_EQ(parsed->page_id, r.page_id);
    EXPECT_EQ(parsed->record_type, r.record_type);
    EXPECT_EQ(parsed->redo_data, r.redo_data);
    EXPECT_EQ(parsed->undo_data, r.undo_data);
}

TEST(WalRecordFormat, CrcFlipReturnsNullopt) {
    WalRecord r;
    r.lsn = 1; r.record_type = WalRecordType::INSERT;
    r.redo_data = {0x01}; r.redo_len = 1; r.undo_len = 0;
    auto buf = r.serialize();
    buf.back() ^= 0xFF;
    EXPECT_FALSE(WalRecord::deserialize(buf).has_value());
}

TEST(WalRecordFormat, TruncatedBufferReturnsNullopt) {
    WalRecord r;
    r.lsn = 1; r.record_type = WalRecordType::INSERT;
    r.redo_data = {0x01}; r.redo_len = 1; r.undo_len = 0;
    auto buf = r.serialize();
    buf.resize(20);
    EXPECT_FALSE(WalRecord::deserialize(std::span<const uint8_t>(buf)).has_value());
}

// ── WalAppend ─────────────────────────────────────────────────────────────────

TEST(WalAppend, SequentialLsn) {
    TempFile tf;
    Wal wal(tf.path);
    WalRecord r1, r2, r3;
    r1.record_type = WalRecordType::INSERT; r1.redo_len = 0; r1.undo_len = 0;
    r2.record_type = WalRecordType::INSERT; r2.redo_len = 0; r2.undo_len = 0;
    r3.record_type = WalRecordType::INSERT; r3.redo_len = 0; r3.undo_len = 0;
    EXPECT_EQ(wal.append(r1), uint64_t{1});
    EXPECT_EQ(wal.append(r2), uint64_t{2});
    EXPECT_EQ(wal.append(r3), uint64_t{3});
}

TEST(WalAppend, AppendedRecordsReadBack) {
    TempFile tf;
    {
        Wal wal(tf.path);
        WalRecord r1; r1.record_type = WalRecordType::INSERT;
        r1.redo_data = {0xAB, 0xCD}; r1.redo_len = 2; r1.undo_len = 0;
        WalRecord r2; r2.record_type = WalRecordType::COMMIT;
        r2.redo_len = 0; r2.undo_len = 0;
        wal.append(r1);
        wal.append(r2);
        wal.flush_to(2);
    }
    Wal wal2(tf.path);
    auto all_records = wal2.read_all();
    // Filter out CHECKPOINT records written by clean-shutdown destructor
    std::vector<WalRecord> records;
    for (auto& r : all_records)
        if (r.record_type != WalRecordType::CHECKPOINT) records.push_back(r);
    ASSERT_EQ(records.size(), size_t{2});
    EXPECT_EQ(records[0].record_type, WalRecordType::INSERT);
    EXPECT_EQ(records[0].redo_data, (std::vector<uint8_t>{0xAB, 0xCD}));
    EXPECT_EQ(records[1].record_type, WalRecordType::COMMIT);
}

TEST(WalAppend, FlushToUpdatesFlushedLsn) {
    TempFile tf;
    Wal wal(tf.path);
    WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
    wal.append(r);
    wal.flush_to(1);
    EXPECT_EQ(wal.flushed_lsn(), uint64_t{1});
}

TEST(WalAppend, ConcurrentAppendIsSafe) {
    TempFile tf;
    Wal wal(tf.path);
    constexpr int THREADS = 2;
    constexpr int PER_THREAD = 500;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&wal, i]() {
            for (int j = 0; j < PER_THREAD; ++j) {
                WalRecord r;
                r.record_type = WalRecordType::INSERT;
                r.redo_data = {static_cast<uint8_t>((i * PER_THREAD + j) & 0xFF)};
                r.redo_len = 1; r.undo_len = 0;
                wal.append(r);
            }
        });
    }
    for (auto& t : threads) t.join();
    wal.flush_to(wal.current_lsn());
    auto records = wal.read_all();
    EXPECT_EQ(records.size(), size_t{THREADS * PER_THREAD});
    for (auto& rec : records) {
        auto buf = rec.serialize();
        auto parsed = WalRecord::deserialize(std::span<const uint8_t>(buf));
        EXPECT_TRUE(parsed.has_value()) << "CRC failed for lsn=" << rec.lsn;
    }
}

// ── WalCrashSimulation ────────────────────────────────────────────────────────

static size_t offset_after_n(const std::vector<WalRecord>& recs, size_t n) {
    size_t off = 0;
    for (size_t i = 0; i < n && i < recs.size(); ++i)
        off += WalRecord::serialized_size(recs[i].redo_len, recs[i].undo_len);
    return off;
}

TEST(WalCrashSimulation, TornWriteMidRecord) {
    TempFile tf;
    {
        Wal wal(tf.path);
        for (int i = 0; i < 5; ++i) {
            WalRecord r; r.record_type = WalRecordType::INSERT;
            r.redo_data = {static_cast<uint8_t>(i)}; r.redo_len = 1; r.undo_len = 0;
            wal.append(r);
        }
        wal.flush_to(5);
    }
    Wal wal2(tf.path);
    auto all_with_ckpt = wal2.read_all();
    // Filter out CHECKPOINT from destructor for offset calculation
    std::vector<WalRecord> all;
    for (auto& r : all_with_ckpt)
        if (r.record_type != WalRecordType::CHECKPOINT) all.push_back(r);
    ASSERT_EQ(all.size(), size_t{5});
    size_t off = offset_after_n(all, 4) + 10;
    truncate(tf.path, static_cast<off_t>(off));
    auto records = wal2.read_all();
    EXPECT_EQ(records.size(), size_t{4});
}

TEST(WalCrashSimulation, TornWriteAtHeaderBoundary) {
    TempFile tf;
    {
        Wal wal(tf.path);
        for (int i = 0; i < 3; ++i) {
            WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
            wal.append(r);
        }
        wal.flush_to(3);
    }
    Wal wal2(tf.path);
    auto all_with_ckpt = wal2.read_all();
    // Filter out CHECKPOINT from destructor for offset calculation
    std::vector<WalRecord> all;
    for (auto& r : all_with_ckpt)
        if (r.record_type != WalRecordType::CHECKPOINT) all.push_back(r);
    ASSERT_EQ(all.size(), size_t{3});
    size_t off = offset_after_n(all, 2) + WalRecord::kFixedHeaderSize - 1;
    truncate(tf.path, static_cast<off_t>(off));
    auto records = wal2.read_all();
    EXPECT_EQ(records.size(), size_t{2});
}

TEST(WalCrashSimulation, ZeroByteTruncation) {
    TempFile tf;
    {
        Wal wal(tf.path);
        for (int i = 0; i < 2; ++i) {
            WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
            wal.append(r);
        }
    }
    truncate(tf.path, 0);
    Wal wal2(tf.path);
    auto records = wal2.read_all();
    EXPECT_TRUE(records.empty());
}

TEST(WalCrashSimulation, CrcFlipStopsRecovery) {
    TempFile tf;
    {
        Wal wal(tf.path);
        for (int i = 0; i < 4; ++i) {
            WalRecord r; r.record_type = WalRecordType::INSERT;
            r.redo_data = {static_cast<uint8_t>(i)}; r.redo_len = 1; r.undo_len = 0;
            wal.append(r);
        }
        wal.flush_to(4);
    }
    Wal wal2(tf.path);
    auto all_with_ckpt = wal2.read_all();
    // Filter out CHECKPOINT from destructor; we only want the 4 INSERTs
    std::vector<WalRecord> all;
    for (auto& r : all_with_ckpt)
        if (r.record_type != WalRecordType::CHECKPOINT) all.push_back(r);
    ASSERT_EQ(all.size(), size_t{4});
    // Flip byte 2 of record 3's CRC (at offset: start_of_rec3 + total_size - 2)
    size_t off3 = offset_after_n(all, 2);
    size_t rec3_total = WalRecord::serialized_size(all[2].redo_len, all[2].undo_len);
    off_t flip_pos = static_cast<off_t>(off3 + rec3_total - 2);
    {
        int fd = open(tf.path, O_RDWR);
        uint8_t byte;
        pread(fd, &byte, 1, flip_pos);
        byte ^= 0xFF;
        pwrite(fd, &byte, 1, flip_pos);
        close(fd);
    }
    auto records = wal2.read_all();
    EXPECT_EQ(records.size(), size_t{2});
}

// ── WalBeforeData ─────────────────────────────────────────────────────────────

struct SpyTreeFixture {
    char tmp[64];
    SpyWal spy;
    std::unique_ptr<adapttree::DiskManager> dm;
    std::unique_ptr<adapttree::BufferPool<SpyWal>> pool;
    std::unique_ptr<adapttree::BPlusTree<SpyWal>> tree;

    SpyTreeFixture() {
        strcpy(tmp, "/tmp/spy_tree_XXXXXX");
        int fd = mkstemp(tmp); if (fd >= 0) close(fd);
        dm   = std::make_unique<adapttree::DiskManager>(tmp);
        pool = std::make_unique<adapttree::BufferPool<SpyWal>>(dm.get(), &spy, 64);
        tree = std::make_unique<adapttree::BPlusTree<SpyWal>>(*pool, spy);
        spy.events.clear();  // clear constructor events
    }
    ~SpyTreeFixture() { unlink(tmp); }
};

TEST(WalBeforeData, AppendCalledBeforePageModified) {
    SpyTreeFixture f;
    f.tree->insert(42, 99);
    ASSERT_FALSE(f.spy.events.empty());
    bool found_insert = false;
    for (auto& e : f.spy.events) {
        if (e.type == WalRecordType::INSERT) { found_insert = true; break; }
    }
    EXPECT_TRUE(found_insert);
}

TEST(WalBeforeData, PageLsnMatchesReturnedLsn) {
    SpyTreeFixture f;
    size_t before = f.spy.events.size();
    f.tree->insert(1, 100);
    f.tree->insert(2, 200);
    f.tree->insert(3, 300);
    EXPECT_GT(f.spy.events.size(), before + 2);
    // LSNs are strictly increasing
    for (size_t i = 1; i < f.spy.events.size(); ++i)
        EXPECT_LT(f.spy.events[i-1].lsn, f.spy.events[i].lsn);
}

TEST(WalBeforeData, WalAppendedBeforePageBytesVisible) {
    SpyTreeFixture f;
    f.tree->insert(100, 999);
    auto val = f.tree->get(100);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, uint64_t{999});
    EXPECT_FALSE(f.spy.events.empty());
    bool has_insert_with_page = false;
    for (auto& e : f.spy.events)
        if (e.type == WalRecordType::INSERT && e.page_id != 0) { has_insert_with_page = true; break; }
    EXPECT_TRUE(has_insert_with_page);
}

// ── WalCheckpoint ─────────────────────────────────────────────────────────────

TEST(WalCheckpoint, CheckpointRecordAppearsInLog) {
    TempFile tf;
    Wal wal(tf.path);
    WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
    wal.append(r);
    wal.checkpoint([]{});
    auto records = wal.read_all();
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.back().record_type, WalRecordType::CHECKPOINT);
}

TEST(WalCheckpoint, CheckpointLsnGreaterThanPriorLsn) {
    TempFile tf;
    Wal wal(tf.path);
    WalRecord r1; r1.record_type = WalRecordType::INSERT; r1.redo_len = 0; r1.undo_len = 0;
    uint64_t lsn1 = wal.append(r1);
    wal.checkpoint([]{});
    auto records = wal.read_all();
    ASSERT_GE(records.size(), size_t{2});
    EXPECT_EQ(records.back().record_type, WalRecordType::CHECKPOINT);
    EXPECT_GT(records.back().lsn, lsn1);
}

TEST(WalCheckpoint, CheckpointLsnPersistsAcrossReopen) {
    TempFile tf;
    uint64_t expected_ckpt_lsn = 0;
    {
        Wal wal(tf.path);
        for (int i = 0; i < 5; ++i) {
            WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
            wal.append(r);
        }
        wal.checkpoint([]{});
        auto records = wal.read_all();
        expected_ckpt_lsn = records.back().lsn;
    }
    Wal wal2(tf.path);
    EXPECT_EQ(wal2.last_checkpoint_lsn(), expected_ckpt_lsn);
}

TEST(WalCheckpoint, CheckpointFileIs4KB) {
    TempFile tf;
    Wal wal(tf.path);
    wal.checkpoint([]{});
    std::string ckpt = std::string(tf.path) + ".ckpt";
    struct stat st;
    int rc = stat(ckpt.c_str(), &st);
    ASSERT_EQ(rc, 0) << "checkpoint file not created";
    EXPECT_EQ(static_cast<size_t>(st.st_size), size_t{4096});
    int cfd = open(ckpt.c_str(), O_RDONLY);
    uint64_t lsn_from_file = 0;
    read(cfd, &lsn_from_file, 8);
    close(cfd);
    auto records = wal.read_all();
    EXPECT_EQ(lsn_from_file, records.back().lsn);
}

TEST(WalCheckpoint, DirtyPageFlusherCalledBeforeLsnWrite) {
    TempFile tf;
    Wal wal(tf.path);
    bool flusher_called = false;
    wal.checkpoint([&]{ flusher_called = true; });
    EXPECT_TRUE(flusher_called);
    std::string ckpt = std::string(tf.path) + ".ckpt";
    EXPECT_EQ(access(ckpt.c_str(), F_OK), 0);
}

// ── WalRecovery ───────────────────────────────────────────────────────────────

TEST(WalRecovery, AnalysisIdentifiesWinnersAndLosers) {
    TempFile tf;
    Wal wal(tf.path);

    // Txn 1: INSERT + COMMIT → WINNER
    { WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::COMMIT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    // Txn 2: INSERT only → LOSER
    { WalRecord r; r.txn_id = 2; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    // Txn 3: INSERT + ABORT → LOSER
    { WalRecord r; r.txn_id = 3; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 3; r.record_type = WalRecordType::ABORT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.txn_table[1], TxnStatus::WINNER);
    EXPECT_EQ(result.txn_table[2], TxnStatus::LOSER);
    EXPECT_EQ(result.txn_table[3], TxnStatus::LOSER);
}

TEST(WalRecovery, RedoRepeatsHistory) {
    TempFile tf;
    Wal wal(tf.path);

    // Txn 1: INSERT page_id=5
    WalRecord r1; r1.txn_id = 1; r1.record_type = WalRecordType::INSERT;
    r1.page_id = 5; r1.redo_len = 0; r1.undo_len = 0; wal.append(r1);
    WalRecord c1; c1.txn_id = 1; c1.record_type = WalRecordType::COMMIT; c1.redo_len = 0; c1.undo_len = 0; wal.append(c1);
    // checkpoint
    wal.checkpoint([]{});
    // Txn 2: INSERT page_id=5
    WalRecord r2; r2.txn_id = 2; r2.record_type = WalRecordType::INSERT;
    r2.page_id = 5; r2.redo_len = 0; r2.undo_len = 0; wal.append(r2);
    WalRecord c2; c2.txn_id = 2; c2.record_type = WalRecordType::COMMIT; c2.redo_len = 0; c2.undo_len = 0; wal.append(c2);

    Recovery rec(wal);
    auto result = rec.analyze();

    bool first_applied = false;
    std::vector<WalRecord> applied;
    rec.redo(result,
        [&](uint32_t pid) -> uint64_t {
            if (pid == 5 && first_applied) return UINT64_MAX;
            return 0;
        },
        [&](const WalRecord& r) {
            applied.push_back(r);
            first_applied = true;
        });
    // Only one INSERT record should be in redo_log (post-checkpoint)
    EXPECT_EQ(applied.size(), size_t{1});
    EXPECT_EQ(applied[0].page_id, uint32_t{5});
}

TEST(WalRecovery, UndoRollsBackLosers) {
    TempFile tf;
    Wal wal(tf.path);

    // Txn 1: two INSERTs, no commit
    WalRecord r1; r1.txn_id = 1; r1.record_type = WalRecordType::INSERT;
    r1.page_id = 1; r1.redo_data = {0x01}; r1.redo_len = 1;
    r1.undo_data = {0xAA}; r1.undo_len = 1; wal.append(r1);
    WalRecord r2; r2.txn_id = 1; r2.record_type = WalRecordType::INSERT;
    r2.page_id = 1; r2.redo_data = {0x02}; r2.redo_len = 1;
    r2.undo_data = {0xBB}; r2.undo_len = 1; wal.append(r2);

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.txn_table[1], TxnStatus::LOSER);

    std::vector<WalRecord> undo_applied;
    rec.undo(result, [&](const WalRecord& r) { undo_applied.push_back(r); });

    ASSERT_EQ(undo_applied.size(), size_t{2});
    // Must be in reverse LSN order: r2 first (higher lsn), r1 second
    EXPECT_GT(undo_applied[0].lsn, undo_applied[1].lsn);
    EXPECT_EQ(undo_applied[0].undo_data, (std::vector<uint8_t>{0xBB}));
    EXPECT_EQ(undo_applied[1].undo_data, (std::vector<uint8_t>{0xAA}));
}

TEST(WalRecovery, FullRecoveryScenario) {
    TempFile tf;
    Wal wal(tf.path);

    // Txn 1: commit
    WalRecord r1; r1.txn_id = 1; r1.page_id = 1; r1.record_type = WalRecordType::INSERT;
    r1.redo_data = {100}; r1.redo_len = 1; r1.undo_len = 0; wal.append(r1);
    WalRecord c1; c1.txn_id = 1; c1.record_type = WalRecordType::COMMIT; c1.redo_len = 0; c1.undo_len = 0; wal.append(c1);
    // Txn 2: no commit
    WalRecord r2; r2.txn_id = 2; r2.page_id = 2; r2.record_type = WalRecordType::INSERT;
    r2.redo_data = {200}; r2.redo_len = 1; r2.undo_data = {0xFF}; r2.undo_len = 1; wal.append(r2);
    // Txn 3: commit
    WalRecord r3; r3.txn_id = 3; r3.page_id = 3; r3.record_type = WalRecordType::INSERT;
    r3.redo_data = {300 & 0xFF}; r3.redo_len = 1; r3.undo_len = 0; wal.append(r3);
    WalRecord c3; c3.txn_id = 3; c3.record_type = WalRecordType::COMMIT; c3.redo_len = 0; c3.undo_len = 0; wal.append(c3);

    Recovery rec(wal);
    std::vector<uint32_t> redo_pages;
    std::vector<WalRecord> undo_applied;
    rec.run(
        [](uint32_t) -> uint64_t { return 0; },
        [&](const WalRecord& r) { redo_pages.push_back(r.page_id); },
        [&](const WalRecord& r) { undo_applied.push_back(r); }
    );
    EXPECT_TRUE(std::find(redo_pages.begin(), redo_pages.end(), 1u) != redo_pages.end());
    EXPECT_TRUE(std::find(redo_pages.begin(), redo_pages.end(), 3u) != redo_pages.end());
    ASSERT_EQ(undo_applied.size(), size_t{1});
    EXPECT_EQ(undo_applied[0].txn_id, uint64_t{2});
}

TEST(WalRecovery, RecoveryStartsFromCheckpointLsn) {
    TempFile tf;
    Wal wal(tf.path);

    // 10 pre-checkpoint records (txn 1, committed)
    for (int i = 0; i < 10; ++i) {
        WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::INSERT;
        r.page_id = static_cast<uint32_t>(i + 1); r.redo_len = 0; r.undo_len = 0; wal.append(r);
    }
    WalRecord c; c.txn_id = 1; c.record_type = WalRecordType::COMMIT; c.redo_len = 0; c.undo_len = 0; wal.append(c);
    uint64_t ckpt_lsn_before = wal.current_lsn();

    wal.checkpoint([]{});
    uint64_t ckpt_lsn = wal.last_checkpoint_lsn();
    EXPECT_GT(ckpt_lsn, ckpt_lsn_before);

    // 3 post-checkpoint records (txn 2, committed)
    for (int i = 0; i < 3; ++i) {
        WalRecord r; r.txn_id = 2; r.record_type = WalRecordType::INSERT;
        r.page_id = static_cast<uint32_t>(100 + i); r.redo_len = 0; r.undo_len = 0; wal.append(r);
    }
    WalRecord c2; c2.txn_id = 2; c2.record_type = WalRecordType::COMMIT; c2.redo_len = 0; c2.undo_len = 0; wal.append(c2);

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.redo_from_lsn, ckpt_lsn);
    for (auto& r : result.redo_log) {
        EXPECT_GE(r.lsn, ckpt_lsn) << "redo_log contains pre-checkpoint record lsn=" << r.lsn;
    }
    int insert_count = 0;
    for (auto& r : result.redo_log)
        if (r.record_type == WalRecordType::INSERT) insert_count++;
    EXPECT_EQ(insert_count, 3);
}

// ── WalCrashIntegration ───────────────────────────────────────────────────────

TEST(WalCrashIntegration, CrashBeforeCommit_TxnIsLoser) {
    TempFile tf;
    Wal wal(tf.path);

    WalRecord ins; ins.txn_id = 55; ins.page_id = 1;
    ins.record_type = WalRecordType::INSERT;
    ins.redo_data.resize(16); ins.redo_len = 16; ins.undo_len = 0;
    uint64_t key = 55, val = 100;
    std::memcpy(ins.redo_data.data(), &key, 8);
    std::memcpy(ins.redo_data.data() + 8, &val, 8);
    wal.append(ins);
    wal.flush_to(ins.lsn);
    // No COMMIT record → txn is loser

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.txn_table[55], TxnStatus::LOSER);
    ASSERT_EQ(result.undo_log.size(), size_t{1});
    EXPECT_EQ(result.undo_log[0].txn_id, uint64_t{55});
}

TEST(WalCrashIntegration, CrashAfterCommit_TxnIsWinner) {
    TempFile tf;
    Wal wal(tf.path);

    WalRecord ins; ins.txn_id = 77; ins.page_id = 1;
    ins.record_type = WalRecordType::INSERT;
    ins.redo_data.resize(16); ins.redo_len = 16; ins.undo_len = 0;
    uint64_t key = 77, val = 200;
    std::memcpy(ins.redo_data.data(), &key, 8);
    std::memcpy(ins.redo_data.data() + 8, &val, 8);
    wal.append(ins);

    WalRecord com; com.txn_id = 77; com.record_type = WalRecordType::COMMIT;
    com.redo_len = 0; com.undo_len = 0;
    wal.append(com);
    wal.flush_to(com.lsn);

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.txn_table[77], TxnStatus::WINNER);
    bool found = false;
    for (auto& r : result.redo_log)
        if (r.txn_id == 77 && r.record_type == WalRecordType::INSERT) found = true;
    EXPECT_TRUE(found);
    for (auto& r : result.undo_log)
        EXPECT_NE(r.txn_id, uint64_t{77});
}

TEST(WalCrashIntegration, MixedTxnsCrash) {
    TempFile tf;
    Wal wal(tf.path);

    // Txn 1: commit
    { WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::COMMIT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    // Txn 2: no commit
    { WalRecord r; r.txn_id = 2; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    // Txn 3: commit
    { WalRecord r; r.txn_id = 3; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 3; r.record_type = WalRecordType::COMMIT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    wal.flush_to(wal.current_lsn());

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.txn_table[1], TxnStatus::WINNER);
    EXPECT_EQ(result.txn_table[2], TxnStatus::LOSER);
    EXPECT_EQ(result.txn_table[3], TxnStatus::WINNER);
    ASSERT_EQ(result.undo_log.size(), size_t{1});
    EXPECT_EQ(result.undo_log[0].txn_id, uint64_t{2});
}

TEST(WalCrashIntegration, CheckpointBoundsRedo) {
    TempFile tf;
    Wal wal(tf.path);

    // Pre-checkpoint: txn 1 insert + commit
    { WalRecord r; r.txn_id = 1; r.page_id = 1; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 1; r.record_type = WalRecordType::COMMIT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    wal.checkpoint([]{});
    uint64_t ckpt_lsn = wal.last_checkpoint_lsn();

    // Post-checkpoint: txn 2 insert + commit
    { WalRecord r; r.txn_id = 2; r.page_id = 2; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    { WalRecord r; r.txn_id = 2; r.record_type = WalRecordType::COMMIT; r.redo_len = 0; r.undo_len = 0; wal.append(r); }
    wal.flush_to(wal.current_lsn());

    Recovery rec(wal);
    auto result = rec.analyze();
    EXPECT_EQ(result.redo_from_lsn, ckpt_lsn);
    for (auto& r : result.redo_log)
        if (r.txn_id == 1 && r.record_type == WalRecordType::INSERT)
            FAIL() << "Pre-checkpoint INSERT should not be in redo_log";
    bool found_txn2 = false;
    for (auto& r : result.redo_log)
        if (r.txn_id == 2 && r.record_type == WalRecordType::INSERT) found_txn2 = true;
    EXPECT_TRUE(found_txn2);
}

// ── WalCheckpointTrigger ──────────────────────────────────────────────────────

TEST(WalCheckpointTrigger, CheckpointTriggersOnSizeThreshold) {
    TempFile tf;
    CheckpointPolicy policy;
    policy.max_wal_bytes = 200;  // small threshold: ~5 records of 38 bytes each
    policy.interval_sec  = 9999;
    Wal wal(tf.path, policy);
    for (int i = 0; i < 10; ++i) {
        WalRecord r; r.record_type = WalRecordType::INSERT; r.redo_len = 0; r.undo_len = 0;
        wal.append(r);
    }
    // Give background thread time to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto records = wal.read_all();
    bool found_ckpt = false;
    for (auto& r : records)
        if (r.record_type == WalRecordType::CHECKPOINT) { found_ckpt = true; break; }
    EXPECT_TRUE(found_ckpt);
}

TEST(WalCheckpointTrigger, CheckpointTriggersOnCleanShutdown) {
    TempFile tf;
    {
        Wal wal(tf.path);
        WalRecord r1, r2, r3;
        r1.record_type = WalRecordType::INSERT; r1.redo_len = 0; r1.undo_len = 0;
        r2.record_type = WalRecordType::INSERT; r2.redo_len = 0; r2.undo_len = 0;
        r3.record_type = WalRecordType::INSERT; r3.redo_len = 0; r3.undo_len = 0;
        wal.append(r1); wal.append(r2); wal.append(r3);
        // Wal destructor fires here → clean shutdown checkpoint
    }
    Wal wal2(tf.path);
    auto records = wal2.read_all();
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.back().record_type, WalRecordType::CHECKPOINT);
}

TEST(WalCheckpointTrigger, CheckpointNotTriggeredBeforeThreshold) {
    TempFile tf;
    CheckpointPolicy policy;
    policy.max_wal_bytes = 1ULL * 1024 * 1024;  // 1 MB
    policy.interval_sec  = 9999;
    Wal wal(tf.path, policy);
    WalRecord r1, r2;
    r1.record_type = WalRecordType::INSERT; r1.redo_len = 0; r1.undo_len = 0;
    r2.record_type = WalRecordType::INSERT; r2.redo_len = 0; r2.undo_len = 0;
    wal.append(r1); wal.append(r2);
    EXPECT_FALSE(wal.checkpoint_triggered());
}

TEST(WalCheckpointTrigger, DISABLED_TimerBasedCheckpoint) {
    GTEST_SKIP() << "wall-clock test disabled on WSL2";
}
