#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adapttree {

struct OldVersion {
    uint64_t value;
    uint64_t commit_ts;
};

struct Transaction {
    uint64_t txn_id;   // monotonic ID assigned at begin()
    uint64_t read_ts;  // snapshot of txn_id counter at begin()
    uint64_t write_ts; // assigned at commit() from same counter
};

} // namespace adapttree

// Hash specialization for std::pair<uint64_t, uint32_t>
namespace std {
template <>
struct hash<std::pair<uint64_t, uint32_t>> {
    std::size_t operator()(const std::pair<uint64_t, uint32_t>& p) const noexcept {
        std::size_t h1 = std::hash<uint64_t>{}(p.first);
        std::size_t h2 = std::hash<uint32_t>{}(p.second);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x6c62272e07bb0142ULL + (h1 << 6) + (h1 >> 2));
    }
};
} // namespace std

namespace adapttree {

class MVCC {
public:
    Transaction begin();
    void commit(Transaction& txn);
    void abort(Transaction& txn);

    void archive_version(uint64_t page_id, uint32_t slot_idx,
                         uint64_t old_value, uint64_t commit_ts);

    std::optional<uint64_t> read_version(const Transaction& txn,
                                          uint64_t page_id, uint32_t slot_idx,
                                          uint64_t current_value,
                                          uint64_t current_commit_ts) const;

    void gc();
    uint64_t oldest_active_read_ts() const;

private:
    std::atomic<uint64_t> next_txn_id_{1};
    mutable std::mutex mvcc_mutex_;
    std::set<uint64_t> active_txns_;
    std::unordered_map<std::pair<uint64_t, uint32_t>, std::vector<OldVersion>> old_versions_;
};

} // namespace adapttree
