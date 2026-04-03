#pragma once
#include "buffer_pool.hpp"
#include <string>
#include <vector>
#include <cstring>

struct PageRow {
    std::vector<std::string> values;
    std::int64_t expires_at = 0;
};

inline std::string serialize_row(const std::vector<std::string>& values, std::int64_t expires_at) {
    std::string buf;
    buf.append(reinterpret_cast<const char*>(&expires_at), sizeof(expires_at));
    uint16_t ncols = static_cast<uint16_t>(values.size());
    buf.append(reinterpret_cast<const char*>(&ncols), sizeof(ncols));
    for (const auto& v : values) {
        uint16_t vlen = static_cast<uint16_t>(v.size());
        buf.append(reinterpret_cast<const char*>(&vlen), sizeof(vlen));
        buf.append(v.data(), v.size());
    }
    return buf;
}

inline bool deserialize_row(const char* data, std::size_t data_len, std::size_t& offset, PageRow& out) {
    if (offset + sizeof(std::int64_t) + sizeof(uint16_t) > data_len) return false;
    std::memcpy(&out.expires_at, data + offset, sizeof(out.expires_at));
    offset += sizeof(out.expires_at);
    uint16_t ncols = 0;
    std::memcpy(&ncols, data + offset, sizeof(ncols));
    offset += sizeof(ncols);
    out.values.resize(ncols);
    for (uint16_t i = 0; i < ncols; ++i) {
        if (offset + sizeof(uint16_t) > data_len) return false;
        uint16_t vlen = 0;
        std::memcpy(&vlen, data + offset, sizeof(vlen));
        offset += sizeof(vlen);
        if (offset + vlen > data_len) return false;
        out.values[i].assign(data + offset, vlen);
        offset += vlen;
    }
    return true;
}

class TableIterator {
public:
    TableIterator(BufferPoolManager& bpm, page_id_t num_pages)
        : bpm_(bpm), num_pages_(num_pages), cur_page_(0), cur_frame_(nullptr), row_idx_(0) {
        advance_page();
    }
    ~TableIterator() { if (cur_frame_) bpm_.unpin(cur_frame_->page_id, false); }
    bool valid() const { return cur_frame_ != nullptr && row_idx_ < rows_in_page_.size(); }
    const PageRow& current() const { return rows_in_page_[row_idx_]; }
    void next() {
        ++row_idx_;
        if (row_idx_ >= rows_in_page_.size()) {
            bpm_.unpin(cur_frame_->page_id, false);
            cur_frame_ = nullptr;
            ++cur_page_;
            advance_page();
        }
    }
    page_id_t current_page() const { return cur_frame_ ? cur_frame_->page_id : INVALID_PAGE_ID; }
    uint16_t current_slot() const { return static_cast<uint16_t>(row_idx_); }
private:
    void advance_page() {
        rows_in_page_.clear(); row_idx_ = 0;
        while (cur_page_ < num_pages_) {
            cur_frame_ = bpm_.pin(cur_page_);
            if (!cur_frame_) { ++cur_page_; continue; }
            const PageHeader* hdr = cur_frame_->page.header();
            if (hdr->row_count == 0) { bpm_.unpin(cur_page_, false); cur_frame_ = nullptr; ++cur_page_; continue; }
            const char* pdata = cur_frame_->page.data;
            std::size_t off = PAGE_HEADER_SIZE;
            for (uint16_t i = 0; i < hdr->row_count; ++i) {
                if (off + sizeof(uint16_t) > PAGE_SIZE) break;
                uint16_t slot_len = 0;
                std::memcpy(&slot_len, pdata + off, sizeof(slot_len));
                off += sizeof(slot_len);
                if (off + slot_len > PAGE_SIZE) break;
                PageRow row; std::size_t inner = 0;
                if (deserialize_row(pdata + off, slot_len, inner, row))
                    rows_in_page_.push_back(std::move(row));
                off += slot_len;
            }
            if (!rows_in_page_.empty()) return;
            bpm_.unpin(cur_page_, false); cur_frame_ = nullptr; ++cur_page_;
        }
        cur_frame_ = nullptr;
    }
    BufferPoolManager& bpm_;
    page_id_t num_pages_, cur_page_;
    Frame* cur_frame_;
    std::vector<PageRow> rows_in_page_;
    std::size_t row_idx_;
};
