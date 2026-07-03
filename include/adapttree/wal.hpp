#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "adapttree/buffer_pool.hpp"

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

// ── CheckpointPolicy ──────────────────────────────────────────────────────────

struct CheckpointPolicy {
    size_t   max_wal_bytes = 64ULL * 1024 * 1024;  // 64 MB
    uint32_t interval_sec  = 30;
};

// ── Wal class ─────────────────────────────────────────────────────────────────

class Wal {
public:
    explicit Wal(std::string_view path, CheckpointPolicy policy = {});
    ~Wal();

    uint64_t append(WalRecord& record);
    void flush_to(uint64_t lsn);
    void checkpoint(std::function<void()> flush_dirty_pages);
    uint64_t flushed_lsn() const { return flushed_lsn_.load(std::memory_order_acquire); }
    uint64_t current_lsn() const { return next_lsn_.load(std::memory_order_acquire) - 1; }
    uint64_t last_checkpoint_lsn() const;
    std::vector<WalRecord> read_all() const;
    bool checkpoint_triggered() const {
        return bytes_since_checkpoint_.load(std::memory_order_relaxed) >= policy_.max_wal_bytes;
    }

    // Replay all committed WAL records to disk. Call after a crash before opening
    // a new BPlusTree on the same files. For each data record with redo_len ==
    // PAGE_SIZE where the on-disk page_lsn < record.lsn, writes redo_data to disk.
    void recover_to_disk(adapttree::DiskManager& dm);

private:
    std::string path_;
    int fd_ = -1;
    std::atomic<uint64_t> next_lsn_{1};
    std::atomic<uint64_t> flushed_lsn_{0};
    mutable std::mutex append_mutex_;
    std::mutex checkpoint_mutex_;

    CheckpointPolicy policy_;
    std::atomic<size_t> bytes_since_checkpoint_{0};
    std::atomic<bool> dirty_since_checkpoint_{false};
    std::chrono::steady_clock::time_point last_checkpoint_time_;
    std::thread bg_thread_;
    std::atomic<bool> stop_flag_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;
};

// ── ARIES Recovery ────────────────────────────────────────────────────────────

enum class TxnStatus { WINNER, LOSER };

struct RecoveryResult {
    uint64_t redo_from_lsn = 0;
    std::unordered_map<uint64_t, TxnStatus> txn_table;
    std::vector<WalRecord> redo_log;
    std::vector<WalRecord> undo_log;
};

class Recovery {
public:
    explicit Recovery(Wal& wal);

    // Phase 1: scan log from last_checkpoint_lsn, classify txns
    RecoveryResult analyze();

    // Phase 2: replay redo_log, skip pages where page_lsn >= record.lsn
    void redo(const RecoveryResult& result,
              std::function<uint64_t(uint32_t page_id)> page_lsn_provider,
              std::function<void(const WalRecord&)> redo_applier);

    // Phase 3: undo loser records in reverse LSN order
    void undo(const RecoveryResult& result,
              std::function<void(const WalRecord&)> undo_applier);

    // Convenience: all three phases
    void run(std::function<uint64_t(uint32_t)> page_lsn_provider,
             std::function<void(const WalRecord&)> redo_applier,
             std::function<void(const WalRecord&)> undo_applier);

private:
    Wal& wal_;
};
