#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include "adapttree/disk_manager.hpp"
#include "adapttree/page.hpp"

namespace fs = std::filesystem;

static std::string TempPath(const std::string& suffix) {
    return "/tmp/adapttree_test_" + suffix + ".db";
}

class DiskManagerTest : public ::testing::Test {
protected:
    std::string path_;

    void SetUp() override {
        path_ = TempPath(::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove(path_);
    }
    void TearDown() override {
        fs::remove(path_);
    }
};

// ── DISK-01 + DISK-02: Open and allocatePage ──────────────────────────────────

TEST_F(DiskManagerTest, OpenCreatesFile) {
    adapttree::DiskManager dm(path_);
    EXPECT_TRUE(fs::exists(path_));
}

TEST_F(DiskManagerTest, AllocateFirstPageReturnsPageId0) {
    adapttree::DiskManager dm(path_);
    uint32_t id = dm.allocatePage();
    EXPECT_EQ(id, 0u);
}

TEST_F(DiskManagerTest, AllocateSecondPageReturnsPageId1) {
    adapttree::DiskManager dm(path_);
    dm.allocatePage();
    uint32_t id = dm.allocatePage();
    EXPECT_EQ(id, 1u);
}

TEST_F(DiskManagerTest, AllocatePageGrowsFileSizeByOnePage) {
    adapttree::DiskManager dm(path_);
    dm.allocatePage();
    uintmax_t size = fs::file_size(path_);
    EXPECT_EQ(size, static_cast<uintmax_t>(PAGE_SIZE));
}

TEST_F(DiskManagerTest, AllocateTwoPagesGrowsFileSizeByTwoPages) {
    adapttree::DiskManager dm(path_);
    dm.allocatePage();
    dm.allocatePage();
    uintmax_t size = fs::file_size(path_);
    EXPECT_EQ(size, static_cast<uintmax_t>(2 * PAGE_SIZE));
}

TEST_F(DiskManagerTest, AllocateReturnsUniqueIds) {
    adapttree::DiskManager dm(path_);
    std::vector<uint32_t> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(dm.allocatePage());
    }
    std::sort(ids.begin(), ids.end());
    auto it = std::adjacent_find(ids.begin(), ids.end());
    EXPECT_EQ(it, ids.end()) << "Duplicate page ID detected";
}

// ── PAGE-03 + DISK-01: Write/read round-trip with checksum ───────────────────

TEST_F(DiskManagerTest, WriteAndReadPageRoundTrip) {
    adapttree::DiskManager dm(path_);
    uint32_t id = dm.allocatePage();

    adapttree::Page written;
    written.init(id, PageType::LEAF);
    uint8_t payload[20] = {0xCA, 0xFE, 0xBA, 0xBE};
    adapttree::insertRecord(written, payload, 20);
    written.computeChecksum();
    dm.writePage(id, written);

    adapttree::Page read_back;
    dm.readPage(id, read_back);

    EXPECT_EQ(read_back.header().page_id,   id);
    EXPECT_EQ(read_back.header().page_type, static_cast<uint8_t>(PageType::LEAF));
    EXPECT_EQ(read_back.header().num_slots, 1u);
    EXPECT_TRUE(read_back.verifyChecksum());
}

TEST_F(DiskManagerTest, CorruptionDetectedAfterRead) {
    adapttree::DiskManager dm(path_);
    uint32_t id = dm.allocatePage();

    adapttree::Page p;
    p.init(id, PageType::INTERNAL);
    p.computeChecksum();
    dm.writePage(id, p);

    int fd = ::open(path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    off_t corrupt_offset = static_cast<off_t>(id) * PAGE_SIZE + 100;
    uint8_t garbage = 0xAB;
    ::pwrite(fd, &garbage, 1, corrupt_offset);
    ::close(fd);

    adapttree::Page corrupted;
    dm.readPage(id, corrupted);
    EXPECT_FALSE(corrupted.verifyChecksum());
}

TEST_F(DiskManagerTest, MultiplePages_AllRoundTripCorrectly) {
    adapttree::DiskManager dm(path_);

    for (uint32_t i = 0; i < 5; ++i) {
        uint32_t id = dm.allocatePage();
        adapttree::Page p;
        p.init(id, PageType::LEAF);
        adapttree::insertRecord(p, &id, sizeof(id));
        p.computeChecksum();
        dm.writePage(id, p);
    }

    for (uint32_t i = 0; i < 5; ++i) {
        adapttree::Page p;
        dm.readPage(i, p);
        EXPECT_TRUE(p.verifyChecksum()) << "page " << i << " failed checksum";
        EXPECT_EQ(p.header().page_id, i);
        EXPECT_EQ(p.header().num_slots, 1u);
    }
}

TEST_F(DiskManagerTest, FlushSucceedsWithoutError) {
    adapttree::DiskManager dm(path_);
    uint32_t id = dm.allocatePage();
    adapttree::Page p;
    p.init(id, PageType::LEAF);
    dm.writePage(id, p);
    EXPECT_NO_THROW(dm.flush());
}
