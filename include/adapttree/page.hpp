#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace adapttree {

// ── Constants ─────────────────────────────────────────────────────────────────

inline constexpr uint32_t PAGE_SIZE        = 4096;
inline constexpr uint32_t HEADER_SIZE      = 32;    // sizeof(PageHeader) with padding
inline constexpr uint32_t MODEL_AREA_SIZE  = 32;    // reserved bytes after header for learned model
inline constexpr uint32_t SLOT_DIR_OFFSET  = HEADER_SIZE + MODEL_AREA_SIZE;  // 64
inline constexpr uint32_t PAGE_USABLE      = PAGE_SIZE - SLOT_DIR_OFFSET;    // 4032

// ── PageType ──────────────────────────────────────────────────────────────────

enum class PageType : uint8_t {
    INVALID  = 0,
    INTERNAL = 1,
    LEAF     = 2,
    FREE     = 3,
};

// ── PageHeader ────────────────────────────────────────────────────────────────
// Fixed 32-byte header at offset 0 of every page.
// Field layout (no implicit padding — verified by static_assert):
//   [0..3]   page_id       uint32_t
//   [4..7]   crc32         uint32_t  — CRC32 over bytes [HEADER_SIZE, PAGE_SIZE)
//   [8..9]   num_slots     uint16_t
//   [10..11] free_start    uint16_t  — byte offset of next free slot entry (grows up)
//   [12..13] free_end      uint16_t  — byte offset of next free record byte (grows down)
//   [14]     page_type     uint8_t   (PageType)
//   [15]     flags         uint8_t
//   [16..19] next_leaf_id  uint32_t  — for leaf chain; 0 if none
//   [20..27] lsn           uint64_t  — Log Sequence Number (set by WAL layer)
//   [28..31] _pad          uint8_t[4]
//
// Bytes [32..63] are the reserved_model area (not in this struct).
// Slot directory starts at byte 64 (SLOT_DIR_OFFSET).

#pragma pack(push, 1)
struct PageHeader {
    uint32_t page_id;
    uint32_t crc32;
    uint16_t num_slots;
    uint16_t free_start;   // offset of first free slot entry byte (rel to page start)
    uint16_t free_end;     // offset just past last inserted record byte (rel to page start)
    uint8_t  page_type;    // cast to PageType
    uint8_t  flags;
    uint32_t next_leaf_id;
    uint64_t lsn;
    uint8_t  _pad[4];
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 32, "PageHeader must be exactly 32 bytes");
static_assert(offsetof(PageHeader, page_id)      ==  0);
static_assert(offsetof(PageHeader, crc32)        ==  4);
static_assert(offsetof(PageHeader, num_slots)    ==  8);
static_assert(offsetof(PageHeader, free_start)   == 10);
static_assert(offsetof(PageHeader, free_end)     == 12);
static_assert(offsetof(PageHeader, page_type)    == 14);
static_assert(offsetof(PageHeader, flags)        == 15);
static_assert(offsetof(PageHeader, next_leaf_id) == 16);
static_assert(offsetof(PageHeader, lsn)          == 20);

// ── Slot ──────────────────────────────────────────────────────────────────────
// Each slot entry is 4 bytes: offset(2) + length(2).
// offset: byte offset of the record within the page (from page start).
// length: byte length of the record. 0 means the slot is deleted/empty.

#pragma pack(push, 1)
struct Slot {
    uint16_t offset;   // byte offset of record from page start
    uint16_t length;   // byte length of record; 0 = deleted
};
#pragma pack(pop)

static_assert(sizeof(Slot) == 4, "Slot must be 4 bytes");

// ── CRC32 ─────────────────────────────────────────────────────────────────────
// Software CRC32 (IEEE 802.3 polynomial 0xEDB88320 — reversed).
// No zlib dependency. Called from Page for page body checksumming.

inline uint32_t crc32_compute(const void* data, size_t len) {
    uint32_t crc = 0xFFFF'FFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB8'8320u & -(crc & 1u));
        }
    }
    return crc ^ 0xFFFF'FFFFu;
}

// ── Page ──────────────────────────────────────────────────────────────────────
// Owns a 4096-byte aligned buffer. Provides typed header access and checksum ops.
// The model area bytes [32..63] are accessible via modelArea() — Phase 8 writes here.
// The slot directory starts at byte 64 (SLOT_DIR_OFFSET).

class Page {
public:
    Page() { std::memset(data_, 0, PAGE_SIZE); }

    // Initialize header fields; set free_start/free_end for an empty page.
    void init(uint32_t id, PageType type) {
        std::memset(data_, 0, PAGE_SIZE);
        header().page_id    = id;
        header().page_type  = static_cast<uint8_t>(type);
        header().num_slots  = 0;
        header().free_start = static_cast<uint16_t>(SLOT_DIR_OFFSET);
        header().free_end   = static_cast<uint16_t>(PAGE_SIZE);
        header().lsn        = 0;
        header().next_leaf_id = 0;
        header().flags      = 0;
    }

    // CRC32 computed over bytes [HEADER_SIZE, PAGE_SIZE) — body only, not the header itself.
    void computeChecksum() {
        header().crc32 = crc32_compute(data_ + HEADER_SIZE, PAGE_SIZE - HEADER_SIZE);
    }

    bool verifyChecksum() const {
        uint32_t expected = crc32_compute(data_ + HEADER_SIZE, PAGE_SIZE - HEADER_SIZE);
        return header().crc32 == expected;
    }

    // Mutable header reference — callers update fields directly.
    PageHeader& header() {
        return *reinterpret_cast<PageHeader*>(data_);
    }
    const PageHeader& header() const {
        return *reinterpret_cast<const PageHeader*>(data_);
    }

    // Model area: bytes [32..63]. Phase 8 (PGM/Learned Index) writes here.
    uint8_t* modelArea() { return data_ + HEADER_SIZE; }
    const uint8_t* modelArea() const { return data_ + HEADER_SIZE; }

    // Raw page buffer — used by DiskManager for pread/pwrite.
    uint8_t* rawBytes() { return data_; }
    const uint8_t* rawBytes() const { return data_; }

    static constexpr uint32_t SIZE = PAGE_SIZE;

private:
    alignas(4096) uint8_t data_[PAGE_SIZE];
};

// ── Slotted record API ────────────────────────────────────────────────────────
// insertRecord: appends a record to a page using slotted layout.
//   - Record bytes are written at [free_end - length, free_end).
//   - A Slot entry {offset, length} is written at free_start.
//   - Returns the slot index (0-based) on success, or -1 if the page is full.
//
// Full condition: free_start + sizeof(Slot) + length > free_end

inline int32_t insertRecord(Page& page, const void* data, uint16_t length) {
    PageHeader& hdr = page.header();

    uint32_t needed = sizeof(Slot) + length;
    if (static_cast<uint32_t>(hdr.free_end) - static_cast<uint32_t>(hdr.free_start) < needed) {
        return -1;  // PAGE-04: full
    }

    // Write record downward from free_end.
    uint16_t record_offset = static_cast<uint16_t>(hdr.free_end - length);
    std::memcpy(page.rawBytes() + record_offset, data, length);

    // Write slot entry at free_start.
    Slot s{record_offset, length};
    std::memcpy(page.rawBytes() + hdr.free_start, &s, sizeof(Slot));

    // Advance pointers.
    uint16_t slot_index    = hdr.num_slots;
    hdr.num_slots          = static_cast<uint16_t>(hdr.num_slots + 1);
    hdr.free_start         = static_cast<uint16_t>(hdr.free_start + sizeof(Slot));
    hdr.free_end           = record_offset;

    return static_cast<int32_t>(slot_index);
}

// recordAt: retrieves a pointer and length for the record at slot_index.
//   out_ptr  — set to the start of the record bytes within the page buffer.
//   out_len  — set to the byte length of the record.

inline void recordAt(Page& page, uint16_t slot_index, uint8_t*& out_ptr, uint16_t& out_len) {
    const uint8_t* base = page.rawBytes() + SLOT_DIR_OFFSET;
    Slot s;
    std::memcpy(&s, base + slot_index * sizeof(Slot), sizeof(Slot));
    out_ptr = page.rawBytes() + s.offset;
    out_len = s.length;
}

} // namespace adapttree

// Pull into global scope for test convenience.
using adapttree::PAGE_SIZE;
using adapttree::HEADER_SIZE;
using adapttree::MODEL_AREA_SIZE;
using adapttree::SLOT_DIR_OFFSET;
using adapttree::PAGE_USABLE;
using adapttree::PageType;
using adapttree::PageHeader;
using adapttree::Slot;
