#pragma once
#include "disk_manager.hpp"
#include <unordered_map>
#include <list>
#include <vector>
#include <mutex>
#include <cstddef>

struct Frame {
    Page      page;
    page_id_t page_id   = INVALID_PAGE_ID;
    int       pin_count = 0;
    bool      dirty     = false;
};

class BufferPoolManager {
public:
    explicit BufferPoolManager(DiskManager& dm, std::size_t pool_size = 32768)
        : dm_(dm), pool_size_(pool_size) {
        frames_.resize(pool_size_);
        for (std::size_t i = 0; i < pool_size_; ++i) free_list_.push_back(i);
    }
    Frame* pin(page_id_t pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = page_table_.find(pid);
        if (it != page_table_.end()) {
            frames_[it->second].pin_count++;
            lru_move_front(it->second);
            return &frames_[it->second];
        }
        std::size_t fid = get_frame();
        if (fid == SIZE_MAX) return nullptr;
        Frame& f = frames_[fid];
        f.page_id = pid; f.pin_count = 1; f.dirty = false;
        if (!dm_.read_page(pid, f.page)) { free_list_.push_back(fid); return nullptr; }
        page_table_[pid] = fid;
        lru_insert_front(fid);
        return &f;
    }
    Frame* new_page(page_id_t& pid_out) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t fid = get_frame();
        if (fid == SIZE_MAX) return nullptr;
        Frame& f = frames_[fid];
        pid_out = dm_.allocate_page(f.page);
        f.page_id = pid_out; f.pin_count = 1; f.dirty = true;
        page_table_[pid_out] = fid;
        lru_insert_front(fid);
        return &f;
    }
    void unpin(page_id_t pid, bool dirty) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = page_table_.find(pid);
        if (it == page_table_.end()) return;
        Frame& f = frames_[it->second];
        if (f.pin_count > 0) f.pin_count--;
        if (dirty) f.dirty = true;
    }
    void flush_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [pid, fid] : page_table_) {
            if (frames_[fid].dirty) { dm_.write_page(pid, frames_[fid].page); frames_[fid].dirty = false; }
        }
    }
    std::size_t pool_size() const { return pool_size_; }
private:
    std::size_t get_frame() {
        if (!free_list_.empty()) {
            std::size_t fid = free_list_.front(); free_list_.pop_front(); return fid;
        }
        return evict();
    }
    std::size_t evict() {
        for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
            std::size_t fid = *it;
            if (frames_[fid].pin_count == 0) {
                if (frames_[fid].dirty) { dm_.write_page(frames_[fid].page_id, frames_[fid].page); frames_[fid].dirty = false; }
                page_table_.erase(frames_[fid].page_id);
                frames_[fid].page_id = INVALID_PAGE_ID;
                lru_list_.erase(std::next(it).base());
                lru_pos_.erase(fid);
                return fid;
            }
        }
        return SIZE_MAX;
    }
    void lru_insert_front(std::size_t fid) {
        lru_list_.push_front(fid);
        lru_pos_[fid] = lru_list_.begin();
    }
    void lru_move_front(std::size_t fid) {
        auto it = lru_pos_.find(fid);
        if (it != lru_pos_.end()) lru_list_.erase(it->second);
        lru_list_.push_front(fid);
        lru_pos_[fid] = lru_list_.begin();
    }
    DiskManager& dm_;
    std::size_t pool_size_;
    std::vector<Frame> frames_;
    std::unordered_map<page_id_t, std::size_t> page_table_;
    std::unordered_map<std::size_t, std::list<std::size_t>::iterator> lru_pos_;
    std::list<std::size_t> lru_list_;
    std::list<std::size_t> free_list_;
    std::mutex mutex_;
};
