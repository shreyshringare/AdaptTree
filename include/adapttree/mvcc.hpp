#pragma once
#include <cstdint>

// MVCC (Multi-Version Concurrency Control) interface — real implementation in Phase 5.
// Phase 3 does not use MVCC; these stubs exist for future integration and unit tests.

struct Mvcc {
    virtual ~Mvcc() = default;

    // Begin a new transaction.  Returns a read timestamp for this transaction.
    virtual uint64_t begin() = 0;

    // Returns true if a record written at write_ts is visible to a transaction
    // with read timestamp txn_read_ts.
    virtual bool is_visible(uint64_t write_ts, uint64_t txn_read_ts) const = 0;
};

// NullMvcc: no-op stub.  All writes are immediately visible to all readers.
struct NullMvcc final : Mvcc {
    uint64_t begin() override { return 1; }
    bool is_visible(uint64_t /*write_ts*/, uint64_t /*txn_read_ts*/) const override {
        return true;
    }
};
