#pragma once

// NOTE: Do NOT include page.hpp here — it defines PAGE_SIZE as uint32_t and
// exports it to global scope, conflicting with buffer_pool.hpp's size_t version.
#include "adapttree/buffer_pool.hpp"  // PAGE_SIZE (size_t), INVALID_PAGE_ID, BufferPool<WAL>
#include "adapttree/wal.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace adapttree {

// ── B+ Tree constants ─────────────────────────────────────────────────────────
// ORDER is the maximum number of entries in a leaf node and the maximum number
// of keys in an internal node (which has ORDER+1 children).
// A node is full when it reaches ORDER entries; it is split after insertion.
inline constexpr uint32_t ORDER        = 200;
inline constexpr uint32_t SPLIT_HALF   = 100;    // entries kept in left node after split
inline constexpr uint32_t META_PAGE_ID = 0;      // page 0 is always the meta page

// INVALID_PAGE_ID is already defined in buffer_pool.hpp (= UINT32_MAX).
// Use it throughout — do not redefine.

// ── PageType ──────────────────────────────────────────────────────────────────
// Matches the encoding used in page.hpp (Phase 1) so the same byte layout is
// reused when pages are read back from disk.
enum class PageType : uint8_t {
    INVALID  = 0,
    INTERNAL = 1,
    LEAF     = 2,
    FREE     = 3,
    META     = 4,
};

// ── BPHeader ──────────────────────────────────────────────────────────────────
// 32-byte fixed header at byte offset 0 of every page managed by BPlusTree.
// Layout is byte-identical to Phase 1's PageHeader so pages written by either
// layer can be read back by the other.
//
//   [0..3]   page_id       uint32_t
//   [4..7]   crc32         uint32_t  (unused in Phase 3 — set to 0)
//   [8..9]   num_slots     uint16_t  (leaf: entry count; internal: key count)
//   [10..11] free_start    uint16_t  (unused in Phase 3 — set to 0)
//   [12..13] free_end      uint16_t  (unused in Phase 3 — set to 0)
//   [14]     page_type     PageType  (uint8_t)
//   [15]     flags         uint8_t
//   [16..19] next_leaf_id  uint32_t  (leaf chain; INVALID_PAGE_ID if tail)
//   [20..27] lsn           uint64_t  (WAL sequence number)
//   [28..31] _pad          uint8_t[4]

#pragma pack(push, 1)
struct BPHeader {
    uint32_t page_id;
    uint32_t crc32;
    uint16_t num_slots;
    uint16_t free_start;
    uint16_t free_end;
    PageType page_type;
    uint8_t  flags;
    uint32_t next_leaf_id;
    uint64_t lsn;
    uint8_t  _pad[4];
};
#pragma pack(pop)

static_assert(sizeof(BPHeader) == 32, "BPHeader must be exactly 32 bytes");
static_assert(offsetof(BPHeader, page_id)      ==  0);
static_assert(offsetof(BPHeader, crc32)        ==  4);
static_assert(offsetof(BPHeader, num_slots)    ==  8);
static_assert(offsetof(BPHeader, free_start)   == 10);
static_assert(offsetof(BPHeader, free_end)     == 12);
static_assert(offsetof(BPHeader, page_type)    == 14);
static_assert(offsetof(BPHeader, flags)        == 15);
static_assert(offsetof(BPHeader, next_leaf_id) == 16);
static_assert(offsetof(BPHeader, lsn)          == 20);

// ── Leaf entry ────────────────────────────────────────────────────────────────
// Each leaf stores up to ORDER fixed-size (key, value) pairs sorted by key.
// Direct array layout (not slotted) because entries are uniform size.
struct LeafEntry {
    uint64_t key;
    uint64_t value;
};
static_assert(sizeof(LeafEntry) == 16);

// ── LeafNode ──────────────────────────────────────────────────────────────────
// Occupies exactly PAGE_SIZE (4096) bytes.
// entries[0..num_slots-1] are valid and kept in ascending key order.
// Padding fills the remainder of the page.
struct LeafNode {
    BPHeader  header;                           // 32 bytes
    LeafEntry entries[ORDER];                   // 200 * 16 = 3200 bytes
    uint8_t   _padding[PAGE_SIZE
                        - sizeof(BPHeader)
                        - ORDER * sizeof(LeafEntry)];  // 864 bytes
};
static_assert(sizeof(LeafNode) == PAGE_SIZE,
              "LeafNode must be exactly PAGE_SIZE bytes");

// ── InternalNode ──────────────────────────────────────────────────────────────
// Occupies exactly PAGE_SIZE (4096) bytes.
// Stores ORDER keys and ORDER+1 child page IDs.
// keys[i] is the fence key separating children[i] and children[i+1]:
//   all keys in subtree children[i]   <  keys[i]
//   all keys in subtree children[i+1] >= keys[i]
// num_slots == number of keys currently stored (children count = num_slots + 1).
struct InternalNode {
    BPHeader header;                            // 32 bytes
    uint64_t keys[ORDER];                       // 200 * 8 = 1600 bytes
    uint32_t children[ORDER + 1];               // 201 * 4 = 804 bytes
    uint8_t  _padding[PAGE_SIZE
                       - sizeof(BPHeader)
                       - ORDER * sizeof(uint64_t)
                       - (ORDER + 1) * sizeof(uint32_t)];  // 1660 bytes
};
static_assert(sizeof(InternalNode) == PAGE_SIZE,
              "InternalNode must be exactly PAGE_SIZE bytes");

// ── MetaPage ──────────────────────────────────────────────────────────────────
// Page 0 (META_PAGE_ID).  Stores the root page ID and tree height.
// Updated on every root split.
struct MetaPage {
    BPHeader header;        // 32 bytes
    uint32_t root_page_id;  // 4 bytes
    uint32_t tree_height;   // 4 bytes — 1 means root is a leaf
    uint8_t  _padding[PAGE_SIZE - sizeof(BPHeader) - 8];  // 4056 bytes
};
static_assert(sizeof(MetaPage) == PAGE_SIZE,
              "MetaPage must be exactly PAGE_SIZE bytes");

// ── BPlusTree ─────────────────────────────────────────────────────────────────
// Templated on WAL_T so it can be used with NullWAL in tests or a real WAL
// in production without virtual dispatch overhead in the common path.
//
// Public interface:
//   insert(key, value) — inserts (key, value) pair; rejects duplicates (returns false)
//   get(key)           — returns std::optional<uint64_t>; nullopt if not found
//   height()           — returns current tree height (1 = root is leaf)
//
// Internally the tree stores all data in pages managed by the buffer pool.
// The tree does not perform any logging beyond calling wal_.append() to obtain
// an LSN for each newly dirtied page.

template <typename WAL_T>
class BPlusTree {
public:
    // Construct (or open) a B+ tree backed by `pool`.
    // On a fresh database the constructor allocates page 0 (meta) and page 1
    // (the initial empty root leaf).  On an existing database it trusts the
    // meta page already present in the pool.
    explicit BPlusTree(BufferPool<WAL_T>& pool, WAL_T& wal);

    // Insert (key, value).  Returns false if key already exists.
    // May trigger one or more page splits; amortised O(log n) buffer-pool round trips.
    bool insert(uint64_t key, uint64_t value);

    // Look up key.  Returns the associated value, or nullopt if not found.
    std::optional<uint64_t> get(uint64_t key);

    // Return the current height of the tree (1 means the root is a leaf node).
    uint32_t height();

private:
    BufferPool<WAL_T>& pool_;
    WAL_T&             wal_;

    // ── Meta helpers ──────────────────────────────────────────────────────────
    uint32_t root_page_id();
    void     set_root_page_id(uint32_t new_root_id);
    void     set_height(uint32_t h);

    // ── Traversal helpers ─────────────────────────────────────────────────────
    // findLeaf: descend the tree from root to the leaf that should contain key.
    // Fills `path` with the internal-node page IDs visited along the way
    // (root first, leaf's parent last).  Returns the leaf's page ID.
    uint32_t findLeaf(uint64_t key, std::vector<uint32_t>& path);

    // findSlotInLeaf: binary search for key in a sorted leaf.
    // Returns the index of key if found (>= 0), or -1 if not found.
    // If insert_pos != nullptr, *insert_pos is set to the position where
    // key should be inserted to maintain sorted order.
    int findSlotInLeaf(const LeafNode* leaf,
                       uint64_t        target,
                       int*            insert_pos = nullptr) const;

    // findChildIndex: return the child slot index for search_key in an internal node.
    // Returns i such that children[i] is the correct subtree to descend into.
    int findChildIndex(const InternalNode* node, uint64_t search_key) const;

    // ── Split helpers ─────────────────────────────────────────────────────────
    // insert_into_parent: after a split, insert (fence_key, right_child_id) into
    // the parent of left_child_id.  Creates a new root if path is empty.
    bool insert_into_parent(const std::vector<uint32_t>& path,
                            uint32_t left_child_id,
                            uint64_t fence_key,
                            uint32_t right_child_id);
};

}  // namespace adapttree
