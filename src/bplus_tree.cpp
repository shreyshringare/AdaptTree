#include "adapttree/bplus_tree.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>   // std::abs
#include <cstring>

namespace adapttree {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T, typename MVCC_T>
BPlusTree<WAL_T, MVCC_T>::BPlusTree(BufferPool<WAL_T>& pool, WAL_T& wal)
    : pool_(pool), wal_(wal), trivial_mvcc_{}, mvcc_(trivial_mvcc_)
{
    auto meta_opt = pool_.fetchPage(META_PAGE_ID);
    if (!meta_opt) {
        // Fresh database — allocate page 0 (meta) then page 1 (initial root leaf).
        {
            auto meta_g = pool_.newPage();   // allocates page 0
            assert(meta_g.has_value());
            assert(meta_g->page_id() == META_PAGE_ID);
            auto* meta             = reinterpret_cast<MetaPage*>(meta_g->data());
            meta->header.page_id      = META_PAGE_ID;
            meta->header.page_type    = PageType::META;
            meta->header.num_slots    = 0;
            meta->header.free_start   = 0;
            meta->header.free_end     = 0;
            meta->header.flags        = 0;
            meta->header.next_leaf_id = INVALID_PAGE_ID;
            { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = META_PAGE_ID; _r.redo_len = 0; _r.undo_len = 0; meta->header.lsn = wal_.append(_r); }
            meta->tree_height         = 1;
            meta->root_page_id        = 1;
            meta_g->markDirty();
        }
        {
            auto root_g = pool_.newPage();   // allocates page 1
            assert(root_g.has_value());
            assert(root_g->page_id() == uint32_t{1});
            auto* root             = reinterpret_cast<LeafNode*>(root_g->data());
            root->header.page_id      = root_g->page_id();
            root->header.page_type    = PageType::LEAF;
            root->header.num_slots    = 0;
            root->header.free_start   = 0;
            root->header.free_end     = 0;
            root->header.flags        = 0;
            root->header.next_leaf_id = INVALID_PAGE_ID;
            { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = root_g->page_id(); _r.redo_len = 0; _r.undo_len = 0; root->header.lsn = wal_.append(_r); }
            root_g->markDirty();
        }
    }
    // else: existing database — meta page already correct in pool
}

template <typename WAL_T, typename MVCC_T>
BPlusTree<WAL_T, MVCC_T>::BPlusTree(BufferPool<WAL_T>& pool, WAL_T& wal, MVCC_T& mvcc)
    : pool_(pool), wal_(wal), trivial_mvcc_{}, mvcc_(mvcc)
{
    auto meta_opt = pool_.fetchPage(META_PAGE_ID);
    if (!meta_opt) {
        {
            auto meta_g = pool_.newPage();
            assert(meta_g.has_value());
            assert(meta_g->page_id() == META_PAGE_ID);
            auto* meta             = reinterpret_cast<MetaPage*>(meta_g->data());
            meta->header.page_id      = META_PAGE_ID;
            meta->header.page_type    = PageType::META;
            meta->header.num_slots    = 0;
            meta->header.free_start   = 0;
            meta->header.free_end     = 0;
            meta->header.flags        = 0;
            meta->header.next_leaf_id = INVALID_PAGE_ID;
            { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = META_PAGE_ID; _r.redo_len = 0; _r.undo_len = 0; meta->header.lsn = wal_.append(_r); }
            meta->tree_height         = 1;
            meta->root_page_id        = 1;
            meta_g->markDirty();
        }
        {
            auto root_g = pool_.newPage();
            assert(root_g.has_value());
            assert(root_g->page_id() == uint32_t{1});
            auto* root             = reinterpret_cast<LeafNode*>(root_g->data());
            root->header.page_id      = root_g->page_id();
            root->header.page_type    = PageType::LEAF;
            root->header.num_slots    = 0;
            root->header.free_start   = 0;
            root->header.free_end     = 0;
            root->header.flags        = 0;
            root->header.next_leaf_id = INVALID_PAGE_ID;
            { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = root_g->page_id(); _r.redo_len = 0; _r.undo_len = 0; root->header.lsn = wal_.append(_r); }
            root_g->markDirty();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Learned index helpers (Phase 9)
// ─────────────────────────────────────────────────────────────────────────────

// fullBinarySearchOpt: standard binary search returning optional slot index.
template <typename WAL_T, typename MVCC_T>
std::optional<uint32_t>
BPlusTree<WAL_T, MVCC_T>::fullBinarySearchOpt(const LeafNode* leaf, uint64_t key) const
{
    int n = static_cast<int>(leaf->header.num_slots);
    int lo = 0, hi = n;
    uint64_t iters = 0;
    while (lo < hi) {
        ++iters;
        int mid = lo + (hi - lo) / 2;
        if      (leaf->entries[mid].key < key) lo = mid + 1;
        else if (leaf->entries[mid].key > key) hi = mid;
        else { cmp_count_.fetch_add(iters, std::memory_order_relaxed); return static_cast<uint32_t>(mid); }
    }
    cmp_count_.fetch_add(iters, std::memory_order_relaxed);
    return std::nullopt;
}

// boundedBinarySearch: binary search restricted to slot range [lo, hi] inclusive.
template <typename WAL_T, typename MVCC_T>
std::optional<uint32_t>
BPlusTree<WAL_T, MVCC_T>::boundedBinarySearch(const LeafNode* leaf, uint64_t key,
                                       uint32_t lo, uint32_t hi) const
{
    if (lo > hi) return std::nullopt;
    int ilo = static_cast<int>(lo);
    int ihi = static_cast<int>(hi) + 1;  // half-open upper bound
    uint64_t iters = 0;
    while (ilo < ihi) {
        ++iters;
        int mid = ilo + (ihi - ilo) / 2;
        if      (leaf->entries[mid].key < key) ilo = mid + 1;
        else if (leaf->entries[mid].key > key) ihi = mid;
        else { cmp_count_.fetch_add(iters, std::memory_order_relaxed); return static_cast<uint32_t>(mid); }
    }
    cmp_count_.fetch_add(iters, std::memory_order_relaxed);
    return std::nullopt;
}

// findSlotLearned: dispatch between model-guided bounded search and full search.
template <typename WAL_T, typename MVCC_T>
std::optional<uint32_t>
BPlusTree<WAL_T, MVCC_T>::findSlotLearned(const LeafNode* leaf, uint64_t key) const
{
    // Gate 1: learned index disabled or no model fitted yet
    if (!use_learned_index_ || !leaf->model.has_model) {
        return fullBinarySearchOpt(leaf, key);
    }

    // Gate 2: stale model — too many inserts since last rebuild
    if (leaf->model.inserts_since_model_rebuild > kModelRebuildThreshold) {
        fallback_count_.fetch_add(1, std::memory_order_relaxed);
        return fullBinarySearchOpt(leaf, key);
    }

    // Use model: predict slot, clamp window to valid range
    const LearnedSegment& seg = leaf->model.learnedSegment();
    uint32_t ns   = static_cast<uint32_t>(leaf->header.num_slots);
    uint32_t pred = seg.predict(key, ns);

    static constexpr uint32_t kEpsilon = 4;
    uint32_t lo = (pred >= kEpsilon) ? (pred - kEpsilon) : 0u;
    uint32_t hi = std::min(pred + kEpsilon, ns > 0 ? ns - 1 : 0u);

    auto result = boundedBinarySearch(leaf, key, lo, hi);
    if (!result) {
        // Window miss — fall back to full search
        fallback_count_.fetch_add(1, std::memory_order_relaxed);
        return fullBinarySearchOpt(leaf, key);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Meta-page helpers (each fetches the meta page, modifies, and releases)
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T, typename MVCC_T>
uint32_t BPlusTree<WAL_T, MVCC_T>::root_page_id()
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    return reinterpret_cast<const MetaPage*>(g->data())->root_page_id;
}

template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::set_root_page_id(uint32_t new_root_id)
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    auto* meta           = reinterpret_cast<MetaPage*>(g->data());
    meta->root_page_id   = new_root_id;
    { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = META_PAGE_ID; _r.redo_len = 0; _r.undo_len = 0; meta->header.lsn = wal_.append(_r); }
    g->markDirty();
}

template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::set_height(uint32_t h)
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    auto* meta         = reinterpret_cast<MetaPage*>(g->data());
    meta->tree_height  = h;
    { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = META_PAGE_ID; _r.redo_len = 0; _r.undo_len = 0; meta->header.lsn = wal_.append(_r); }
    g->markDirty();
}

template <typename WAL_T, typename MVCC_T>
uint32_t BPlusTree<WAL_T, MVCC_T>::height()
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    return reinterpret_cast<const MetaPage*>(g->data())->tree_height;
}

// ─────────────────────────────────────────────────────────────────────────────
// Traversal helpers
// ─────────────────────────────────────────────────────────────────────────────

// findChildIndex: linear scan to find child index for search_key.
// Returns i such that children[i] is the correct subtree.
// Invariant: children[i] holds keys k where keys[i-1] <= k < keys[i]
// (keys[-1] = -inf, keys[ns] = +inf).
template <typename WAL_T, typename MVCC_T>
int BPlusTree<WAL_T, MVCC_T>::findChildIndex(const InternalNode* node,
                                      uint64_t search_key) const
{
    int ns = static_cast<int>(node->header.num_slots);
    // Binary search for first key strictly greater than search_key.
    int lo = 0, hi = ns;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (node->keys[mid] <= search_key) lo = mid + 1;
        else                               hi = mid;
    }
    return lo;
}

// findSlotInLeaf: binary search in a sorted leaf.
// Returns index of target if found, else -1.
// If insert_pos != nullptr, sets *insert_pos to the sorted insertion position.
template <typename WAL_T, typename MVCC_T>
int BPlusTree<WAL_T, MVCC_T>::findSlotInLeaf(const LeafNode* leaf,
                                      uint64_t        target,
                                      int*            insert_pos) const
{
    int n  = static_cast<int>(leaf->header.num_slots);
    int lo = 0, hi = n;
    uint64_t iters = 0;
    while (lo < hi) {
        ++iters;
        int mid = lo + (hi - lo) / 2;
        if (leaf->entries[mid].key < target)       lo = mid + 1;
        else if (leaf->entries[mid].key > target)  hi = mid;
        else {
            cmp_count_.fetch_add(iters, std::memory_order_relaxed);
            if (insert_pos) *insert_pos = mid;
            return mid;
        }
    }
    cmp_count_.fetch_add(iters, std::memory_order_relaxed);
    if (insert_pos) *insert_pos = lo;
    return -1;
}

// findLeaf: descend from root to leaf.
// Appends internal-node IDs to path.  Returns leaf page ID.
template <typename WAL_T, typename MVCC_T>
uint32_t BPlusTree<WAL_T, MVCC_T>::findLeaf(uint64_t key, std::vector<uint32_t>& path)
{
    uint32_t cur = root_page_id();

    while (true) {
        auto g = pool_.fetchPage(cur);
        assert(g.has_value());
        const auto* hdr = reinterpret_cast<const BPHeader*>(g->data());

        if (hdr->page_type == PageType::LEAF) {
            return cur;
        }
        const auto* node = reinterpret_cast<const InternalNode*>(g->data());
        path.push_back(cur);
        int ci = findChildIndex(node, key);
        cur    = node->children[ci];
        // g destructs here, unpinning `cur` (previous parent page)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// get()
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T, typename MVCC_T>
std::optional<uint64_t> BPlusTree<WAL_T, MVCC_T>::get(uint64_t key)
{
    std::vector<uint32_t> path;
    uint32_t leaf_id = findLeaf(key, path);

    auto g = pool_.fetchPage(leaf_id);
    if (!g) return std::nullopt;
    const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());

    // Use learned index path if enabled; otherwise fall back to traditional binary search.
    uint64_t raw_value;
    if (use_learned_index_) {
        auto opt_idx = findSlotLearned(leaf, key);
        if (!opt_idx) return std::nullopt;
        raw_value = leaf->entries[*opt_idx].value;
    } else {
        int idx = findSlotInLeaf(leaf, key);
        if (idx < 0) return std::nullopt;
        raw_value = leaf->entries[idx].value;
    }
    // MVCC visibility check: pass commit_ts=0 (per-slot commit_ts not yet stored in LeafEntry).
    // For TrivialMvcc this is a no-op passthrough; for real MVCC it enforces snapshot isolation.
    auto txn     = mvcc_.begin();
    auto visible = mvcc_.read_version(txn, /*page_id*/leaf_id, /*slot_idx*/0u,
                                      raw_value, /*current_commit_ts*/0u);
    mvcc_.commit(txn);
    return visible;  // nullopt if not visible to this txn's snapshot
}

// ─────────────────────────────────────────────────────────────────────────────
// insert_into_parent()
// ─────────────────────────────────────────────────────────────────────────────
// Insert fence_key and right_child_id into the parent of left_child_id.
// path: list of internal node IDs from root down to (but not including) the
//       node whose parent we are modifying.  The last element of path is the
//       direct parent.
// If path is empty, left_child_id is the current root → create a new root.

template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::insert_into_parent(const std::vector<uint32_t>& path,
                                           uint32_t left_child_id,
                                           uint64_t fence_key,
                                           uint32_t right_child_id)
{
    if (path.empty()) {
        // left_child_id was the root — create a new internal root.
        auto new_root_g = pool_.newPage();
        if (!new_root_g) return false;

        uint32_t new_root_id = new_root_g->page_id();
        auto* nr             = reinterpret_cast<InternalNode*>(new_root_g->data());

        nr->header.page_id      = new_root_id;
        nr->header.page_type    = PageType::INTERNAL;
        nr->header.num_slots    = 1;
        nr->header.flags        = 0;
        nr->header.free_start   = 0;
        nr->header.free_end     = 0;
        nr->header.next_leaf_id = INVALID_PAGE_ID;
        { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = new_root_id; _r.redo_len = 0; _r.undo_len = 0; nr->header.lsn = wal_.append(_r); }
        nr->keys[0]             = fence_key;
        nr->children[0]         = left_child_id;
        nr->children[1]         = right_child_id;
        new_root_g->markDirty();

        // Update meta
        uint32_t old_h = height();
        set_root_page_id(new_root_id);
        set_height(old_h + 1);
        return true;
    }

    // Insert into the direct parent (last element of path).
    uint32_t parent_id = path.back();

    // We need to find the insertion position in the parent and insert fence_key
    // plus right_child_id.  If the parent overflows, split it.
    bool parent_overflows = false;
    uint64_t push_up_key  = 0;
    uint32_t right_int_id = INVALID_PAGE_ID;

    {
        auto parent_g = pool_.fetchPage(parent_id);
        if (!parent_g) return false;
        auto* parent = reinterpret_cast<InternalNode*>(parent_g->data());

        int ns = static_cast<int>(parent->header.num_slots);
        int pos = findChildIndex(parent, fence_key);  // where right_child should go

        if (ns < static_cast<int>(ORDER)) {
            // Parent has room — shift and insert
            if (pos < ns) {
                std::memmove(&parent->keys[pos + 1], &parent->keys[pos],
                             static_cast<size_t>(ns - pos) * sizeof(uint64_t));
                std::memmove(&parent->children[pos + 2], &parent->children[pos + 1],
                             static_cast<size_t>(ns - pos) * sizeof(uint32_t));
            }
            parent->keys[pos]        = fence_key;
            parent->children[pos + 1] = right_child_id;
            parent->header.num_slots  = static_cast<uint16_t>(ns + 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; parent->header.lsn = wal_.append(_r); }
            parent_g->markDirty();
            return true;
        }

        // Parent is full (ORDER keys) — temporarily store ORDER+1 keys and
        // ORDER+2 children, then split.
        uint64_t all_keys[ORDER + 1];
        uint32_t all_children[ORDER + 2];

        std::memcpy(all_keys,     parent->keys,     ORDER * sizeof(uint64_t));
        std::memcpy(all_children, parent->children, (ORDER + 1) * sizeof(uint32_t));

        // Insert at pos
        if (pos < ns) {
            std::memmove(&all_keys[pos + 1], &all_keys[pos],
                         static_cast<size_t>(ns - pos) * sizeof(uint64_t));
            std::memmove(&all_children[pos + 2], &all_children[pos + 1],
                         static_cast<size_t>(ns - pos) * sizeof(uint32_t));
        }
        all_keys[pos]        = fence_key;
        all_children[pos + 1] = right_child_id;
        // Now we have ORDER+1 keys and ORDER+2 children.

        // Middle key to push up
        push_up_key         = all_keys[SPLIT_HALF];
        parent_overflows    = true;

        // Write left half into parent: keys[0..SPLIT_HALF-1], children[0..SPLIT_HALF]
        std::memcpy(parent->keys,     all_keys,     SPLIT_HALF * sizeof(uint64_t));
        std::memcpy(parent->children, all_children, (SPLIT_HALF + 1) * sizeof(uint32_t));
        parent->header.num_slots = static_cast<uint16_t>(SPLIT_HALF);
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; parent->header.lsn = wal_.append(_r); }
        parent_g->markDirty();

        // Allocate right internal node before parent_g destructs
        // (but parent_g destructs at end of this scope before newPage).
        // We store the right half in a local buffer and create the right node
        // after this scope ends.

        // Store right half for use after scope
        // Right gets keys[SPLIT_HALF+1..ORDER] and children[SPLIT_HALF+1..ORDER+1]
        uint32_t right_key_count = static_cast<uint32_t>(ORDER - SPLIT_HALF);

        // parent_g destructs here, unpinning parent
        // We need to create the right node AFTER this scope ends.
        // Save right half data into a temporary approach:
        // We'll allocate the right node inside this scope just before parent_g destructs.
        // Actually, parent_g destructs at the END of this scope — it's still alive here.
        // Let's allocate the right node now (inside this scope, parent_g still alive).

        auto right_int_g = pool_.newPage();
        if (!right_int_g) return false;
        right_int_id = right_int_g->page_id();

        auto* rn             = reinterpret_cast<InternalNode*>(right_int_g->data());
        rn->header.page_id      = right_int_id;
        rn->header.page_type    = PageType::INTERNAL;
        rn->header.num_slots    = static_cast<uint16_t>(right_key_count);
        rn->header.flags        = 0;
        rn->header.free_start   = 0;
        rn->header.free_end     = 0;
        rn->header.next_leaf_id = INVALID_PAGE_ID;
        { WalRecord _r; _r.record_type = WalRecordType::INSERT; _r.page_id = right_int_id; _r.redo_len = 0; _r.undo_len = 0; rn->header.lsn = wal_.append(_r); }

        std::memcpy(rn->keys,
                    &all_keys[SPLIT_HALF + 1],
                    right_key_count * sizeof(uint64_t));
        std::memcpy(rn->children,
                    &all_children[SPLIT_HALF + 1],
                    (right_key_count + 1) * sizeof(uint32_t));
        right_int_g->markDirty();

        // right_int_g and parent_g both destruct here (unpin)
    }

    if (parent_overflows) {
        // Recurse up
        std::vector<uint32_t> grandparent_path(path.begin(), path.end() - 1);
        return insert_into_parent(grandparent_path, parent_id, push_up_key, right_int_id);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// insert()
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::insert(uint64_t key, uint64_t value)
{
    std::vector<uint32_t> path;
    uint32_t leaf_id = findLeaf(key, path);

    {
        auto leaf_g = pool_.fetchPage(leaf_id);
        assert(leaf_g.has_value());
        auto* leaf = reinterpret_cast<LeafNode*>(leaf_g->data());

        int insert_pos = 0;
        int found = findSlotInLeaf(leaf, key, &insert_pos);
        if (found >= 0) {
            // Future UPDATE path: mvcc_.archive_version(leaf_id, found, leaf->entries[found].value, 0);
            return false;   // duplicate key
        }

        int n = static_cast<int>(leaf->header.num_slots);

        if (n < static_cast<int>(ORDER)) {
            // Leaf has space — WAL-before-data (WAL-02): log before touching page bytes
            WalRecord wal_rec;
            wal_rec.txn_id      = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
            wal_rec.page_id     = leaf_id;
            wal_rec.record_type = WalRecordType::INSERT;
            wal_rec.redo_data.resize(16);
            std::memcpy(wal_rec.redo_data.data(),     &key,   8);
            std::memcpy(wal_rec.redo_data.data() + 8, &value, 8);
            wal_rec.redo_len = 16;
            wal_rec.undo_len = 0;
            uint64_t lsn = wal_.append(wal_rec);
            // Now write actual slot data:
            if (insert_pos < n) {
                std::memmove(&leaf->entries[insert_pos + 1],
                             &leaf->entries[insert_pos],
                             static_cast<size_t>(n - insert_pos) * sizeof(LeafEntry));
            }
            leaf->entries[insert_pos] = {key, value};
            leaf->header.num_slots    = static_cast<uint16_t>(n + 1);
            leaf->header.lsn          = lsn;
            // LEARN-05: track staleness; saturate at UINT16_MAX
            if (leaf->model.has_model &&
                leaf->model.inserts_since_model_rebuild < UINT16_MAX) {
                ++leaf->model.inserts_since_model_rebuild;
            }
            leaf_g->markDirty();
            return true;
        }

        // Leaf is full — build ORDER+1 sorted entry array, store it in the leaf,
        // then split.  We temporarily use the leaf's entry array for ORDER entries
        // plus a local buffer for the extra entry.
        LeafEntry all_entries[ORDER + 1];
        std::memcpy(all_entries, leaf->entries, static_cast<size_t>(n) * sizeof(LeafEntry));
        if (insert_pos < n) {
            std::memmove(&all_entries[insert_pos + 1],
                         &all_entries[insert_pos],
                         static_cast<size_t>(n - insert_pos) * sizeof(LeafEntry));
        }
        all_entries[insert_pos] = {key, value};

        // Write all ORDER+1 entries back into the leaf (the array can hold ORDER+1
        // because we defined entries[ORDER] which is exactly ORDER slots — but we
        // ONLY write ORDER+1 items into a temporary local buffer, then copy SPLIT_HALF
        // back; the leaf array only ever holds ORDER entries).
        //
        // Actually: the leaf struct has entries[ORDER] (exactly ORDER slots).
        // We cannot write ORDER+1 items into it.
        // Instead: write SPLIT_HALF into the leaf now, then call split_leaf with
        // a different protocol where the right half comes from our local all_entries.

        // Write SPLIT_HALF entries into left leaf — WAL-before-data (WAL-02)
        { WalRecord _r; _r.txn_id = next_txn_id_.fetch_add(1, std::memory_order_relaxed); _r.record_type = WalRecordType::INSERT; _r.page_id = leaf_id; _r.redo_len = 0; _r.undo_len = 0; leaf->header.lsn = wal_.append(_r); }
        std::memcpy(leaf->entries, all_entries, SPLIT_HALF * sizeof(LeafEntry));
        leaf->header.num_slots    = static_cast<uint16_t>(SPLIT_HALF);
        // LEARN-02: fit a learned model for the left child immediately after split.
        // Model bytes are in place before markDirty() so the dirty page image carries them.
        {
            std::vector<KeyPos> left_kp;
            left_kp.reserve(SPLIT_HALF);
            for (uint32_t i = 0; i < SPLIT_HALF; ++i) {
                left_kp.push_back({all_entries[i].key, i});
            }
            auto left_segs = pgm_builder_.fit(left_kp);
            // Only store a model when fit() produces exactly one segment.
            // Multiple segments mean the key distribution is non-linear; we skip
            // model storage and leave has_model=0 so search falls back to binary search.
            if (left_segs.size() == 1) {
                leaf->model.setLearnedSegment(left_segs[0]);
                leaf->model.has_model                   = 1;
                leaf->model.inserts_since_model_rebuild = 0;
#ifndef NDEBUG
                // LEARN-02 / T-09-01: post-split validation — every training key must
                // be predicted within error_bound slots of its actual rank.
                const LearnedSegment& dbg_seg = leaf->model.learnedSegment();
                for (uint32_t i = 0; i < SPLIT_HALF; ++i) {
                    uint32_t predicted = dbg_seg.predict(all_entries[i].key, SPLIT_HALF);
                    int64_t  diff      = static_cast<int64_t>(predicted) - static_cast<int64_t>(i);
                    assert(diff >= -static_cast<int64_t>(dbg_seg.error_bound) &&
                           diff <=  static_cast<int64_t>(dbg_seg.error_bound));
                }
#endif
            }
        }
        leaf_g->markDirty();

        uint32_t old_next = leaf->header.next_leaf_id;

        // leaf_g destructs here — unpin left leaf BEFORE allocating right leaf
        // But we still have `all_entries` in scope!
        (void)old_next;  // will use leaf_g data after re-fetch — see below

        // We need old_next_leaf but leaf_g is about to destruct.
        // Capture it before destruct:
        // Actually leaf_g destructs at end of this scope.  Let's capture now:
        uint32_t captured_old_next = leaf->header.next_leaf_id;
        // Note: after leaf_g destructs, `leaf` pointer is stale.
        // We must not use `leaf` after this inner scope ends.
        // Since the scope is still open, `leaf` is valid here.

        // Unpin by destroying leaf_g early via move
        {
            // Create a local scope to destruct leaf_g before newPage
            // leaf_g will be moved into this scope and destructed
        }
        // leaf_g still alive (we can't easily destruct it mid-scope without reset())
        // SOLUTION: restructure so the rest is outside this scope.
        // We'll store what we need and let leaf_g destruct naturally at end of scope.

        // Allocate right leaf node (leaf_g still pinned — this is a 2nd pinned page)
        auto right_g = pool_.newPage();
        if (!right_g) return false;

        uint32_t right_id = right_g->page_id();
        auto* right       = reinterpret_cast<LeafNode*>(right_g->data());

        right->header.page_id      = right_id;
        right->header.page_type    = PageType::LEAF;
        right->header.num_slots    = static_cast<uint16_t>(ORDER + 1 - SPLIT_HALF);
        right->header.flags        = 0;
        right->header.free_start   = 0;
        right->header.free_end     = 0;
        right->header.next_leaf_id = captured_old_next;
        { WalRecord _r; _r.txn_id = next_txn_id_.fetch_add(1, std::memory_order_relaxed); _r.record_type = WalRecordType::INSERT; _r.page_id = right_id; _r.redo_len = 0; _r.undo_len = 0; right->header.lsn = wal_.append(_r); }

        std::memcpy(right->entries,
                    &all_entries[SPLIT_HALF],
                    (ORDER + 1 - SPLIT_HALF) * sizeof(LeafEntry));
        // LEARN-02: fit a learned model for the right child.
        {
            uint32_t right_count = ORDER + 1 - SPLIT_HALF;
            std::vector<KeyPos> right_kp;
            right_kp.reserve(right_count);
            for (uint32_t i = 0; i < right_count; ++i) {
                right_kp.push_back({all_entries[SPLIT_HALF + i].key, i});
            }
            auto right_segs = pgm_builder_.fit(right_kp);
            // Only store a model when fit() produces exactly one segment.
            if (right_segs.size() == 1) {
                right->model.setLearnedSegment(right_segs[0]);
                right->model.has_model                   = 1;
                right->model.inserts_since_model_rebuild = 0;
#ifndef NDEBUG
                // LEARN-02 / T-09-01: post-split validation for right child.
                const LearnedSegment& dbg_seg = right->model.learnedSegment();
                for (uint32_t i = 0; i < right_count; ++i) {
                    uint32_t predicted = dbg_seg.predict(all_entries[SPLIT_HALF + i].key, right_count);
                    int64_t  diff      = static_cast<int64_t>(predicted) - static_cast<int64_t>(i);
                    assert(diff >= -static_cast<int64_t>(dbg_seg.error_bound) &&
                           diff <=  static_cast<int64_t>(dbg_seg.error_bound));
                }
#endif
            }
        }
        right_g->markDirty();

        // Update left leaf's next_leaf_id → right_id
        // leaf_g still alive at this point so we can update through it
        leaf->header.next_leaf_id = right_id;
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = leaf_id; _r.redo_len = 0; _r.undo_len = 0; leaf->header.lsn = wal_.append(_r); }
        leaf_g->markDirty();

        uint64_t fence_key = all_entries[SPLIT_HALF].key;

        // right_g and leaf_g both destruct here
        // Insert fence_key into parent AFTER both guards are released
        // But we're still inside the scope — right_g and leaf_g are alive.
        // We need to call insert_into_parent after they're released.
        // Use nested scope trick:

        // Store what we need
        uint64_t fence = fence_key;
        uint32_t rid   = right_id;
        uint32_t lid   = leaf_id;

        // Explicitly release both guards by assigning nullopt
        // (no reset() available, but move-assignment to a local destroys the old)
        {
            // Destruct right_g and leaf_g by moving to temporaries that immediately destruct
            auto tmp1 = std::move(right_g);
            auto tmp2 = std::move(leaf_g);
            // tmp1 and tmp2 destruct here, unpinning
        }

        return insert_into_parent(path, lid, fence, rid);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 5 — Delete helpers
// ─────────────────────────────────────────────────────────────────────────────

// findLeafWithChildIdx: descend from root to leaf, recording (parent_id, child_idx) pairs.
template <typename WAL_T, typename MVCC_T>
uint32_t BPlusTree<WAL_T, MVCC_T>::findLeafWithChildIdx(
    uint64_t key,
    std::vector<std::pair<uint32_t, int>>& path)
{
    uint32_t cur = root_page_id();
    while (true) {
        auto g = pool_.fetchPage(cur);
        assert(g.has_value());
        const auto* hdr = reinterpret_cast<const BPHeader*>(g->data());
        if (hdr->page_type == PageType::LEAF) {
            return cur;
        }
        const auto* node = reinterpret_cast<const InternalNode*>(g->data());
        int ci = findChildIndex(node, key);
        path.push_back({cur, ci});
        cur = node->children[ci];
    }
}

// remove_from_leaf: compact-shift deletion in a leaf node.
template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::remove_from_leaf(uint32_t leaf_id, uint64_t key)
{
    auto g = pool_.fetchPage(leaf_id);
    assert(g.has_value());
    auto* leaf = reinterpret_cast<LeafNode*>(g->data());

    int idx = findSlotInLeaf(leaf, key);
    if (idx < 0) return false;

    int n = static_cast<int>(leaf->header.num_slots);
    if (idx < n - 1) {
        std::memmove(&leaf->entries[idx], &leaf->entries[idx + 1],
                     static_cast<size_t>(n - 1 - idx) * sizeof(LeafEntry));
    }
    leaf->header.num_slots = static_cast<uint16_t>(n - 1);
    { WalRecord _r; _r.record_type = WalRecordType::DELETE; _r.page_id = leaf_id; _r.redo_len = 0; _r.undo_len = 0; leaf->header.lsn = wal_.append(_r); }
    g->markDirty();
    return true;
}

// find_child_idx_in_parent: linear scan for child_id in parent's children array.
template <typename WAL_T, typename MVCC_T>
int BPlusTree<WAL_T, MVCC_T>::find_child_idx_in_parent(uint32_t parent_id, uint32_t child_id)
{
    auto g = pool_.fetchPage(parent_id);
    assert(g.has_value());
    const auto* node = reinterpret_cast<const InternalNode*>(g->data());
    int n = static_cast<int>(node->header.num_slots) + 1;
    for (int i = 0; i < n; ++i) {
        if (node->children[i] == child_id) return i;
    }
    return -1;
}

// redistribute_leaves: borrow one entry from a sibling leaf.
// child_idx is the index of the underfull leaf in parent.children[].
template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::redistribute_leaves(uint32_t parent_id, int child_idx)
{
    auto parent_g = pool_.fetchPage(parent_id);
    assert(parent_g.has_value());
    auto* parent = reinterpret_cast<InternalNode*>(parent_g->data());
    int parent_ns = static_cast<int>(parent->header.num_slots);

    uint32_t underfull_id = parent->children[child_idx];

    // Try left sibling first (child_idx > 0)
    if (child_idx > 0) {
        uint32_t left_sib_id = parent->children[child_idx - 1];
        auto left_g          = pool_.fetchPage(left_sib_id);
        assert(left_g.has_value());
        auto* left_sib = reinterpret_cast<LeafNode*>(left_g->data());

        if (left_sib->header.num_slots > static_cast<uint16_t>(SPLIT_HALF)) {
            // Borrow rightmost entry from left sibling
            auto uf_g = pool_.fetchPage(underfull_id);
            assert(uf_g.has_value());
            auto* uf = reinterpret_cast<LeafNode*>(uf_g->data());

            int uf_n  = static_cast<int>(uf->header.num_slots);
            int lf_n  = static_cast<int>(left_sib->header.num_slots);

            // Shift underfull's entries right by 1
            std::memmove(&uf->entries[1], &uf->entries[0],
                         static_cast<size_t>(uf_n) * sizeof(LeafEntry));
            uf->entries[0]         = left_sib->entries[lf_n - 1];
            uf->header.num_slots   = static_cast<uint16_t>(uf_n + 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = underfull_id; _r.redo_len = 0; _r.undo_len = 0; uf->header.lsn = wal_.append(_r); }
            uf_g->markDirty();

            left_sib->header.num_slots = static_cast<uint16_t>(lf_n - 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = left_sib_id; _r.redo_len = 0; _r.undo_len = 0; left_sib->header.lsn = wal_.append(_r); }
            left_g->markDirty();

            // Update parent separator: parent->keys[child_idx-1] = first key of underfull
            parent->keys[child_idx - 1] = uf->entries[0].key;
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; parent->header.lsn = wal_.append(_r); }
            parent_g->markDirty();
            return true;
        }
    }

    // Try right sibling (child_idx < parent_ns)
    if (child_idx < parent_ns) {
        uint32_t right_sib_id = parent->children[child_idx + 1];
        auto right_g          = pool_.fetchPage(right_sib_id);
        assert(right_g.has_value());
        auto* right_sib = reinterpret_cast<LeafNode*>(right_g->data());

        if (right_sib->header.num_slots > static_cast<uint16_t>(SPLIT_HALF)) {
            // Borrow leftmost entry from right sibling
            auto uf_g = pool_.fetchPage(underfull_id);
            assert(uf_g.has_value());
            auto* uf = reinterpret_cast<LeafNode*>(uf_g->data());

            int uf_n  = static_cast<int>(uf->header.num_slots);
            int rf_n  = static_cast<int>(right_sib->header.num_slots);

            uf->entries[uf_n]      = right_sib->entries[0];
            uf->header.num_slots   = static_cast<uint16_t>(uf_n + 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = underfull_id; _r.redo_len = 0; _r.undo_len = 0; uf->header.lsn = wal_.append(_r); }
            uf_g->markDirty();

            // Shift right sibling's entries left by 1
            std::memmove(&right_sib->entries[0], &right_sib->entries[1],
                         static_cast<size_t>(rf_n - 1) * sizeof(LeafEntry));
            right_sib->header.num_slots = static_cast<uint16_t>(rf_n - 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = right_sib_id; _r.redo_len = 0; _r.undo_len = 0; right_sib->header.lsn = wal_.append(_r); }
            right_g->markDirty();

            // Update parent separator: parent->keys[child_idx] = first key of right sibling
            parent->keys[child_idx] = right_sib->entries[0].key;
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; parent->header.lsn = wal_.append(_r); }
            parent_g->markDirty();
            return true;
        }
    }

    return false;
}

// merge_leaves: absorb right into left, remove separator from parent.
template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::merge_leaves(uint32_t parent_id, int child_idx)
{
    auto parent_g = pool_.fetchPage(parent_id);
    assert(parent_g.has_value());
    auto* parent   = reinterpret_cast<InternalNode*>(parent_g->data());
    int parent_ns  = static_cast<int>(parent->header.num_slots);

    // Always merge left+right where left = children[sep_idx], right = children[sep_idx+1]
    int sep_idx;
    uint32_t left_id, right_id;
    if (child_idx > 0) {
        sep_idx  = child_idx - 1;
        left_id  = parent->children[child_idx - 1];
        right_id = parent->children[child_idx];
    } else {
        sep_idx  = 0;
        left_id  = parent->children[0];
        right_id = parent->children[1];
    }

    {
        auto left_g  = pool_.fetchPage(left_id);
        auto right_g = pool_.fetchPage(right_id);
        assert(left_g.has_value() && right_g.has_value());
        auto* left  = reinterpret_cast<LeafNode*>(left_g->data());
        auto* right = reinterpret_cast<LeafNode*>(right_g->data());

        int ln = static_cast<int>(left->header.num_slots);
        int rn = static_cast<int>(right->header.num_slots);

        // Copy right's entries into left
        std::memcpy(&left->entries[ln], right->entries,
                    static_cast<size_t>(rn) * sizeof(LeafEntry));
        left->header.num_slots  = static_cast<uint16_t>(ln + rn);
        left->header.next_leaf_id = right->header.next_leaf_id;
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = left_id; _r.redo_len = 0; _r.undo_len = 0; left->header.lsn = wal_.append(_r); }
        left_g->markDirty();

        // Mark right as FREE
        right->header.page_type = PageType::FREE;
        right->header.num_slots = 0;
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = right_id; _r.redo_len = 0; _r.undo_len = 0; right->header.lsn = wal_.append(_r); }
        right_g->markDirty();
    }

    // Remove separator key (sep_idx) and right child pointer (sep_idx+1) from parent
    if (sep_idx < parent_ns - 1) {
        std::memmove(&parent->keys[sep_idx], &parent->keys[sep_idx + 1],
                     static_cast<size_t>(parent_ns - 1 - sep_idx) * sizeof(uint64_t));
        std::memmove(&parent->children[sep_idx + 1], &parent->children[sep_idx + 2],
                     static_cast<size_t>(parent_ns - 1 - sep_idx) * sizeof(uint32_t));
    }
    parent->header.num_slots = static_cast<uint16_t>(parent_ns - 1);
    { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; parent->header.lsn = wal_.append(_r); }
    parent_g->markDirty();
}

// redistribute_internal: rotate a key through the grandparent separator between two siblings.
template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::redistribute_internal(uint32_t grandparent_id, int child_idx)
{
    auto gp_g = pool_.fetchPage(grandparent_id);
    assert(gp_g.has_value());
    auto* gp   = reinterpret_cast<InternalNode*>(gp_g->data());
    int gp_ns  = static_cast<int>(gp->header.num_slots);

    uint32_t underfull_id = gp->children[child_idx];

    // Try left sibling
    if (child_idx > 0) {
        uint32_t left_sib_id = gp->children[child_idx - 1];
        auto left_g          = pool_.fetchPage(left_sib_id);
        assert(left_g.has_value());
        auto* left_sib = reinterpret_cast<InternalNode*>(left_g->data());

        if (left_sib->header.num_slots > static_cast<uint16_t>(SPLIT_HALF - 1)) {
            // Rotate: grandparent's separator[child_idx-1] goes down to underfull as first key,
            // left_sib's last key comes up to grandparent.
            auto uf_g = pool_.fetchPage(underfull_id);
            assert(uf_g.has_value());
            auto* uf    = reinterpret_cast<InternalNode*>(uf_g->data());
            int uf_ns   = static_cast<int>(uf->header.num_slots);
            int lf_ns   = static_cast<int>(left_sib->header.num_slots);

            // Shift underfull keys right, make room for separator at [0]
            std::memmove(&uf->keys[1], &uf->keys[0],
                         static_cast<size_t>(uf_ns) * sizeof(uint64_t));
            std::memmove(&uf->children[1], &uf->children[0],
                         static_cast<size_t>(uf_ns + 1) * sizeof(uint32_t));
            uf->keys[0]     = gp->keys[child_idx - 1];
            uf->children[0] = left_sib->children[lf_ns];
            uf->header.num_slots = static_cast<uint16_t>(uf_ns + 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = underfull_id; _r.redo_len = 0; _r.undo_len = 0; uf->header.lsn = wal_.append(_r); }
            uf_g->markDirty();

            // Promote left_sib's last key to grandparent
            gp->keys[child_idx - 1] = left_sib->keys[lf_ns - 1];
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = grandparent_id; _r.redo_len = 0; _r.undo_len = 0; gp->header.lsn = wal_.append(_r); }
            gp_g->markDirty();

            left_sib->header.num_slots = static_cast<uint16_t>(lf_ns - 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = left_sib_id; _r.redo_len = 0; _r.undo_len = 0; left_sib->header.lsn = wal_.append(_r); }
            left_g->markDirty();
            return true;
        }
    }

    // Try right sibling
    if (child_idx < gp_ns) {
        uint32_t right_sib_id = gp->children[child_idx + 1];
        auto right_g          = pool_.fetchPage(right_sib_id);
        assert(right_g.has_value());
        auto* right_sib = reinterpret_cast<InternalNode*>(right_g->data());

        if (right_sib->header.num_slots > static_cast<uint16_t>(SPLIT_HALF - 1)) {
            auto uf_g = pool_.fetchPage(underfull_id);
            assert(uf_g.has_value());
            auto* uf    = reinterpret_cast<InternalNode*>(uf_g->data());
            int uf_ns   = static_cast<int>(uf->header.num_slots);
            int rf_ns   = static_cast<int>(right_sib->header.num_slots);

            // Append grandparent separator to underfull, then right_sib's first child
            uf->keys[uf_ns]       = gp->keys[child_idx];
            uf->children[uf_ns + 1] = right_sib->children[0];
            uf->header.num_slots  = static_cast<uint16_t>(uf_ns + 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = underfull_id; _r.redo_len = 0; _r.undo_len = 0; uf->header.lsn = wal_.append(_r); }
            uf_g->markDirty();

            // Promote right_sib's first key to grandparent
            gp->keys[child_idx] = right_sib->keys[0];
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = grandparent_id; _r.redo_len = 0; _r.undo_len = 0; gp->header.lsn = wal_.append(_r); }
            gp_g->markDirty();

            // Shift right_sib left
            std::memmove(&right_sib->keys[0], &right_sib->keys[1],
                         static_cast<size_t>(rf_ns - 1) * sizeof(uint64_t));
            std::memmove(&right_sib->children[0], &right_sib->children[1],
                         static_cast<size_t>(rf_ns) * sizeof(uint32_t));
            right_sib->header.num_slots = static_cast<uint16_t>(rf_ns - 1);
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = right_sib_id; _r.redo_len = 0; _r.undo_len = 0; right_sib->header.lsn = wal_.append(_r); }
            right_g->markDirty();
            return true;
        }
    }

    return false;
}

// merge_internal: merge internal node at child_idx with adjacent sibling, pulling down separator.
template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::merge_internal(uint32_t grandparent_id, int child_idx)
{
    auto gp_g = pool_.fetchPage(grandparent_id);
    assert(gp_g.has_value());
    auto* gp   = reinterpret_cast<InternalNode*>(gp_g->data());
    int gp_ns  = static_cast<int>(gp->header.num_slots);

    int sep_idx;
    uint32_t left_id, right_id;
    if (child_idx > 0) {
        sep_idx  = child_idx - 1;
        left_id  = gp->children[child_idx - 1];
        right_id = gp->children[child_idx];
    } else {
        sep_idx  = 0;
        left_id  = gp->children[0];
        right_id = gp->children[1];
    }

    uint64_t sep_key = gp->keys[sep_idx];

    {
        auto left_g  = pool_.fetchPage(left_id);
        auto right_g = pool_.fetchPage(right_id);
        assert(left_g.has_value() && right_g.has_value());
        auto* left  = reinterpret_cast<InternalNode*>(left_g->data());
        auto* right = reinterpret_cast<InternalNode*>(right_g->data());

        int ln = static_cast<int>(left->header.num_slots);
        int rn = static_cast<int>(right->header.num_slots);

        // Pull separator key down into left
        left->keys[ln] = sep_key;
        // Copy right's keys and children into left
        std::memcpy(&left->keys[ln + 1], right->keys,
                    static_cast<size_t>(rn) * sizeof(uint64_t));
        std::memcpy(&left->children[ln + 1], right->children,
                    static_cast<size_t>(rn + 1) * sizeof(uint32_t));
        left->header.num_slots = static_cast<uint16_t>(ln + 1 + rn);
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = left_id; _r.redo_len = 0; _r.undo_len = 0; left->header.lsn = wal_.append(_r); }
        left_g->markDirty();

        // Mark right as FREE
        right->header.page_type = PageType::FREE;
        right->header.num_slots = 0;
        { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = right_id; _r.redo_len = 0; _r.undo_len = 0; right->header.lsn = wal_.append(_r); }
        right_g->markDirty();
    }

    // Remove separator and right child pointer from grandparent
    if (sep_idx < gp_ns - 1) {
        std::memmove(&gp->keys[sep_idx], &gp->keys[sep_idx + 1],
                     static_cast<size_t>(gp_ns - 1 - sep_idx) * sizeof(uint64_t));
        std::memmove(&gp->children[sep_idx + 1], &gp->children[sep_idx + 2],
                     static_cast<size_t>(gp_ns - 1 - sep_idx) * sizeof(uint32_t));
    }
    gp->header.num_slots = static_cast<uint16_t>(gp_ns - 1);
    { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = grandparent_id; _r.redo_len = 0; _r.undo_len = 0; gp->header.lsn = wal_.append(_r); }
    gp_g->markDirty();
}

// fix_internal_underflow: propagate merge upward after an internal node loses a key.
template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::fix_internal_underflow(
    std::vector<std::pair<uint32_t, int>>& path_with_idx, int level)
{
    // path_with_idx[level] = {underflowing_node_id, child_idx_it_took_in_its_parent}
    // path_with_idx[level-1] = {grandparent_id, ...}
    uint32_t node_id = path_with_idx[level].first;

    // Check if this IS the root
    uint32_t cur_root = root_page_id();
    if (node_id == cur_root) {
        // Root collapse: if root has 0 keys, promote single child
        auto root_g = pool_.fetchPage(node_id);
        assert(root_g.has_value());
        auto* root = reinterpret_cast<InternalNode*>(root_g->data());
        if (root->header.num_slots == 0) {
            uint32_t surviving_child = root->children[0];
            root->header.page_type   = PageType::FREE;
            root->header.num_slots   = 0;
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = node_id; _r.redo_len = 0; _r.undo_len = 0; root->header.lsn = wal_.append(_r); }
            root_g->markDirty();
            set_root_page_id(surviving_child);
            uint32_t old_h = height();
            set_height(old_h - 1);
        }
        return;
    }

    // Check underflow threshold for internal nodes: must have >= SPLIT_HALF - 1 keys
    {
        auto g = pool_.fetchPage(node_id);
        assert(g.has_value());
        const auto* node = reinterpret_cast<const InternalNode*>(g->data());
        if (node->header.num_slots >= static_cast<uint16_t>(SPLIT_HALF - 1)) {
            return;  // no underflow
        }
    }

    if (level == 0) {
        // This should not happen: level 0 is the root, handled above
        return;
    }

    uint32_t grandparent_id = path_with_idx[level - 1].first;
    int child_idx = find_child_idx_in_parent(grandparent_id, node_id);
    if (child_idx < 0) return;

    if (redistribute_internal(grandparent_id, child_idx)) {
        return;
    }

    // Merge
    merge_internal(grandparent_id, child_idx);

    // Check if grandparent is now underflowing
    fix_internal_underflow(path_with_idx, level - 1);
}

// remove(): main entry point for deletion.
template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::remove(uint64_t key)
{
    std::vector<std::pair<uint32_t, int>> path_with_idx;
    uint32_t leaf_id = findLeafWithChildIdx(key, path_with_idx);

    if (!remove_from_leaf(leaf_id, key)) return false;

    // Check for underflow
    uint16_t ns;
    {
        auto g = pool_.fetchPage(leaf_id);
        assert(g.has_value());
        ns = reinterpret_cast<const LeafNode*>(g->data())->header.num_slots;
    }

    if (ns >= static_cast<uint16_t>(SPLIT_HALF)) return true;  // no underflow

    // Leaf is root: no siblings, cannot merge/redistribute. Root leaf can be empty.
    if (path_with_idx.empty()) return true;

    uint32_t parent_id = path_with_idx.back().first;
    int child_idx      = find_child_idx_in_parent(parent_id, leaf_id);
    if (child_idx < 0) return true;  // shouldn't happen

    if (redistribute_leaves(parent_id, child_idx)) {
        return true;
    }

    // Merge leaves
    merge_leaves(parent_id, child_idx);

    // Check if parent (internal node) is now underflowing or is root with 0 keys
    uint32_t cur_root = root_page_id();
    if (parent_id == cur_root) {
        // Root collapse check
        auto root_g = pool_.fetchPage(parent_id);
        assert(root_g.has_value());
        auto* root = reinterpret_cast<InternalNode*>(root_g->data());
        if (root->header.num_slots == 0) {
            uint32_t surviving_child = root->children[0];
            root->header.page_type   = PageType::FREE;
            root->header.num_slots   = 0;
            { WalRecord _r; _r.record_type = WalRecordType::UPDATE; _r.page_id = parent_id; _r.redo_len = 0; _r.undo_len = 0; root->header.lsn = wal_.append(_r); }
            root_g->markDirty();
            set_root_page_id(surviving_child);
            uint32_t old_h = height();
            set_height(old_h - 1);
        }
        return true;
    }

    // Propagate underflow upward
    fix_internal_underflow(path_with_idx, static_cast<int>(path_with_idx.size()) - 1);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 5 — scan() iterator
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::Iterator::advance_to_next_valid()
{
    while (current_leaf_id_ != INVALID_PAGE_ID) {
        auto g = tree_->pool_.fetchPage(current_leaf_id_);
        if (!g.has_value()) {
            valid_ = false;
            return;
        }
        const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());
        int n = static_cast<int>(leaf->header.num_slots);
        if (slot_idx_ < n) {
            if (leaf->entries[slot_idx_].key <= hi_) {
                valid_ = true;
                return;
            } else {
                // All remaining entries in this and future leaves are > hi_
                valid_ = false;
                return;
            }
        }
        // Follow leaf chain
        uint32_t next = leaf->header.next_leaf_id;
        current_leaf_id_ = next;
        slot_idx_ = 0;
    }
    valid_ = false;
}

template <typename WAL_T, typename MVCC_T>
bool BPlusTree<WAL_T, MVCC_T>::Iterator::valid() const
{
    return valid_;
}

template <typename WAL_T, typename MVCC_T>
uint64_t BPlusTree<WAL_T, MVCC_T>::Iterator::key() const
{
    auto g = tree_->pool_.fetchPage(current_leaf_id_);
    assert(g.has_value());
    const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());
    return leaf->entries[slot_idx_].key;
}

template <typename WAL_T, typename MVCC_T>
uint64_t BPlusTree<WAL_T, MVCC_T>::Iterator::value() const
{
    auto g = tree_->pool_.fetchPage(current_leaf_id_);
    assert(g.has_value());
    const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());
    return leaf->entries[slot_idx_].value;
}

template <typename WAL_T, typename MVCC_T>
void BPlusTree<WAL_T, MVCC_T>::Iterator::next()
{
    if (!valid_) return;
    slot_idx_++;
    advance_to_next_valid();
}

template <typename WAL_T, typename MVCC_T>
typename BPlusTree<WAL_T, MVCC_T>::Iterator BPlusTree<WAL_T, MVCC_T>::scan(uint64_t lo, uint64_t hi)
{
    Iterator it;
    it.tree_            = this;
    it.hi_              = hi;
    it.valid_           = false;
    it.slot_idx_        = 0;
    it.current_leaf_id_ = INVALID_PAGE_ID;

    // Descend to the leaf that would contain lo
    uint32_t cur = root_page_id();
    while (true) {
        auto g = pool_.fetchPage(cur);
        assert(g.has_value());
        const auto* hdr = reinterpret_cast<const BPHeader*>(g->data());
        if (hdr->page_type == PageType::LEAF) {
            it.current_leaf_id_ = cur;
            break;
        }
        const auto* node = reinterpret_cast<const InternalNode*>(g->data());
        int ci = findChildIndex(node, lo);
        // findChildIndex returns i such that children[i] is the correct subtree for lo.
        // For a lower-bound search we need the child that might contain lo.
        // When lo == keys[i], findChildIndex returns i+1 (since keys[mid] <= lo → lo = mid+1).
        // That means we go to the right of the separator, which is correct for keys >= lo.
        // However, for lo = 0 or values less than the first key, ci=0 is also correct.
        // Adjust: for scan we want the leftmost leaf that could contain a key >= lo.
        // If lo > 0, we may need ci-1 to catch keys equal to a separator.
        // Actually findChildIndex already handles this: it returns the child containing keys
        // strictly > all keys[0..ci-1], which is what we want for lower-bound.
        cur = node->children[ci];
    }

    // Position to first slot >= lo
    {
        auto g = pool_.fetchPage(it.current_leaf_id_);
        assert(g.has_value());
        const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());
        int insert_pos = 0;
        findSlotInLeaf(leaf, lo, &insert_pos);
        it.slot_idx_ = insert_pos;
        int n = static_cast<int>(leaf->header.num_slots);
        if (insert_pos >= n) {
            // lo is past all entries in this leaf; follow chain
            it.current_leaf_id_ = leaf->header.next_leaf_id;
            it.slot_idx_ = 0;
        }
    }

    it.advance_to_next_valid();
    return it;
}

// ─────────────────────────────────────────────────────────────────────────────
// Explicit template instantiations
// ─────────────────────────────────────────────────────────────────────────────
// Since BPlusTree is templated and the implementation is in a .cpp file (not
// a header-only library), we must explicitly instantiate for each WAL type
// used by tests.  NullWAL is the only one used in Phase 3.

template class BPlusTree<NullWAL>;  // BPlusTree<NullWAL, TrivialMvcc> via default

}  // namespace adapttree
