#include <gtest/gtest.h>
#include "adapttree/wal.hpp"
#include "adapttree/bplus_tree.hpp"
#include "adapttree/buffer_pool.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

// ── TempDir RAII helper ───────────────────────────────────────────────────────
struct TempDir {
    std::string path;

    TempDir() {
        char tmpl[] = "/tmp/adapttree_crash_XXXXXX";
        char* p = mkdtemp(tmpl);
        if (!p) throw std::runtime_error("mkdtemp failed");
        path = p;
    }

    ~TempDir() {
        for (auto suffix : {"", ".wal", ".wal.ckpt"}) {
            std::string f = path + "/tree.db" + suffix;
            unlink(f.c_str());
        }
        rmdir(path.c_str());
    }

    std::string db()  const { return path + "/tree.db"; }
    std::string wal() const { return path + "/tree.db.wal"; }
};

// ── WalCrashRecovery ──────────────────────────────────────────────────────────

TEST(WalCrashRecovery, InsertCrashRecoverRead) {
    TempDir tmp;
    constexpr int N = 50;

    // ── Phase A: Insert N keys with real WAL, then simulate crash ────────────
    // Simulate crash by heap-allocating and deliberately leaking all objects.
    // This avoids any destructor-based flushing (buffer pool flush, WAL checkpoint,
    // fsync), which is exactly what a real crash does.
    {
        // Disable auto-checkpoint background thread to avoid fsync during test.
        CheckpointPolicy no_ckpt;
        no_ckpt.max_wal_bytes = SIZE_MAX;
        no_ckpt.interval_sec  = UINT32_MAX;

        auto* dm   = new adapttree::DiskManager(tmp.db());
        auto* wal  = new Wal(tmp.wal(), no_ckpt);
        auto* pool = new adapttree::BufferPool<::Wal>(dm, wal, 128);
        auto* tree = new adapttree::BPlusTree<::Wal>(*pool, *wal);

        for (int k = 1; k <= N; ++k) {
            ASSERT_TRUE(tree->insert(static_cast<uint64_t>(k),
                                     static_cast<uint64_t>(k) * 10));
        }

        // WAL records are already in OS buffer cache via write() syscalls.
        // We skip fsync since the "crash" simulated here is process death,
        // not a power failure — OS buffer cache survives.

        // Crash: leak all objects, no destructors run (no buffer pool flush,
        // no WAL checkpoint, no fsync of data file).
        // Intentional leak — this is the crash simulation.
    }

    // Crash simulation: truncate db to PAGE_SIZE, forcing total recovery from WAL.
    // This removes all data pages, so Phase B must reconstruct them entirely from
    // WAL redo records.
    ::truncate(tmp.db().c_str(), static_cast<off_t>(adapttree::PAGE_SIZE));

    // ── Phase B: Recover using WAL ────────────────────────────────────────────
    {
        adapttree::DiskManager dm_recover(tmp.db());
        CheckpointPolicy no_auto_ckpt;
        no_auto_ckpt.max_wal_bytes = SIZE_MAX;
        no_auto_ckpt.interval_sec  = UINT32_MAX;
        Wal wal_recover(tmp.wal(), no_auto_ckpt);

        wal_recover.recover_to_disk(dm_recover);
        // All dirty page after-images are now written to tmp.db
    }

    // ── Phase C: Open fresh tree and verify all keys ──────────────────────────
    {
        adapttree::DiskManager dm_verify(tmp.db());
        CheckpointPolicy no_auto_ckpt;
        no_auto_ckpt.max_wal_bytes = SIZE_MAX;
        no_auto_ckpt.interval_sec  = UINT32_MAX;
        Wal wal_verify(tmp.wal(), no_auto_ckpt);
        adapttree::BufferPool<::Wal> pool_verify(&dm_verify, &wal_verify, 128);
        adapttree::BPlusTree<::Wal> tree_verify(pool_verify, wal_verify);

        for (int k = 1; k <= N; ++k) {
            auto val = tree_verify.get(static_cast<uint64_t>(k));
            ASSERT_TRUE(val.has_value())
                << "key " << k << " missing after crash recovery";
            EXPECT_EQ(*val, static_cast<uint64_t>(k) * 10)
                << "wrong value for key " << k;
        }
    }
}
