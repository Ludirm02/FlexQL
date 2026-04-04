#pragma once
#include <string>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>

static constexpr std::size_t PAGE_SIZE = 8192;
using page_id_t = std::int64_t;
static constexpr page_id_t INVALID_PAGE_ID = -1;

struct PageHeader {
    page_id_t page_id   = INVALID_PAGE_ID;
    uint16_t  row_count = 0;
    uint16_t  free_off  = sizeof(PageHeader);
    uint32_t  flags     = 0;
};
static constexpr std::size_t PAGE_HEADER_SIZE = sizeof(PageHeader);
static constexpr std::size_t PAGE_DATA_SIZE   = PAGE_SIZE - PAGE_HEADER_SIZE;

struct Page {
    alignas(4096) char data[PAGE_SIZE]{};
    PageHeader* header() { return reinterpret_cast<PageHeader*>(data); }
    const PageHeader* header() const { return reinterpret_cast<const PageHeader*>(data); }
    char* payload() { return data + PAGE_HEADER_SIZE; }
    const char* payload() const { return data + PAGE_HEADER_SIZE; }
    uint16_t free_space() const { return static_cast<uint16_t>(PAGE_SIZE - header()->free_off); }
    int append(const char* src, uint16_t len) {
        if (free_space() < static_cast<uint16_t>(len + sizeof(uint16_t))) return -1;
        uint16_t off = header()->free_off;
        std::memcpy(data + off, &len, sizeof(len));
        off += sizeof(len);
        std::memcpy(data + off, src, len);
        header()->free_off = off + len;
        header()->row_count++;
        return static_cast<int>(off - sizeof(len));
    }
    void reset(page_id_t pid) {
        std::memset(data, 0, PAGE_SIZE);
        header()->page_id  = pid;
        header()->free_off = static_cast<uint16_t>(PAGE_HEADER_SIZE);
        header()->row_count = 0;
    }
};

class DiskManager {
public:
    explicit DiskManager(const std::string& table_name) : table_name_(table_name) {
        std::filesystem::create_directories("data/pages");
        path_ = "data/pages/" + table_name + ".db";
        // Use raw fd + pread/pwrite: bypasses C++ stream buffering, fully thread-safe
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ >= 0) {
            off_t sz = ::lseek(fd_, 0, SEEK_END);
            if (sz > 0) {
                num_pages_.store(
                    static_cast<page_id_t>(sz / static_cast<off_t>(PAGE_SIZE)),
                    std::memory_order_release);
            }
        }
    }
    ~DiskManager() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // pread: position-independent, thread-safe, no seekp/seekg races
    bool read_page(page_id_t pid, Page& page) {
        if (pid < 0 || pid >= num_pages_.load(std::memory_order_acquire)) return false;
        off_t off = static_cast<off_t>(pid) * static_cast<off_t>(PAGE_SIZE);
        ssize_t n = ::pread(fd_, page.data, PAGE_SIZE, off);
        return n == static_cast<ssize_t>(PAGE_SIZE);
    }

    // pwrite: position-independent, goes straight to OS page cache (no flush needed)
    // NO fdatasync — only at checkpoint/shutdown
    bool write_page(page_id_t pid, const Page& page) {
        if (pid < 0) return false;
        off_t off = static_cast<off_t>(pid) * static_cast<off_t>(PAGE_SIZE);
        ssize_t n = ::pwrite(fd_, page.data, PAGE_SIZE, off);
        return n == static_cast<ssize_t>(PAGE_SIZE);
    }

    // Atomically extend the file by one blank page
    page_id_t allocate_page(Page& page) {
        std::lock_guard<std::mutex> lock(alloc_mutex_);
        page_id_t pid = num_pages_.fetch_add(1, std::memory_order_acq_rel);
        page.reset(pid);
        off_t off = static_cast<off_t>(pid) * static_cast<off_t>(PAGE_SIZE);
        [[maybe_unused]] ssize_t n2 = ::pwrite(fd_, page.data, PAGE_SIZE, off);
        return pid;
    }

    // Flush OS page cache to physical disk — call at checkpoint/shutdown ONLY
    void fsync_data() {
        if (fd_ >= 0) ::fdatasync(fd_);
    }

    page_id_t num_pages() const { return num_pages_.load(std::memory_order_acquire); }
    const std::string& table_name() const { return table_name_; }

private:
    std::string             table_name_;
    std::string             path_;
    int                     fd_ = -1;
    std::atomic<page_id_t>  num_pages_{0};
    std::mutex              alloc_mutex_;  // serializes fetch_add + pwrite in allocate_page
};
