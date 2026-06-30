#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "adapttree/page.hpp"

namespace adapttree {

// ── DiskManager ───────────────────────────────────────────────────────────────
// Owns the database file descriptor. Provides raw 4KB page I/O.
//
// Key invariants:
//   - All I/O uses pread/pwrite (positional, thread-safe, no seek state).
//   - Buffers passed to readPage/writePage MUST be 4096-byte aligned.
//     Use Page::rawBytes() — Page declares data_ as alignas(4096).
//   - Both read and write paths loop on EINTR (DISK-04).
//   - flush() calls fdatasync (DISK-03).
//   - allocatePage() writes a zero-filled page to disk, returns new page_id.
//
// Error handling: throws std::system_error on unrecoverable I/O errors.

class DiskManager {
public:
    // Opens (or creates) the database file at `path`.
    // Determines next_page_id_ from the current file size.
    explicit DiskManager(std::string_view path);

    // Closes the file descriptor.
    ~DiskManager();

    // Non-copyable, movable.
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&) noexcept;
    DiskManager& operator=(DiskManager&&) noexcept;

    // Allocate a new page: zero-fill and append to file. Returns new page_id. (DISK-02)
    uint32_t allocatePage();

    // Read page `page_id` from disk into `page.rawBytes()`. (DISK-01, DISK-04)
    void readPage(uint32_t page_id, Page& page);

    // Write `page.rawBytes()` to disk at page `page_id`. (DISK-01, DISK-04)
    void writePage(uint32_t page_id, const Page& page);

    // Call fdatasync on the file descriptor. (DISK-03)
    void flush();

    // Returns the number of allocated pages (file size / PAGE_SIZE).
    uint32_t pageCount() const { return next_page_id_; }

private:
    int      fd_{-1};
    uint32_t next_page_id_{0};
    std::string path_;
};

} // namespace adapttree
