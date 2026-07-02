#pragma once
#include "adapttree/wal.hpp"
#include <vector>
#include <cstdint>

// SpyWal: records every append call for unit-test verification.
struct AppendEvent {
    uint64_t      lsn;
    uint32_t      page_id;
    WalRecordType type;
};

struct SpyWal {
    uint64_t append(WalRecord& r) {
        uint64_t lsn = next_lsn_++;
        r.lsn = lsn;
        events.push_back({lsn, r.page_id, r.record_type});
        return lsn;
    }
    void     flush_to(uint64_t) {}
    uint64_t flushed_lsn() const { return UINT64_MAX; }

    std::vector<AppendEvent> events;
private:
    uint64_t next_lsn_{1};
};
