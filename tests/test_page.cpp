#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include "adapttree/page.hpp"

// ── PAGE-01: Header layout ────────────────────────────────────────────────────

TEST(PageHeader, SizeIs32Bytes) {
    EXPECT_EQ(sizeof(PageHeader), 32u);
}

TEST(PageHeader, Offsets) {
    EXPECT_EQ(offsetof(PageHeader, page_id),      0u);
    EXPECT_EQ(offsetof(PageHeader, crc32),         4u);
    EXPECT_EQ(offsetof(PageHeader, num_slots),     8u);
    EXPECT_EQ(offsetof(PageHeader, free_start),   10u);
    EXPECT_EQ(offsetof(PageHeader, free_end),     12u);
    EXPECT_EQ(offsetof(PageHeader, page_type),    14u);
    EXPECT_EQ(offsetof(PageHeader, flags),        15u);
    EXPECT_EQ(offsetof(PageHeader, next_leaf_id), 16u);
    EXPECT_EQ(offsetof(PageHeader, lsn),          20u);
}

// ── PAGE-05: PageType enum ────────────────────────────────────────────────────

TEST(PageTypeEnum, Values) {
    EXPECT_EQ(static_cast<uint8_t>(PageType::INVALID),  0u);
    EXPECT_EQ(static_cast<uint8_t>(PageType::INTERNAL), 1u);
    EXPECT_EQ(static_cast<uint8_t>(PageType::LEAF),     2u);
    EXPECT_EQ(static_cast<uint8_t>(PageType::FREE),     3u);
}

// ── PAGE-01: Page constants ───────────────────────────────────────────────────

TEST(PageConstants, PageSizeIs4096) {
    EXPECT_EQ(PAGE_SIZE, 4096u);
}

TEST(PageConstants, SlotDirectoryStartAfterHeaderAndModelArea) {
    EXPECT_EQ(SLOT_DIR_OFFSET, 64u);
}

TEST(PageConstants, UsableDataRegion) {
    EXPECT_EQ(PAGE_USABLE, 4032u);
}

// ── PAGE-03: CRC32 ────────────────────────────────────────────────────────────

TEST(CRC32, KnownValue_EmptyBuffer) {
    EXPECT_EQ(adapttree::crc32_compute(nullptr, 0), 0x00000000u);
}

TEST(CRC32, KnownValue_SingleZeroByte) {
    uint8_t buf[1] = {0x00};
    EXPECT_EQ(adapttree::crc32_compute(buf, 1), 0xD202EF8Du);
}

TEST(CRC32, KnownValue_ABC) {
    uint8_t buf[3] = {'A', 'B', 'C'};
    EXPECT_EQ(adapttree::crc32_compute(buf, 3), 0xA3830348u);
}

TEST(CRC32, KnownValue_123456789) {
    const uint8_t* msg = reinterpret_cast<const uint8_t*>("123456789");
    EXPECT_EQ(adapttree::crc32_compute(msg, 9), 0xCBF43926u);
}

TEST(CRC32, DetectsFlippedBit) {
    uint8_t original[16] = {};
    uint8_t corrupted[16] = {};
    corrupted[7] ^= 0x01;
    EXPECT_NE(adapttree::crc32_compute(original, 16),
              adapttree::crc32_compute(corrupted, 16));
}

// ── PAGE-01 + PAGE-03: Page class ────────────────────────────────────────────

TEST(Page, InitSetsHeaderFieldsAndFreePointers) {
    adapttree::Page p;
    p.init(42, PageType::LEAF);

    const PageHeader& hdr = p.header();
    EXPECT_EQ(hdr.page_id,      42u);
    EXPECT_EQ(hdr.num_slots,    0u);
    EXPECT_EQ(hdr.free_start,   SLOT_DIR_OFFSET);
    EXPECT_EQ(hdr.free_end,     PAGE_SIZE);
    EXPECT_EQ(hdr.page_type,    static_cast<uint8_t>(PageType::LEAF));
    EXPECT_EQ(hdr.flags,        0u);
    EXPECT_EQ(hdr.next_leaf_id, 0u);
    EXPECT_EQ(hdr.lsn,          0u);
}

TEST(Page, ComputeChecksumAndVerify_RoundTrip) {
    adapttree::Page p;
    p.init(1, PageType::INTERNAL);
    p.computeChecksum();
    EXPECT_TRUE(p.verifyChecksum());
}

TEST(Page, CorruptionDetectedByVerifyChecksum) {
    adapttree::Page p;
    p.init(7, PageType::LEAF);
    p.computeChecksum();
    p.rawBytes()[SLOT_DIR_OFFSET + 10] ^= 0xFF;
    EXPECT_FALSE(p.verifyChecksum());
}

TEST(Page, ChecksumExcludesHeader) {
    adapttree::Page p1, p2;
    p1.init(10, PageType::LEAF);
    p2.init(20, PageType::LEAF);
    p1.computeChecksum();
    p2.computeChecksum();
    EXPECT_EQ(p1.header().crc32, p2.header().crc32);
}

// ── PAGE-02 + PAGE-04: Slotted record layout ─────────────────────────────────

TEST(SlottedPage, InsertFirstRecordReturnsSlotZero) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    int32_t slot = adapttree::insertRecord(p, data, sizeof(data));
    EXPECT_EQ(slot, 0);
}

TEST(SlottedPage, InsertUpdatesSlotCount) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t d1[4] = {1, 2, 3, 4};
    uint8_t d2[4] = {5, 6, 7, 8};
    adapttree::insertRecord(p, d1, 4);
    adapttree::insertRecord(p, d2, 4);
    EXPECT_EQ(p.header().num_slots, 2u);
}

TEST(SlottedPage, InsertFirstRecord_FreePointersAdvanceCorrectly) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t data[12] = {};
    adapttree::insertRecord(p, data, 12);
    EXPECT_EQ(p.header().free_start, SLOT_DIR_OFFSET + 4u);
    EXPECT_EQ(p.header().free_end,   PAGE_SIZE - 12u);
}

TEST(SlottedPage, RecordAtReturnsCorrectBytes) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t written[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    int32_t slot = adapttree::insertRecord(p, written, 6);
    uint8_t* rec = nullptr;
    uint16_t len = 0;
    adapttree::recordAt(p, static_cast<uint16_t>(slot), rec, len);
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(len, 6u);
    EXPECT_EQ(std::memcmp(rec, written, 6), 0);
}

TEST(SlottedPage, MultipleRecordsStoredAndRetrievedInOrder) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t r0[4] = {0, 1, 2, 3};
    uint8_t r1[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    uint8_t r2[2] = {99, 100};
    int32_t s0 = adapttree::insertRecord(p, r0, 4);
    int32_t s1 = adapttree::insertRecord(p, r1, 8);
    int32_t s2 = adapttree::insertRecord(p, r2, 2);
    EXPECT_EQ(s0, 0); EXPECT_EQ(s1, 1); EXPECT_EQ(s2, 2);
    uint8_t* rec; uint16_t len;
    adapttree::recordAt(p, 0, rec, len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(std::memcmp(rec, r0, 4), 0);
    adapttree::recordAt(p, 1, rec, len);
    EXPECT_EQ(len, 8u);
    EXPECT_EQ(std::memcmp(rec, r1, 8), 0);
    adapttree::recordAt(p, 2, rec, len);
    EXPECT_EQ(len, 2u);
    EXPECT_EQ(std::memcmp(rec, r2, 2), 0);
}

TEST(SlottedPage, InsertReturnsMinus1WhenFull) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t data[20] = {};
    int32_t last_good = -1;
    for (int i = 0; i < 168; ++i) {
        data[0] = static_cast<uint8_t>(i);
        last_good = adapttree::insertRecord(p, data, 20);
        ASSERT_NE(last_good, -1) << "Unexpected full at i=" << i;
    }
    int32_t overflow = adapttree::insertRecord(p, data, 20);
    EXPECT_EQ(overflow, -1);
}

TEST(SlottedPage, NoFreeSpaceWhenSlotDirAndRecordAreaCollide) {
    adapttree::Page p;
    p.init(1, PageType::LEAF);
    uint8_t tiny[1] = {0xFF};
    p.header().free_start = static_cast<uint16_t>(PAGE_SIZE - sizeof(Slot));
    p.header().free_end   = static_cast<uint16_t>(PAGE_SIZE - sizeof(Slot));
    int32_t result = adapttree::insertRecord(p, tiny, 1);
    EXPECT_EQ(result, -1);
}
