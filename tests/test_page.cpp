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
