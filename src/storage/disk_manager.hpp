#pragma once
#include <string>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <cstring>

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
        if (!std::filesystem::exists(path_)) { std::ofstream f(path_, std::ios::binary); }
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (file_.is_open()) {
            file_.seekg(0, std::ios::end);
            auto sz = file_.tellg();
            num_pages_ = static_cast<page_id_t>(sz / PAGE_SIZE);
        }
    }
    ~DiskManager() { if (file_.is_open()) file_.close(); }
    bool read_page(page_id_t pid, Page& page) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pid < 0 || pid >= num_pages_) return false;
        file_.seekg(pid * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
        file_.read(page.data, PAGE_SIZE);
        return file_.good();
    }
    bool write_page(page_id_t pid, const Page& page) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.seekp(pid * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
        file_.write(page.data, PAGE_SIZE);
        file_.flush();
        return file_.good();
    }
    page_id_t allocate_page(Page& page) {
        std::lock_guard<std::mutex> lock(mutex_);
        page_id_t pid = num_pages_++;
        page.reset(pid);
        file_.seekp(0, std::ios::end);
        file_.write(page.data, PAGE_SIZE);
        file_.flush();
        return pid;
    }
    page_id_t num_pages() const { return num_pages_; }
    const std::string& table_name() const { return table_name_; }
private:
    std::string table_name_;
    std::string path_;
    std::fstream file_;
    std::mutex mutex_;
    page_id_t num_pages_ = 0;
};
