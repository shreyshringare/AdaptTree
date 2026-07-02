#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Forward declaration — WalRecord is defined later in this header.
struct WalRecord;

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
    // New WalRecord-based interface for BPlusTree<NullWAL> usage (defined after WalRecord below)
    uint64_t append(WalRecord& r);
    void     flush_to(uint64_t /*target_lsn*/) override {}
    uint64_t flushed_lsn() const override { return UINT64_MAX; }
};

// ── Phase 6: WAL Record ───────────────────────────────────────────────────────

enum class WalRecordType : uint8_t {
    INSERT = 1, UPDATE = 2, DELETE = 3,
    COMMIT = 4, ABORT  = 5, CHECKPOINT = 6,
};

// WAL fixed header layout (34 bytes, little-endian, manually serialized):
//   [0:8)   lsn
//   [8:16)  prev_lsn
//   [16:24) txn_id
//   [24:28) page_id
//   [28]    record_type
//   [29:31) redo_len   (uint16_t LE)
//   [31:33) undo_len   (uint16_t LE)
//   [33]    _reserved  (= 0)
// Followed by: redo_data[redo_len], undo_data[undo_len], crc32c[4]

struct WalRecord {
    uint64_t      lsn         = 0;
    uint64_t      prev_lsn    = 0;
    uint64_t      txn_id      = 0;
    uint32_t      page_id     = 0;
    WalRecordType record_type = WalRecordType::INSERT;
    uint16_t      redo_len    = 0;
    uint16_t      undo_len    = 0;
    std::vector<uint8_t> redo_data;
    std::vector<uint8_t> undo_data;

    static constexpr size_t kFixedHeaderSize = 34;

    static constexpr size_t serialized_size(uint16_t r, uint16_t u) {
        return kFixedHeaderSize + r + u + 4;
    }

    std::vector<uint8_t> serialize() const;
    static std::optional<WalRecord> deserialize(std::span<const uint8_t> buf);
};

// NullWAL::append(WalRecord&) — defined here after WalRecord is complete.
inline uint64_t NullWAL::append(WalRecord& r) { r.lsn = UINT64_MAX; return UINT64_MAX; }

// ── Wal class ─────────────────────────────────────────────────────────────────

class Wal {
public:
    explicit Wal(std::string_view path);
    ~Wal();

    uint64_t append(WalRecord& record);
    void flush_to(uint64_t lsn);
    void checkpoint(std::function<void()> flush_dirty_pages);
    uint64_t flushed_lsn() const { return flushed_lsn_.load(std::memory_order_acquire); }
    uint64_t current_lsn() const { return next_lsn_.load(std::memory_order_acquire) - 1; }
    uint64_t last_checkpoint_lsn() const;
    std::vector<WalRecord> read_all() const;

private:
    std::string path_;
    int fd_ = -1;
    std::atomic<uint64_t> next_lsn_{1};
    std::atomic<uint64_t> flushed_lsn_{0};
    mutable std::mutex append_mutex_;
};
