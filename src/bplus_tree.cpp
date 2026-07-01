#include "adapttree/bplus_tree.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace adapttree {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T>
BPlusTree<WAL_T>::BPlusTree(BufferPool<WAL_T>& pool, WAL_T& wal)
    : pool_(pool), wal_(wal)
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
            meta->header.lsn          = wal_.append(META_PAGE_ID, nullptr, 0);
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
            root->header.lsn          = wal_.append(root_g->page_id(), nullptr, 0);
            root_g->markDirty();
        }
    }
    // else: existing database — meta page already correct in pool
}

// ─────────────────────────────────────────────────────────────────────────────
// Meta-page helpers (each fetches the meta page, modifies, and releases)
// ─────────────────────────────────────────────────────────────────────────────

template <typename WAL_T>
uint32_t BPlusTree<WAL_T>::root_page_id()
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    return reinterpret_cast<const MetaPage*>(g->data())->root_page_id;
}

template <typename WAL_T>
void BPlusTree<WAL_T>::set_root_page_id(uint32_t new_root_id)
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    auto* meta           = reinterpret_cast<MetaPage*>(g->data());
    meta->root_page_id   = new_root_id;
    meta->header.lsn     = wal_.append(META_PAGE_ID, nullptr, 0);
    g->markDirty();
}

template <typename WAL_T>
void BPlusTree<WAL_T>::set_height(uint32_t h)
{
    auto g = pool_.fetchPage(META_PAGE_ID);
    assert(g.has_value());
    auto* meta         = reinterpret_cast<MetaPage*>(g->data());
    meta->tree_height  = h;
    meta->header.lsn   = wal_.append(META_PAGE_ID, nullptr, 0);
    g->markDirty();
}

template <typename WAL_T>
uint32_t BPlusTree<WAL_T>::height()
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
template <typename WAL_T>
int BPlusTree<WAL_T>::findChildIndex(const InternalNode* node,
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
template <typename WAL_T>
int BPlusTree<WAL_T>::findSlotInLeaf(const LeafNode* leaf,
                                      uint64_t        target,
                                      int*            insert_pos) const
{
    int n  = static_cast<int>(leaf->header.num_slots);
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (leaf->entries[mid].key < target)       lo = mid + 1;
        else if (leaf->entries[mid].key > target)  hi = mid;
        else {
            if (insert_pos) *insert_pos = mid;
            return mid;
        }
    }
    if (insert_pos) *insert_pos = lo;
    return -1;
}

// findLeaf: descend from root to leaf.
// Appends internal-node IDs to path.  Returns leaf page ID.
template <typename WAL_T>
uint32_t BPlusTree<WAL_T>::findLeaf(uint64_t key, std::vector<uint32_t>& path)
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

template <typename WAL_T>
std::optional<uint64_t> BPlusTree<WAL_T>::get(uint64_t key)
{
    std::vector<uint32_t> path;
    uint32_t leaf_id = findLeaf(key, path);

    auto g = pool_.fetchPage(leaf_id);
    if (!g) return std::nullopt;
    const auto* leaf = reinterpret_cast<const LeafNode*>(g->data());

    int idx = findSlotInLeaf(leaf, key);
    if (idx < 0) return std::nullopt;
    return leaf->entries[idx].value;
}

// ─────────────────────────────────────────────────────────────────────────────
// insert_into_parent()
// ─────────────────────────────────────────────────────────────────────────────
// Insert fence_key and right_child_id into the parent of left_child_id.
// path: list of internal node IDs from root down to (but not including) the
//       node whose parent we are modifying.  The last element of path is the
//       direct parent.
// If path is empty, left_child_id is the current root → create a new root.

template <typename WAL_T>
bool BPlusTree<WAL_T>::insert_into_parent(const std::vector<uint32_t>& path,
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
        nr->header.lsn          = wal_.append(new_root_id, nullptr, 0);
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
            parent->header.lsn        = wal_.append(parent_id, nullptr, 0);
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
        parent->header.lsn       = wal_.append(parent_id, nullptr, 0);
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
        rn->header.lsn          = wal_.append(right_int_id, nullptr, 0);

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

template <typename WAL_T>
bool BPlusTree<WAL_T>::insert(uint64_t key, uint64_t value)
{
    std::vector<uint32_t> path;
    uint32_t leaf_id = findLeaf(key, path);

    {
        auto leaf_g = pool_.fetchPage(leaf_id);
        assert(leaf_g.has_value());
        auto* leaf = reinterpret_cast<LeafNode*>(leaf_g->data());

        int insert_pos = 0;
        int found = findSlotInLeaf(leaf, key, &insert_pos);
        if (found >= 0) return false;   // duplicate

        int n = static_cast<int>(leaf->header.num_slots);

        if (n < static_cast<int>(ORDER)) {
            // Leaf has space
            if (insert_pos < n) {
                std::memmove(&leaf->entries[insert_pos + 1],
                             &leaf->entries[insert_pos],
                             static_cast<size_t>(n - insert_pos) * sizeof(LeafEntry));
            }
            leaf->entries[insert_pos] = {key, value};
            leaf->header.num_slots    = static_cast<uint16_t>(n + 1);
            leaf->header.lsn          = wal_.append(leaf_id, nullptr, 0);
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

        // Write SPLIT_HALF entries into left leaf
        std::memcpy(leaf->entries, all_entries, SPLIT_HALF * sizeof(LeafEntry));
        leaf->header.num_slots    = static_cast<uint16_t>(SPLIT_HALF);
        leaf->header.lsn          = wal_.append(leaf_id, nullptr, 0);
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
        right->header.lsn          = wal_.append(right_id, nullptr, 0);

        std::memcpy(right->entries,
                    &all_entries[SPLIT_HALF],
                    (ORDER + 1 - SPLIT_HALF) * sizeof(LeafEntry));
        right_g->markDirty();

        // Update left leaf's next_leaf_id → right_id
        // leaf_g still alive at this point so we can update through it
        leaf->header.next_leaf_id = right_id;
        leaf->header.lsn          = wal_.append(leaf_id, nullptr, 0);
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
// Explicit template instantiations
// ─────────────────────────────────────────────────────────────────────────────
// Since BPlusTree is templated and the implementation is in a .cpp file (not
// a header-only library), we must explicitly instantiate for each WAL type
// used by tests.  NullWAL is the only one used in Phase 3.

template class BPlusTree<NullWAL>;

}  // namespace adapttree
