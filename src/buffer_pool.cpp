#include "adapttree/buffer_pool.hpp"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace adapttree {

DiskManager::DiskManager(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager(bp): open failed: " + path);
    struct stat st{};
    if (::fstat(fd_, &st) < 0)
        throw std::system_error(errno, std::generic_category(), "fstat");
    next_ = static_cast<size_t>(st.st_size) / PAGE_SIZE;
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) { ::fdatasync(fd_); ::close(fd_); fd_ = -1; }
}

page_id_t DiskManager::allocatePage() {
    page_id_t pid    = static_cast<page_id_t>(next_++);
    off_t     offset = static_cast<off_t>(pid) * static_cast<off_t>(PAGE_SIZE);
    static const std::byte zeros[PAGE_SIZE]{};
    const std::byte* src = zeros;
    size_t rem = PAGE_SIZE;
    while (rem > 0) {
        ssize_t n = ::pwrite(fd_, src, rem, offset);
        if (n < 0) { if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "pwrite allocatePage"); }
        src += n; offset += n; rem -= static_cast<size_t>(n);
    }
    return pid;
}

bool DiskManager::readPage(page_id_t page_id, std::byte* buf) {
    off_t  offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
    size_t rem    = PAGE_SIZE;
    while (rem > 0) {
        ssize_t n = ::pread(fd_, buf, rem, offset);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        buf += n; offset += n; rem -= static_cast<size_t>(n);
    }
    return true;
}

bool DiskManager::writePage(page_id_t page_id, const std::byte* buf) {
    off_t  offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
    size_t rem    = PAGE_SIZE;
    while (rem > 0) {
        ssize_t n = ::pwrite(fd_, buf, rem, offset);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        buf += n; offset += n; rem -= static_cast<size_t>(n);
    }
    return true;
}

void DiskManager::flush() { ::fdatasync(fd_); }

}  // namespace adapttree
