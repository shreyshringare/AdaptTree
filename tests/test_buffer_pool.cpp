#include <gtest/gtest.h>
#include "adapttree/buffer_pool.hpp"
#include <filesystem>
#include <cstring>
#include <vector>
#include <algorithm>
#include <limits>
#include <span>

// ─── NullWAL stub ────────────────────────────────────────────────────────────
struct NullWAL {
    uint64_t flushed_lsn = std::numeric_limits<uint64_t>::max();
    uint64_t append(std::span<const std::byte>) { return 0; }
    void flush_to(uint64_t) {}
};

// ─── Test fixture ─────────────────────────────────────────────────────────────
class BufferPoolTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<adapttree::DiskManager> dm_;
    std::unique_ptr<adapttree::BufferPool<NullWAL>> pool_;
    NullWAL wal_;

    static constexpr size_t POOL_SIZE = 4;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "buf_pool_test.db";
        std::filesystem::remove(db_path_);
        dm_   = std::make_unique<adapttree::DiskManager>(db_path_.string());
        pool_ = std::make_unique<adapttree::BufferPool<NullWAL>>(dm_.get(), &wal_, POOL_SIZE);
    }

    void TearDown() override {
        pool_.reset();
        dm_.reset();
        std::filesystem::remove(db_path_);
    }
};

// ── BUF-01 ────────────────────────────────────────────────────────────────────
TEST_F(BufferPoolTest, BUF01_FixedPoolSize_ExhaustsWhenAllPinned) {
    std::vector<adapttree::PageGuard> guards;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value()) << "newPage() failed at i=" << i;
        guards.push_back(std::move(*g));
    }
    auto extra = pool_->newPage();
    EXPECT_FALSE(extra.has_value()) << "Expected nullopt when pool full and all pinned";
}

// ── BUF-02 ────────────────────────────────────────────────────────────────────
TEST_F(BufferPoolTest, BUF02_LRUK_SingleAccessEvictedBeforeDoubleAccess) {
    std::vector<adapttree::page_id_t> ids;
    {
        std::vector<adapttree::PageGuard> guards;
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            auto g = pool_->newPage();
            ASSERT_TRUE(g.has_value());
            ids.push_back(g->page_id());
            guards.push_back(std::move(*g));
        }
    }
    // Give ids[0] a second access
    { auto g = pool_->fetchPage(ids[0]); ASSERT_TRUE(g.has_value()); }

    // New page must evict one of ids[1..3], NOT ids[0]
    auto new_g = pool_->newPage();
    ASSERT_TRUE(new_g.has_value());

    auto still_there = pool_->fetchPage(ids[0]);
    EXPECT_TRUE(still_there.has_value()) << "Page with 2 accesses must not be evicted";
}

TEST_F(BufferPoolTest, BUF02_LRUK_TieBreak_EarliestTLastEvictedFirst) {
    std::vector<adapttree::page_id_t> ids;
    {
        std::vector<adapttree::PageGuard> guards;
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            auto g = pool_->newPage();
            ASSERT_TRUE(g.has_value());
            ids.push_back(g->page_id());
            guards.push_back(std::move(*g));
        }
    }
    // All single-access — ids[0] has earliest t_last, should be evicted
    auto new_g = pool_->newPage();
    ASSERT_TRUE(new_g.has_value());

    for (int i = 1; i < 4; ++i) {
        auto g = pool_->fetchPage(ids[i]);
        EXPECT_TRUE(g.has_value()) << "Page " << i << " should not have been evicted";
    }
}

// ── BUF-03 + BUF-04 ──────────────────────────────────────────────────────────
TEST_F(BufferPoolTest, BUF03_PageGuard_AutoUnpinsOnDestruction) {
    {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value());
    }
    // Frame is unpinned — can now allocate POOL_SIZE more
    std::vector<adapttree::PageGuard> guards;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value()) << "PageGuard did not unpin at i=" << i;
        guards.push_back(std::move(*g));
    }
}

TEST_F(BufferPoolTest, BUF03_PageGuard_MarkDirty_SetsDirtyFlag) {
    adapttree::page_id_t pid;
    {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value());
        pid = g->page_id();
        g->markDirty();
    }
    // Evict dirty page — WAL-before-data assert must not fire
    std::vector<adapttree::PageGuard> guards;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value()) << "Failed at i=" << i;
        guards.push_back(std::move(*g));
    }
}

TEST_F(BufferPoolTest, BUF04_PageGuard_MovedFrom_HoldsNull_DoesNotUnpin) {
    auto g1 = pool_->newPage();
    ASSERT_TRUE(g1.has_value());
    adapttree::page_id_t pid = g1->page_id();

    adapttree::PageGuard g2 = std::move(*g1);
    EXPECT_EQ(g2.page_id(), pid);

    { adapttree::PageGuard moved_into = std::move(g2); }

    // If double-unpin: pin_count underflows, pool thinks frame still pinned
    std::vector<adapttree::PageGuard> guards;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value()) << "Double-unpin at i=" << i;
        guards.push_back(std::move(*g));
    }
}

TEST_F(BufferPoolTest, BUF04_PageGuard_MoveAssignment_NullsSource) {
    auto g1_opt = pool_->newPage();
    auto g2_opt = pool_->newPage();
    ASSERT_TRUE(g1_opt.has_value());
    ASSERT_TRUE(g2_opt.has_value());

    adapttree::PageGuard g1 = std::move(*g1_opt);
    adapttree::PageGuard g2 = std::move(*g2_opt);
    adapttree::page_id_t pid2 = g2.page_id();

    g1 = std::move(g2);
    EXPECT_EQ(g1.page_id(), pid2);
}

// ── BUF-05 ────────────────────────────────────────────────────────────────────
TEST_F(BufferPoolTest, BUF05_FetchPage_CacheHit_ReturnsSameData) {
    auto g1 = pool_->newPage();
    ASSERT_TRUE(g1.has_value());
    adapttree::page_id_t pid = g1->page_id();

    static const uint8_t MAGIC = 0xAB;
    std::memset(g1->data(), MAGIC, adapttree::PAGE_SIZE);
    g1->markDirty();
    { adapttree::PageGuard tmp = std::move(*g1); }

    auto g2 = pool_->fetchPage(pid);
    ASSERT_TRUE(g2.has_value());
    EXPECT_EQ(static_cast<uint8_t>(g2->data()[0]), MAGIC);
}

TEST_F(BufferPoolTest, BUF05_FetchPage_DiskLoad_AfterEviction) {
    adapttree::page_id_t pid;
    {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value());
        pid = g->page_id();
        static const uint8_t MAGIC = 0xCD;
        std::memset(g->data(), MAGIC, adapttree::PAGE_SIZE);
        g->markDirty();
    }
    // Force eviction
    {
        std::vector<adapttree::PageGuard> guards;
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            auto g = pool_->newPage();
            ASSERT_TRUE(g.has_value());
            guards.push_back(std::move(*g));
        }
    }
    auto g2 = pool_->fetchPage(pid);
    ASSERT_TRUE(g2.has_value());
    EXPECT_EQ(static_cast<uint8_t>(g2->data()[0]), static_cast<uint8_t>(0xCD))
        << "Page bytes not preserved across eviction and reload";
}

// ── BUF-06 ────────────────────────────────────────────────────────────────────
TEST_F(BufferPoolTest, BUF06_NewPage_ReturnsUniquePinnedGuards) {
    std::vector<adapttree::page_id_t> seen;
    std::vector<adapttree::PageGuard> guards;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto g = pool_->newPage();
        ASSERT_TRUE(g.has_value());
        auto pid = g->page_id();
        EXPECT_EQ(std::count(seen.begin(), seen.end(), pid), 0)
            << "Duplicate page_id from newPage()";
        seen.push_back(pid);
        guards.push_back(std::move(*g));
    }
}

// ── BUF-07 ────────────────────────────────────────────────────────────────────
namespace {
struct TrackingWAL {
    uint64_t flushed_lsn = std::numeric_limits<uint64_t>::max();
    std::vector<std::string> call_log;
    uint64_t append(std::span<const std::byte>) {
        call_log.push_back("append");
        return 1;
    }
    void flush_to(uint64_t lsn) {
        call_log.push_back("flush_to:" + std::to_string(lsn));
        flushed_lsn = lsn < flushed_lsn ? flushed_lsn : lsn;
    }
};
}

TEST(BufferPoolWALOrderTest, BUF07_DirtyEviction_WALFlushBeforeWritePage) {
    auto db_path = std::filesystem::temp_directory_path() / "buf_wal_order_test.db";
    std::filesystem::remove(db_path);

    TrackingWAL wal;
    adapttree::DiskManager dm(db_path.string());
    adapttree::BufferPool<TrackingWAL> pool(&dm, &wal, 1);

    adapttree::page_id_t pid;
    {
        auto g = pool.newPage();
        ASSERT_TRUE(g.has_value());
        pid = g->page_id();
        g->markDirty();
        g->setPageLsn(42);
    }

    wal.call_log.clear();

    {
        auto g2 = pool.newPage();
        ASSERT_TRUE(g2.has_value());
    }

    bool found_flush = false;
    for (const auto& call : wal.call_log) {
        if (call.find("flush_to") != std::string::npos) {
            auto colon = call.find(':');
            uint64_t flushed = std::stoull(call.substr(colon + 1));
            EXPECT_GE(flushed, static_cast<uint64_t>(42))
                << "flush_to lsn < page_lsn — WAL-before-data violated";
            found_flush = true;
        }
    }
    EXPECT_TRUE(found_flush) << "flush_to never called during dirty eviction";
    std::filesystem::remove(db_path);
}
