#include "adapttree/wal.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

// ── CRC32C (Castagnoli polynomial 0x82F82011 reflected) ───────────────────────

static std::array<uint32_t, 256> make_crc32c_table() noexcept {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ (0x82F82011u & -(c & 1u));
        t[i] = c;
    }
    return t;
}

static const auto kCrc32cTable = make_crc32c_table();

static uint32_t crc32c(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ kCrc32cTable[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

// ── WalRecord::serialize ──────────────────────────────────────────────────────

std::vector<uint8_t> WalRecord::serialize() const {
    const size_t total = serialized_size(redo_len, undo_len);
    std::vector<uint8_t> buf(total, 0);

    size_t off = 0;
    std::memcpy(buf.data() + off, &lsn,      8); off += 8;
    std::memcpy(buf.data() + off, &prev_lsn, 8); off += 8;
    std::memcpy(buf.data() + off, &txn_id,   8); off += 8;
    std::memcpy(buf.data() + off, &page_id,  4); off += 4;
    buf[off++] = static_cast<uint8_t>(record_type);
    std::memcpy(buf.data() + off, &redo_len, 2); off += 2;
    std::memcpy(buf.data() + off, &undo_len, 2); off += 2;
    buf[off++] = 0; // _reserved — off == 34 == kFixedHeaderSize

    if (redo_len > 0)
        std::memcpy(buf.data() + off, redo_data.data(), redo_len);
    off += redo_len;
    if (undo_len > 0)
        std::memcpy(buf.data() + off, undo_data.data(), undo_len);
    off += undo_len;

    uint32_t crc = crc32c(buf.data(), total - 4);
    std::memcpy(buf.data() + off, &crc, 4);
    return buf;
}

// ── WalRecord::deserialize ────────────────────────────────────────────────────

std::optional<WalRecord> WalRecord::deserialize(std::span<const uint8_t> buf) {
    if (buf.size() < kFixedHeaderSize + 4) return std::nullopt;

    WalRecord r;
    size_t off = 0;
    std::memcpy(&r.lsn,      buf.data() + off, 8); off += 8;
    std::memcpy(&r.prev_lsn, buf.data() + off, 8); off += 8;
    std::memcpy(&r.txn_id,   buf.data() + off, 8); off += 8;
    std::memcpy(&r.page_id,  buf.data() + off, 4); off += 4;
    r.record_type = static_cast<WalRecordType>(buf[off++]);
    std::memcpy(&r.redo_len, buf.data() + off, 2); off += 2;
    std::memcpy(&r.undo_len, buf.data() + off, 2); off += 2;
    off++; // _reserved

    const size_t total = serialized_size(r.redo_len, r.undo_len);
    if (buf.size() < total) return std::nullopt;

    // Verify CRC
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + total - 4, 4);
    if (crc32c(buf.data(), total - 4) != stored_crc) return std::nullopt;

    r.redo_data.assign(buf.data() + off, buf.data() + off + r.redo_len);
    off += r.redo_len;
    r.undo_data.assign(buf.data() + off, buf.data() + off + r.undo_len);
    return r;
}

// ── Wal ──────────────────────────────────────────────────────────────────────

Wal::Wal(std::string_view path) : path_(path) {
    fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) throw std::runtime_error("Wal: failed to open " + path_);
}

Wal::~Wal() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

uint64_t Wal::append(WalRecord& record) {
    std::lock_guard<std::mutex> lock(append_mutex_);
    uint64_t lsn = next_lsn_.fetch_add(1, std::memory_order_relaxed);
    record.lsn = lsn;
    auto buf = record.serialize();
    const uint8_t* ptr = buf.data();
    size_t remaining = buf.size();
    while (remaining > 0) {
        ssize_t written = write(fd_, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("WAL write failed");
        }
        ptr += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }
    return lsn;
}

void Wal::flush_to(uint64_t lsn) {
    fsync(fd_);
    uint64_t cur = flushed_lsn_.load(std::memory_order_acquire);
    while (lsn > cur) {
        if (flushed_lsn_.compare_exchange_weak(cur, lsn,
                std::memory_order_release, std::memory_order_acquire))
            break;
    }
}

void Wal::checkpoint(std::function<void()> flush_dirty_pages) {
    // Step 1: append CHECKPOINT record
    WalRecord rec;
    rec.record_type = WalRecordType::CHECKPOINT;
    rec.redo_len = 0; rec.undo_len = 0;
    uint64_t ckpt_lsn = append(rec);

    // Step 2: fsync WAL
    flush_to(ckpt_lsn);

    // Step 3: flush dirty pages (caller's responsibility)
    flush_dirty_pages();

    // Step 4: write checkpoint LSN to .ckpt sidecar file
    std::string ckpt_path = path_ + ".ckpt";
    int cfd = open(ckpt_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cfd < 0) return;

    // Write 8B lsn + 4B crc32c(lsn) + zero-pad to 4096B
    std::vector<uint8_t> ckpt_buf(4096, 0);
    std::memcpy(ckpt_buf.data(), &ckpt_lsn, 8);
    uint32_t crc = crc32c(reinterpret_cast<const uint8_t*>(&ckpt_lsn), 8);
    std::memcpy(ckpt_buf.data() + 8, &crc, 4);

    const uint8_t* ptr = ckpt_buf.data();
    size_t rem = ckpt_buf.size();
    while (rem > 0) {
        ssize_t n = write(cfd, ptr, rem);
        if (n < 0) { if (errno == EINTR) continue; break; }
        ptr += static_cast<size_t>(n); rem -= static_cast<size_t>(n);
    }
    fdatasync(cfd);
    close(cfd);
}

uint64_t Wal::last_checkpoint_lsn() const {
    std::string ckpt_path = path_ + ".ckpt";
    int cfd = open(ckpt_path.c_str(), O_RDONLY);
    if (cfd < 0) return 0;
    uint8_t buf[12];
    ssize_t n = read(cfd, buf, 12);
    close(cfd);
    if (n < 12) return 0;
    uint64_t stored_lsn = 0;
    uint32_t stored_crc = 0;
    std::memcpy(&stored_lsn, buf,     8);
    std::memcpy(&stored_crc, buf + 8, 4);
    if (crc32c(reinterpret_cast<const uint8_t*>(&stored_lsn), 8) != stored_crc) return 0;
    return stored_lsn;
}

std::vector<WalRecord> Wal::read_all() const {
    std::vector<WalRecord> records;
    int rfd = open(path_.c_str(), O_RDONLY);
    if (rfd < 0) return records;

    while (true) {
        uint8_t hdr[WalRecord::kFixedHeaderSize];
        ssize_t n = read(rfd, hdr, WalRecord::kFixedHeaderSize);
        if (n < static_cast<ssize_t>(WalRecord::kFixedHeaderSize)) break;

        uint16_t redo_len = 0, undo_len = 0;
        std::memcpy(&redo_len, hdr + 29, 2);
        std::memcpy(&undo_len, hdr + 31, 2);

        size_t remain = static_cast<size_t>(redo_len) + undo_len + 4;
        std::vector<uint8_t> full(WalRecord::kFixedHeaderSize + remain);
        std::memcpy(full.data(), hdr, WalRecord::kFixedHeaderSize);
        n = read(rfd, full.data() + WalRecord::kFixedHeaderSize, remain);
        if (n < static_cast<ssize_t>(remain)) break;

        auto rec = WalRecord::deserialize(std::span<const uint8_t>(full));
        if (!rec) break;
        records.push_back(std::move(*rec));
    }

    close(rfd);
    return records;
}
