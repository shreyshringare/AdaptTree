#include "adapttree/mvcc.hpp"
#include <climits>
#include <iterator>

namespace adapttree {

Transaction MVCC::begin() {
    uint64_t id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
    Transaction txn{id, id, 0};
    {
        std::lock_guard<std::mutex> lock(mvcc_mutex_);
        active_txns_.insert(id);
    }
    return txn;
}

void MVCC::commit(Transaction& txn) {
    uint64_t wts = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
    txn.write_ts = wts;
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    active_txns_.erase(txn.txn_id);
}

void MVCC::abort(Transaction& txn) {
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    active_txns_.erase(txn.txn_id);
}

void MVCC::archive_version(uint64_t page_id, uint32_t slot_idx,
                            uint64_t old_value, uint64_t commit_ts) {
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    old_versions_[{page_id, slot_idx}].push_back({old_value, commit_ts});
}

std::optional<uint64_t> MVCC::read_version(const Transaction& txn,
                                             uint64_t page_id, uint32_t slot_idx,
                                             uint64_t current_value,
                                             uint64_t current_commit_ts) const {
    // Rule 1: if current (inline) slot is visible, return it
    if (current_commit_ts <= txn.read_ts) {
        return current_value;
    }

    // Rule 2: walk old_versions from back (newest) to front (oldest)
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    auto it = old_versions_.find({page_id, slot_idx});
    if (it == old_versions_.end()) {
        return std::nullopt;
    }
    const auto& versions = it->second;
    // Walk newest-to-oldest
    for (auto rit = versions.rbegin(); rit != versions.rend(); ++rit) {
        if (rit->commit_ts <= txn.read_ts) {
            return rit->value;
        }
    }

    return std::nullopt;
}

void MVCC::gc() {
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    uint64_t min_ts = active_txns_.empty()
                          ? UINT64_MAX
                          : *active_txns_.begin();

    for (auto& [key, versions] : old_versions_) {
        // Remove from front while commit_ts < min_ts AND a newer entry exists
        auto it = versions.begin();
        while (it != versions.end()) {
            if (it->commit_ts < min_ts && std::next(it) != versions.end()) {
                it = versions.erase(it);
            } else {
                break;
            }
        }
    }

    // Remove map entries with empty vectors
    for (auto it = old_versions_.begin(); it != old_versions_.end(); ) {
        if (it->second.empty()) {
            it = old_versions_.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t MVCC::oldest_active_read_ts() const {
    std::lock_guard<std::mutex> lock(mvcc_mutex_);
    if (active_txns_.empty()) {
        return UINT64_MAX;
    }
    return *active_txns_.begin();
}

} // namespace adapttree
