#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace adapttree {

// ── Constants ─────────────────────────────────────────────────────────────────
inline constexpr size_t   PAGE_SIZE       = 4096;
inline constexpr uint32_t INVALID_PAGE_ID = std::numeric_limits<uint32_t>::max();

using page_id_t = uint32_t;

// ── DiskManager (minimal interface for buffer pool) ───────────────────────────
class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();
    DiskManager(const DiskManager&)            = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    page_id_t allocatePage();
    bool readPage(page_id_t page_id, std::byte* buf);
    bool writePage(page_id_t page_id, const std::byte* buf);
    void flush();

private:
    int    fd_   = -1;
    size_t next_ = 0;
};

// ── Frame ─────────────────────────────────────────────────────────────────────
struct Frame {
    alignas(PAGE_SIZE) std::byte data[PAGE_SIZE]{};

    page_id_t page_id      = INVALID_PAGE_ID;
    int32_t   pin_count    = 0;
    bool      dirty        = false;
    uint64_t  page_lsn     = 0;

    uint64_t  t_last        = 0;
    uint64_t  t_second_last = 0;
    uint32_t  access_count  = 0;

    void reset() noexcept {
        page_id      = INVALID_PAGE_ID;
        pin_count    = 0;
        dirty        = false;
        page_lsn     = 0;
        t_last       = 0;
        t_second_last = 0;
        access_count = 0;
    }
};

// ── BufferPoolBase — non-template base so PageGuard can call unpin ─────────────
class BufferPoolBase {
public:
    virtual ~BufferPoolBase() = default;
    virtual void unpin(page_id_t page_id, bool dirty) = 0;
};

// ── PageGuard (RAII) ──────────────────────────────────────────────────────────
class PageGuard {
public:
    PageGuard(BufferPoolBase* pool, Frame* frame) noexcept
        : pool_(pool), frame_(frame), dirty_(false) {}

    PageGuard(const PageGuard&)            = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    PageGuard(PageGuard&& o) noexcept
        : pool_(o.pool_), frame_(o.frame_), dirty_(o.dirty_) {
        o.frame_ = nullptr;
        o.pool_  = nullptr;
    }
    PageGuard& operator=(PageGuard&& o) noexcept {
        if (this != &o) {
            if (frame_ && pool_) pool_->unpin(frame_->page_id, dirty_);
            pool_    = o.pool_;
            frame_   = o.frame_;
            dirty_   = o.dirty_;
            o.frame_ = nullptr;
            o.pool_  = nullptr;
        }
        return *this;
    }

    ~PageGuard() {
        if (frame_ && pool_) pool_->unpin(frame_->page_id, dirty_);
    }

    void markDirty()         noexcept { dirty_ = true; }
    void setPageLsn(uint64_t lsn) noexcept { if (frame_) frame_->page_lsn = lsn; }

    std::byte*       data()    noexcept { return frame_ ? frame_->data : nullptr; }
    const std::byte* data()    const noexcept { return frame_ ? frame_->data : nullptr; }
    page_id_t        page_id() const noexcept { return frame_ ? frame_->page_id : INVALID_PAGE_ID; }
    bool             valid()   const noexcept { return frame_ != nullptr; }

private:
    BufferPoolBase* pool_  = nullptr;
    Frame*          frame_ = nullptr;
    bool            dirty_ = false;
};

// ── BufferPool<WAL> ───────────────────────────────────────────────────────────
// WAL must expose: uint64_t flushed_lsn; void flush_to(uint64_t);
template <typename WAL>
class BufferPool : public BufferPoolBase {
public:
    explicit BufferPool(DiskManager* dm, WAL* wal, size_t pool_size = 256);
    ~BufferPool() override = default;

    std::optional<PageGuard> fetchPage(page_id_t page_id);
    std::optional<PageGuard> newPage();
    void unpin(page_id_t page_id, bool dirty) override;

private:
    Frame* pinFrame(page_id_t page_id);
    Frame* selectVictim();
    void   evictFrame(Frame* frame);
    void   recordAccess(Frame* frame);

    DiskManager*                          dm_;
    WAL*                                  wal_;
    std::vector<Frame>                    frames_;
    std::unordered_map<page_id_t, Frame*> page_table_;
    std::mutex                            latch_;
    std::atomic<uint64_t>                 tick_{1};
};

// ── Template implementation (included here — single TU) ───────────────────────

template <typename WAL>
BufferPool<WAL>::BufferPool(DiskManager* dm, WAL* wal, size_t pool_size)
    : dm_(dm), wal_(wal), frames_(pool_size) {}

template <typename WAL>
void BufferPool<WAL>::recordAccess(Frame* frame) {
    if (frame->access_count >= 1) frame->t_second_last = frame->t_last;
    frame->t_last = tick_.fetch_add(1, std::memory_order_relaxed);
    if (frame->access_count < 2) ++frame->access_count;
}

template <typename WAL>
Frame* BufferPool<WAL>::selectVictim() {
    uint64_t t_now      = tick_.load(std::memory_order_relaxed);
    Frame*   victim     = nullptr;
    uint64_t best_bk    = 0;
    uint64_t best_tlast = std::numeric_limits<uint64_t>::max();

    for (auto& f : frames_) {
        if (f.pin_count > 0) continue;

        if (f.page_id == INVALID_PAGE_ID) {
            // Empty frame — best possible victim
            victim     = &f;
            best_bk    = std::numeric_limits<uint64_t>::max();
            best_tlast = 0;
            break;  // can't do better
        }

        uint64_t bk = (f.access_count < 2)
                      ? std::numeric_limits<uint64_t>::max()
                      : (t_now - f.t_second_last);

        bool better = (bk > best_bk) || (bk == best_bk && f.t_last < best_tlast);
        if (better) {
            victim     = &f;
            best_bk    = bk;
            best_tlast = f.t_last;
        }
    }
    return victim;
}

template <typename WAL>
void BufferPool<WAL>::evictFrame(Frame* frame) {
    if (frame->page_id == INVALID_PAGE_ID) return;

    if (frame->dirty) {
        wal_->flush_to(frame->page_lsn);
        dm_->writePage(frame->page_id, frame->data);
    }
    page_table_.erase(frame->page_id);
    frame->reset();
}

template <typename WAL>
Frame* BufferPool<WAL>::pinFrame(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Frame* f = it->second;
        ++f->pin_count;
        recordAccess(f);
        return f;
    }
    Frame* f = selectVictim();
    if (!f) return nullptr;
    evictFrame(f);
    if (!dm_->readPage(page_id, f->data)) return nullptr;
    f->page_id   = page_id;
    f->pin_count = 1;
    f->dirty     = false;
    f->page_lsn  = 0;
    recordAccess(f);
    page_table_[page_id] = f;
    return f;
}

template <typename WAL>
std::optional<PageGuard> BufferPool<WAL>::fetchPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    Frame* f = pinFrame(page_id);
    if (!f) return std::nullopt;
    return PageGuard(this, f);
}

template <typename WAL>
std::optional<PageGuard> BufferPool<WAL>::newPage() {
    std::lock_guard<std::mutex> lock(latch_);
    Frame* f = selectVictim();
    if (!f) return std::nullopt;
    evictFrame(f);
    page_id_t pid = dm_->allocatePage();
    f->page_id   = pid;
    f->pin_count = 1;
    f->dirty     = false;
    f->page_lsn  = 0;
    std::memset(f->data, 0, PAGE_SIZE);
    recordAccess(f);
    page_table_[pid] = f;
    return PageGuard(this, f);
}

template <typename WAL>
void BufferPool<WAL>::unpin(page_id_t page_id, bool dirty) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    Frame* f = it->second;
    if (f->pin_count > 0) --f->pin_count;
    if (dirty) f->dirty = true;
}

}  // namespace adapttree
