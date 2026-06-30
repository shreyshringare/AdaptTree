#include "adapttree/disk_manager.hpp"
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace adapttree {

namespace {

// pwrite_all: wraps pwrite, looping on EINTR and short writes. (DISK-04)
void pwrite_all(int fd, const void* buf, size_t count, off_t offset) {
    const auto* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = count;
    off_t  off       = offset;
    while (remaining > 0) {
        ssize_t n = ::pwrite(fd, ptr, remaining, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "pwrite");
        }
        ptr       += n;
        off       += n;
        remaining -= static_cast<size_t>(n);
    }
}

// pread_all: wraps pread, looping on EINTR and short reads. (DISK-04)
void pread_all(int fd, void* buf, size_t count, off_t offset) {
    auto* ptr        = static_cast<uint8_t*>(buf);
    size_t remaining = count;
    off_t  off       = offset;
    while (remaining > 0) {
        ssize_t n = ::pread(fd, ptr, remaining, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "pread");
        }
        if (n == 0) {
            throw std::runtime_error("pread: unexpected EOF");
        }
        ptr       += n;
        off       += n;
        remaining -= static_cast<size_t>(n);
    }
}

} // anonymous namespace

DiskManager::DiskManager(std::string_view path)
    : path_(path)
{
    fd_ = ::open(std::string(path).c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager: cannot open " + std::string(path));
    }

    struct stat st{};
    if (::fstat(fd_, &st) < 0) {
        throw std::system_error(errno, std::generic_category(), "fstat");
    }
    next_page_id_ = static_cast<uint32_t>(st.st_size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

DiskManager::DiskManager(DiskManager&& other) noexcept
    : fd_(other.fd_), next_page_id_(other.next_page_id_), path_(std::move(other.path_))
{
    other.fd_ = -1;
}

DiskManager& DiskManager::operator=(DiskManager&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_           = other.fd_;
        next_page_id_ = other.next_page_id_;
        path_         = std::move(other.path_);
        other.fd_     = -1;
    }
    return *this;
}

uint32_t DiskManager::allocatePage() {
    uint32_t id     = next_page_id_++;
    off_t    offset = static_cast<off_t>(id) * PAGE_SIZE;
    static const uint8_t zeros[PAGE_SIZE] = {};
    pwrite_all(fd_, zeros, PAGE_SIZE, offset);
    return id;
}

void DiskManager::readPage(uint32_t page_id, Page& page) {
    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
    pread_all(fd_, page.rawBytes(), PAGE_SIZE, offset);
}

void DiskManager::writePage(uint32_t page_id, const Page& page) {
    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
    pwrite_all(fd_, page.rawBytes(), PAGE_SIZE, offset);
}

void DiskManager::flush() {
    if (::fdatasync(fd_) < 0) {
        throw std::system_error(errno, std::generic_category(), "fdatasync");
    }
}

} // namespace adapttree
