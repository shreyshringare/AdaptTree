#pragma once
#include <cstdint>
#include <cstddef>

// WAL (Write-Ahead Log) interface — real implementation deferred to Phase 6.
// BPlusTree calls append() before marking a page dirty, then flush_to() before
// writing dirty pages to disk.  The NullWAL stub satisfies duck-typing for tests.

struct WAL {
    virtual ~WAL() = default;

    // Append a redo record for page_id.  Returns the LSN assigned to this record.
    virtual uint64_t append(uint32_t page_id,
                            const std::byte* redo_data,
                            uint16_t         redo_len) = 0;

    // Force all log records up to and including target_lsn to stable storage.
    virtual void flush_to(uint64_t target_lsn) = 0;

    // Return the highest LSN that has been flushed to stable storage.
    virtual uint64_t flushed_lsn() const = 0;
};

// NullWAL: no-op stub used in unit tests.
// Returns UINT64_MAX as LSN so WAL-before-data comparisons always pass.
struct NullWAL final : WAL {
    uint64_t append(uint32_t /*page_id*/,
                    const std::byte* /*redo_data*/,
                    uint16_t /*redo_len*/) override {
        return UINT64_MAX;
    }
    void     flush_to(uint64_t /*target_lsn*/) override {}
    uint64_t flushed_lsn() const override { return UINT64_MAX; }
};
