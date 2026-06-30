#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <system_error>

// ── DISK-04: EINTR retry logic ────────────────────────────────────────────────

static int eintr_call_count = 0;

static ssize_t mock_pwrite_eintr_once(int, const void*, size_t count, off_t) {
    ++eintr_call_count;
    if (eintr_call_count == 1) {
        errno = EINTR;
        return -1;
    }
    return static_cast<ssize_t>(count);
}

template <typename WriteFn>
static void retry_write(WriteFn fn, int fd, const void* buf, size_t count, off_t offset) {
    const auto* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = count;
    off_t  off       = offset;
    while (remaining > 0) {
        ssize_t n = fn(fd, ptr, remaining, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "write failed");
        }
        ptr       += n;
        off       += n;
        remaining -= static_cast<size_t>(n);
    }
}

static ssize_t mock_pread_eintr_once(int, void* buf, size_t count, off_t) {
    ++eintr_call_count;
    if (eintr_call_count == 1) {
        errno = EINTR;
        return -1;
    }
    std::memset(buf, 0xAB, count);
    return static_cast<ssize_t>(count);
}

template <typename ReadFn>
static void retry_read(ReadFn fn, int fd, void* buf, size_t count, off_t offset) {
    auto* ptr        = static_cast<uint8_t*>(buf);
    size_t remaining = count;
    off_t  off       = offset;
    while (remaining > 0) {
        ssize_t n = fn(fd, ptr, remaining, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "read failed");
        }
        if (n == 0) throw std::runtime_error("unexpected EOF");
        ptr       += n;
        off       += n;
        remaining -= static_cast<size_t>(n);
    }
}

TEST(EintrRetry, WriteRetriesOnEintrAndSucceeds) {
    eintr_call_count = 0;
    uint8_t buf[16] = {};
    EXPECT_NO_THROW(retry_write(mock_pwrite_eintr_once, 0, buf, 16, 0));
    EXPECT_EQ(eintr_call_count, 2);
}

TEST(EintrRetry, ReadRetriesOnEintrAndSucceeds) {
    eintr_call_count = 0;
    uint8_t buf[16] = {};
    EXPECT_NO_THROW(retry_read(mock_pread_eintr_once, 0, buf, 16, 0));
    EXPECT_EQ(eintr_call_count, 2);
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], 0xABu) << "at index " << i;
    }
}

static ssize_t mock_pwrite_permanent_error(int, const void*, size_t, off_t) {
    errno = EIO;
    return -1;
}

TEST(EintrRetry, WriteThrowsOnPermanentError) {
    uint8_t buf[16] = {};
    EXPECT_THROW(retry_write(mock_pwrite_permanent_error, 0, buf, 16, 0),
                 std::system_error);
}

static ssize_t mock_pread_permanent_error(int, void*, size_t, off_t) {
    errno = EIO;
    return -1;
}

TEST(EintrRetry, ReadThrowsOnPermanentError) {
    uint8_t buf[16] = {};
    EXPECT_THROW(retry_read(mock_pread_permanent_error, 0, buf, 16, 0),
                 std::system_error);
}
