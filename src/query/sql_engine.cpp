#include "../storage/table_iterator.hpp"
#include <thread>
#include "storage/disk_store.hpp"
#include "sql_engine.hpp"

#pragma GCC optimize("O3,unroll-loops")
#ifdef __AVX2__
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif

#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flexql {

namespace {

constexpr const char* kCreateTableKw = "CREATE TABLE";
constexpr const char* kInsertIntoKw = "INSERT INTO";
constexpr const char* kSelectKw = "SELECT";

inline bool is_space_char(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

void trim_in_place(std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && is_space_char(s[b])) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && is_space_char(s[e - 1])) {
        --e;
    }
    if (b == 0 && e == s.size()) {
        return;
    }
    if (b >= e) {
        s.clear();
        return;
    }
    s.erase(e);
    s.erase(0, b);
}

std::size_t estimate_query_result_bytes(const QueryResult& result) {
    std::size_t bytes = sizeof(QueryResult);
    bytes += result.columns.size() * sizeof(std::string);
    for (const std::string& c : result.columns) {
        bytes += c.size();
    }
    bytes += result.rows.size() * sizeof(std::vector<std::string>);
    for (const auto& row : result.rows) {
        bytes += row.size() * sizeof(std::string);
        for (const std::string& cell : row) {
            bytes += cell.size();
        }
    }
    return bytes;
}

bool starts_with_ci(const char* s, const char* kw) {
    while (*kw != '\0') {
        if (*s == '\0') {
            return false;
        }
        if (std::toupper(static_cast<unsigned char>(*s)) !=
            std::toupper(static_cast<unsigned char>(*kw))) {
            return false;
        }
        ++s;
        ++kw;
    }
    return true;
}

const char* find_keyword_ci(const char* start, const char* kw) {
    const std::size_t kw_len = std::strlen(kw);
    if (kw_len == 0) {
        return start;
    }

    for (const char* p = start; *p != '\0'; ++p) {
        if (!starts_with_ci(p, kw)) {
            continue;
        }
        const bool left_ok = (p == start) || is_space_char(*(p - 1)) || *(p - 1) == ')';
        const char right = p[kw_len];
        const bool right_ok = (right == '\0') || is_space_char(right) || right == '(';
        if (left_ok && right_ok) {
            return p;
        }
    }
    return nullptr;
}

void append_escaped_field_wire(std::string& out, const std::string& value) {
    bool needs_escape = false;
    for (char c : value) {
        if (c == '\\' || c == '\t' || c == '\n') {
            needs_escape = true;
            break;
        }
    }
    if (!needs_escape) {
        out += value;
        return;
    }
    for (char c : value) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
}

void append_line_with_fields_wire(std::string& out,
                                  const char* prefix,
                                  const std::vector<std::string>& fields) {
    out += prefix;
    for (const std::string& f : fields) {
        out.push_back('\t');
        append_escaped_field_wire(out, f);
    }
    out.push_back('\n');
}

std::string build_wire_bytes(const QueryResult& result) {
    std::string wire;
    wire.reserve(64 + result.rows.size() * 32);
    wire += "OK " + std::to_string(result.columns.size()) + "\n";
    if (result.columns.empty()) {
        wire += "END\n";
        return wire;
    }
    append_line_with_fields_wire(wire, "COL", result.columns);
    for (const auto& row : result.rows) {
        append_line_with_fields_wire(wire, "ROW", row);
    }
    wire += "END\n";
    return wire;
}

inline void append_u16_be(std::string& out, std::uint16_t v) {
    const std::uint16_t be = htons(v);
    out.append(reinterpret_cast<const char*>(&be), sizeof(be));
}

inline void append_u32_be(std::string& out, std::uint32_t v) {
    const std::uint32_t be = htonl(v);
    out.append(reinterpret_cast<const char*>(&be), sizeof(be));
}

std::string build_wire_bytes_binary(const QueryResult& result) {
    std::string wire;
    wire.reserve(1 + 4 + 4 + result.columns.size() * 8 + result.rows.size() * 16);
    wire.push_back(static_cast<char>(0x01));  // OK frame
    append_u32_be(wire, static_cast<std::uint32_t>(result.columns.size()));
    append_u32_be(wire, static_cast<std::uint32_t>(result.rows.size()));
    for (const std::string& c : result.columns) {
        append_u16_be(wire, static_cast<std::uint16_t>(std::min<std::size_t>(c.size(), 0xFFFF)));
        wire.append(c.data(), std::min<std::size_t>(c.size(), 0xFFFF));
    }
    for (const auto& row : result.rows) {
        for (const std::string& v : row) {
            append_u32_be(wire, static_cast<std::uint32_t>(v.size()));
            wire.append(v.data(), v.size());
        }
    }
    return wire;
}

}  // namespace

SqlEngine::QueryCache::QueryCache(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity),
      max_bytes_(512ULL * 1024 * 1024) {}

bool SqlEngine::QueryCache::get(const std::string& key,
                                const std::unordered_map<std::string, std::uint64_t>& current_versions,
                                QueryResult* out,
                                std::string* wire_out,
                                bool binary_wire) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }

    CacheEntry& entry = it->second->second;
    if (entry.versions != current_versions) {
        current_bytes_ -= entry.approx_bytes;
        entries_.erase(it->second);
        map_.erase(it);
        return false;
    }

    if (out != nullptr) {
        *out = entry.result;
    }
    if (wire_out != nullptr) {
        *wire_out = binary_wire ? entry.wire_bytes_bin : entry.wire_bytes;
    }
    entries_.splice(entries_.begin(), entries_, it->second);
    return true;
}

void SqlEngine::QueryCache::put(const std::string& key,
                                const QueryResult& result,
                                const std::unordered_map<std::string, std::uint64_t>& versions,
                                bool binary_wire) {
    std::string wire_bytes;
    std::string wire_bytes_bin;
    if (binary_wire) {
        wire_bytes_bin = build_wire_bytes_binary(result);
    } else {
        wire_bytes = build_wire_bytes(result);
    }
    const std::size_t approx_bytes = estimate_query_result_bytes(result) +
                                     wire_bytes.size() + wire_bytes_bin.size();
    if (approx_bytes > 256ULL * 1024 * 1024) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        current_bytes_ -= it->second->second.approx_bytes;
        it->second->second.result = result;
        it->second->second.wire_bytes = std::move(wire_bytes);
        it->second->second.wire_bytes_bin = std::move(wire_bytes_bin);
        it->second->second.versions = versions;
        it->second->second.approx_bytes = approx_bytes;
        current_bytes_ += approx_bytes;
        entries_.splice(entries_.begin(), entries_, it->second);
    } else {
        entries_.push_front(
            {key, CacheEntry{result, std::move(wire_bytes), std::move(wire_bytes_bin), versions, approx_bytes}});
        map_[key] = entries_.begin();
        current_bytes_ += approx_bytes;
    }

    while (!entries_.empty() && (entries_.size() > capacity_ || current_bytes_ > max_bytes_)) {
        auto tail = std::prev(entries_.end());
        current_bytes_ -= tail->second.approx_bytes;
        map_.erase(tail->first);
        entries_.pop_back();
    }
}

SqlEngine::SqlEngine(std::size_t cache_capacity) : cache_(cache_capacity) {}

bool SqlEngine::execute(const std::string& sql, QueryResult& out, std::string& error) {
    return execute(sql, out, error, nullptr, false);
}

bool SqlEngine::execute(const std::string& sql,
                        QueryResult& out,
                        std::string& error,
                        std::string* cached_wire_out,
                        bool binary_wire) {
    out = QueryResult{};
    error.clear();
    if (cached_wire_out != nullptr) {
        cached_wire_out->clear();
    }

    // Fast path: avoid full string copy for INSERT batches
    // Just check first non-space char and strip trailing semicolon in-place
    const char* p = sql.c_str();
    while (*p && is_space_char(*p)) ++p;
    if (!*p) { error = "empty SQL statement"; return false; }

    std::string normalized(p);
    // Strip trailing whitespace and semicolon
    while (!normalized.empty() && (is_space_char(normalized.back()) || normalized.back() == ';')) {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        error = "empty SQL statement";
        return false;
    }

    // Only uppercase first 20 chars to detect command type
    const std::size_t cmd_scan = std::min<std::size_t>(normalized.size(), 20);
    std::string upper = to_upper(normalized.substr(0, cmd_scan));
    if (starts_with_keyword(upper, kCreateTableKw)) {
        std::unique_lock<std::shared_mutex> lock(db_mutex_);
        return execute_create_table(normalized, error);
    }
    if (starts_with_keyword(upper, kInsertIntoKw)) {
        return execute_insert(normalized, error);
    }
    if (starts_with_keyword(upper, kSelectKw)) {
        return execute_select(normalized, out, error, cached_wire_out, binary_wire);
    }

    constexpr const char* kDeleteFromKw = "DELETE FROM";
    {
        std::string full_upper = to_upper(normalized);
        if (starts_with_keyword(full_upper, kDeleteFromKw)) {
            std::unique_lock<std::shared_mutex> lock(db_mutex_);
            return execute_delete(normalized, error);
        }
    }

    error = "unsupported SQL command";
    return false;
}

bool SqlEngine::execute_create_table(const std::string& sql, std::string& error) {
    std::string table_name;
    std::vector<Column> columns;
    int primary_col = -1;
    if (!parse_create_table(sql, table_name, columns, primary_col, error)) {
        return false;
    }

    if (tables_.count(table_name) != 0U) {
        // Clear existing data so re-creation acts as reset
        Table& existing = tables_[table_name];
        existing.primary_index.clear();
        existing.pk_robin_index = RobinHoodIndex(1024);
        ++existing.version;
        if (existing.buf_pool) { existing.buf_pool->flush_all(); }
        std::filesystem::remove("data/pages/" + table_name + ".db");
        if (existing.disk_mgr) {
            existing.disk_mgr = std::make_unique<DiskManager>(table_name);
            existing.buf_pool = std::make_unique<BufferPoolManager>(*existing.disk_mgr);
            existing.last_page_id = INVALID_PAGE_ID;
        }
        return true;
    }

    auto [table_it, inserted] = tables_.try_emplace(table_name);
    if (!inserted) {
        error = "table already exists: " + table_name;
        return false;
    }
    Table& t = table_it->second;
    t.name = table_name;
    t.columns = columns;
    t.primary_key_col = primary_col;
    t.disk_mgr = std::make_unique<DiskManager>(table_name);
    t.buf_pool = std::make_unique<BufferPoolManager>(*t.disk_mgr, 32768);

    if (primary_col >= 0) {
        if (columns[static_cast<std::size_t>(primary_col)].type == DataType::kInt) {
            t.pk_is_int = true;
            t.pk_robin_index.reserve(1024);
        } else {
            t.primary_index.reserve(1024);
        }
    }

    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        t.column_index[t.columns[i].name] = i;
    }

    DiskStore::write_schema(table_name, sql);
    return true;
}

bool SqlEngine::execute_insert(const std::string& sql, std::string& error) {
    std::string table_name;
    std::vector<std::vector<std::string>> rows_values;
    std::vector<std::int64_t> expires_at;
    if (!parse_insert(sql, table_name, rows_values, expires_at, error)) {
        return false;
    }

    std::shared_lock<std::shared_mutex> map_lock(db_mutex_);
    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        error = "unknown table: " + table_name;
        return false;
    }

    Table& table = it->second;
    std::unique_lock<std::shared_mutex> table_lock(table.mutex);
    if (rows_values.empty()) {
        error = "INSERT must contain at least one VALUES tuple";
        return false;
    }

    Frame* current_frame = nullptr;
    page_id_t current_pid = INVALID_PAGE_ID;

    for (std::size_t r = 0; r < rows_values.size(); ++r) {
        std::vector<std::string>& row_vals = rows_values[r];
        std::int64_t expires = expires_at[r];

        if (row_vals.size() != table.columns.size()) {
            if (current_frame) table.buf_pool->unpin(current_pid, true);
            error = "column count mismatch in INSERT";
            return false;
        }

        for (std::size_t i = 0; i < row_vals.size(); ++i) {
            double parsed = 0.0;
            bool numeric_valid = false;
            if (!validate_typed_value(table.columns[i], row_vals[i], error, &parsed, &numeric_valid)) {
                if (current_frame) table.buf_pool->unpin(current_pid, true);
                return false;
            }
        }

        std::int64_t pk_int_val = 0;
        std::string pk_key;
        if (table.primary_key_col >= 0) {
            pk_key = row_vals[static_cast<std::size_t>(table.primary_key_col)];
            if (table.pk_is_int) {
                fast_parse_int64(pk_key, pk_int_val);
                auto existing = table.pk_robin_index.lookup(pk_int_val);
                if (existing != RobinHoodIndex::kEmpty) {
                    if (current_frame) table.buf_pool->unpin(current_pid, true);
                    error = "duplicate primary key value: " + pk_key;
                    return false;
                }
            } else {
                auto idx_it = table.primary_index.find(pk_key);
                if (idx_it != table.primary_index.end()) {
                    if (current_frame) table.buf_pool->unpin(current_pid, true);
                    error = "duplicate primary key value: " + pk_key;
                    return false;
                }
            }
        }

        if (table.buf_pool && table.disk_mgr) {
            std::string serialized = serialize_row(row_vals, expires);
            int slot = -1;
            if (current_frame == nullptr && table.last_page_id != INVALID_PAGE_ID) {
                current_pid = table.last_page_id;
                current_frame = table.buf_pool->pin(current_pid);
            }
            if (current_frame) {
                slot = current_frame->page.append(serialized.data(), static_cast<uint16_t>(serialized.size()));
            }
            if (slot < 0) {
                if (current_frame) table.buf_pool->unpin(current_pid, true);
                page_id_t new_pid;
                current_frame = table.buf_pool->new_page(new_pid);
                if (current_frame) {
                    current_pid = new_pid;
                    slot = current_frame->page.append(serialized.data(), static_cast<uint16_t>(serialized.size()));
                    table.last_page_id = new_pid;
                } else {
                    error = "failed to allocate new page in buffer pool";
                    return false;
                }
            }

            if (table.primary_key_col >= 0) {
                uint64_t loc = (static_cast<uint64_t>(current_pid) << 16) | static_cast<uint16_t>(slot);
                if (table.pk_is_int) {
                    table.pk_robin_index.insert(pk_int_val, loc);
                } else {
                    table.primary_index[pk_key] = loc;
                }
            }
        }
    }
    if (current_frame) table.buf_pool->unpin(current_pid, true);

    // NOTE: We do NOT flush or fsync here. The buffer pool's LRU
    // eviction writes dirty pages to disk automatically when frames
    // are needed. A full checkpoint (flush+fsync per table) happens
    // only at clean shutdown via checkpoint_to_disk(). This design
    // maximises insert throughput while keeping data on disk via
    // the OS page cache (survives most crashes).
    ++table.version;
    return true;
}


static PageRow fetch_row_from_buf(BufferPoolManager* bpm, uint64_t loc) {
    page_id_t pid = loc >> 16;
    uint16_t slot = loc & 0xFFFF;
    Frame* f = bpm->pin(pid);
    PageRow pr;
    if (f) {
        const char* pdata = f->page.data;
        uint16_t slot_len = 0;
        std::memcpy(&slot_len, pdata + slot, sizeof(slot_len));
        std::size_t inner = 0;
        deserialize_row(pdata + slot + sizeof(slot_len), slot_len, inner, pr);
        bpm->unpin(pid, false);
    }
    return pr;
}

bool SqlEngine::execute_select(const std::string& sql,
                               QueryResult& out,
                               std::string& error,
                               std::string* cached_wire_out,
                               bool binary_wire) {
    SelectPlan plan;
    if (!parse_select(sql, plan, error)) return false;

    const std::int64_t now_ts = now_unix();

    std::shared_lock<std::shared_mutex> lock(db_mutex_);
    auto base_it = tables_.find(plan.base_table);
    if (base_it == tables_.end()) { error = "unknown table: " + plan.base_table; return false; }
    const Table& base = base_it->second;

    bool skip_cache = false;
    if (!plan.has_join && plan.where.present && plan.where.op == "=" && base.primary_key_col >= 0) {
        skip_cache = true;
    }

    std::string cache_key;
    std::unordered_map<std::string, std::uint64_t> versions;
    if (!skip_cache) {
        cache_key = normalize_sql_for_cache(sql);
        versions = capture_versions(plan.touched_tables);
        QueryResult* cache_out = &out;
        if (cache_.get(cache_key, versions, cache_out, cached_wire_out, binary_wire)) return true;
    }

    struct Projection { const Table* table; std::size_t index; std::string output_name; };
    std::vector<Projection> projections;

    const Table* right_ptr = nullptr;
    if (plan.has_join) {
        auto r_it = tables_.find(plan.join_table);
        if (r_it == tables_.end()) { error = "unknown join table: " + plan.join_table; return false; }
        right_ptr = &r_it->second;
    }
    
    if (plan.select_star) {
        for (std::size_t i = 0; i < base.columns.size(); ++i) {
            // Use qualified name for JOINs to disambiguate, plain name otherwise
            const std::string col_out = plan.has_join
                ? (base.name + "." + base.columns[i].name)
                : base.columns[i].name;
            projections.push_back({&base, i, col_out});
        }
        if (right_ptr) {
            for (std::size_t i = 0; i < right_ptr->columns.size(); ++i) {
                projections.push_back({right_ptr, i, right_ptr->name + "." + right_ptr->columns[i].name});
            }
        }
    } else {
        for (const std::string& ref : plan.select_columns) {
            std::string tbl, col; split_qualified(ref, tbl, col);
            std::size_t base_idx, right_idx; std::string err1, err2;
            bool in_base = lookup_column(base, col.empty() ? ref : col, base_idx, err1) != nullptr;
            bool in_right = right_ptr && lookup_column(*right_ptr, col.empty() ? ref : col, right_idx, err2) != nullptr;
            if (in_base && in_right) { error = "ambiguous column"; return false; }
            if (!in_base && !in_right) { error = "unknown column"; return false; }
            if (in_base) {
                // Use qualified name in JOINs, plain name for single-table queries
                const std::string out_name = plan.has_join ? (base.name + "." + ref) : (col.empty() ? ref : col);
                projections.push_back({&base, base_idx, out_name});
            } else {
                projections.push_back({right_ptr, right_idx, right_ptr->name + "." + ref});
            }
        }
    }
    for (const auto& p : projections) out.columns.push_back(p.output_name);

    if (!plan.has_join) {
        std::shared_lock<std::shared_mutex> base_table_lock(base.mutex);
        
        auto emit_row = [&](const PageRow& pr) {
            std::vector<std::string> proj; proj.reserve(projections.size());
            for (const auto& p : projections) proj.push_back(cell_value_string(base, pr, p.index));
            out.rows.push_back(std::move(proj));
        };

        if (skip_cache && plan.where.op == "=") {
            const std::string rhs = unquote_literal(plan.where.value);
            uint64_t loc = RobinHoodIndex::kEmpty;
            if (base.pk_is_int) {
                int64_t pk = 0; fast_parse_int64(rhs, pk);
                loc = base.pk_robin_index.lookup(pk);
            } else {
                auto it = base.primary_index.find(rhs);
                if (it != base.primary_index.end()) loc = it->second;
            }
            if (loc != RobinHoodIndex::kEmpty) {
                PageRow pr = fetch_row_from_buf(base.buf_pool.get(), loc);
                if (row_alive(pr, now_ts)) {
                    if (evaluate_where(base, pr, plan.where, &error)) {
                        emit_row(pr);
                    } else if (!error.empty()) {
                        return false;
                    }
                }
            }
        } else {
            if (!base.buf_pool || !base.disk_mgr) return true;
            TableIterator iter(*base.buf_pool, base.disk_mgr->num_pages());
            while (iter.valid()) {
                const PageRow& pr = iter.current();
                if (row_alive(pr, now_ts)) {
                    bool pass = true;
                    if (plan.where.present) {
                        pass = evaluate_where(base, pr, plan.where, &error);
                        if (!error.empty()) return false;
                    }
                    if (pass) emit_row(pr);
                }
                iter.next();
            }
        }
    } else {
        std::shared_lock<std::shared_mutex> ta(base.name <= right_ptr->name ? base.mutex : right_ptr->mutex);
        std::shared_lock<std::shared_mutex> tb(base.name <= right_ptr->name ? right_ptr->mutex : base.mutex);
        
        std::string lj_tbl, lj_col, rj_tbl, rj_col;
        split_qualified(plan.join_left, lj_tbl, lj_col);
        split_qualified(plan.join_right, rj_tbl, rj_col);
        const Table* join_left_table = (lj_tbl == base.name) ? &base : right_ptr;
        const Table* join_right_table = (rj_tbl == base.name) ? &base : right_ptr;
        std::size_t left_join_idx = 0, right_join_idx = 0; std::string err;
        lookup_column(*join_left_table, lj_col, left_join_idx, err);
        lookup_column(*join_right_table, rj_col, right_join_idx, err);

        const Table* hash_table = &base; const Table* probe_table = right_ptr;
        std::size_t hash_idx = (join_left_table == &base) ? left_join_idx : right_join_idx;
        std::size_t probe_idx = (join_left_table == right_ptr) ? left_join_idx : right_join_idx;
        bool hash_is_base = true;

        std::unordered_map<std::string, std::vector<PageRow>> join_hash_str;
        
        if (hash_table->buf_pool && hash_table->disk_mgr) {
            TableIterator hit(*hash_table->buf_pool, hash_table->disk_mgr->num_pages());
            while (hit.valid()) {
                const PageRow& pr = hit.current();
                if (row_alive(pr, now_ts)) {
                    join_hash_str[pr.values[hash_idx]].push_back(pr);
                }
                hit.next();
            }
        }

        if (probe_table->buf_pool && probe_table->disk_mgr) {
            TableIterator pit(*probe_table->buf_pool, probe_table->disk_mgr->num_pages());
            while (pit.valid()) {
                const PageRow& probe_row = pit.current();
                if (!row_alive(probe_row, now_ts)) { pit.next(); continue; }

                if (plan.join_op == "=") {
                    auto it = join_hash_str.find(probe_row.values[probe_idx]);
                    if (it != join_hash_str.end()) {
                        for (const PageRow& hash_row : it->second) {
                            const PageRow* base_r = hash_is_base ? &hash_row : &probe_row;
                            const PageRow* right_r = hash_is_base ? &probe_row : &hash_row;
                            if (evaluate_where_join(base, *base_r, *right_ptr, *right_r, plan.where, &error)) {
                                std::vector<std::string> proj; proj.reserve(projections.size());
                                for (const auto& p : projections) {
                                    proj.push_back(cell_value_string(*(p.table), p.table == &base ? *base_r : *right_r, p.index));
                                }
                                out.rows.push_back(std::move(proj));
                            }
                        }
                    }
                } else {
                    // Nested loop for non-equality
                    for (const auto& [_, hash_rows] : join_hash_str) {
                        for (const PageRow& hash_row : hash_rows) {
                            const PageRow* base_r = hash_is_base ? &hash_row : &probe_row;
                            const PageRow* right_r = hash_is_base ? &probe_row : &hash_row;
                            
                            double lhs = 0, rhs = 0;
                            bool lhs_ok = fast_parse_double(probe_row.values[probe_idx], lhs);
                            bool rhs_ok = fast_parse_double(hash_row.values[hash_idx], rhs);
                            bool pass = false;
                            
                            std::string effective_op = plan.join_op;
                            if (hash_is_base) {
                                if (effective_op == ">") effective_op = "<";
                                else if (effective_op == "<") effective_op = ">";
                                else if (effective_op == ">=") effective_op = "<=";
                                else if (effective_op == "<=") effective_op = ">=";
                            }

                            if (lhs_ok && rhs_ok) {
                                eval_numeric_op(lhs, rhs, effective_op, pass);
                            } else {
                                std::string dummy_err;
                                compare_values(probe_row.values[probe_idx], hash_row.values[hash_idx], DataType::kVarchar, effective_op, pass, dummy_err);
                            }
                            
                            if (pass && evaluate_where_join(base, *base_r, *right_ptr, *right_r, plan.where, &error)) {
                                std::vector<std::string> proj; proj.reserve(projections.size());
                                for (const auto& p : projections) {
                                    proj.push_back(cell_value_string(*(p.table), p.table == &base ? *base_r : *right_r, p.index));
                                }
                                out.rows.push_back(std::move(proj));
                            }
                        }
                    }
                }
                pit.next();
            }
        }
    }

    if (plan.order_by.present) {
        std::size_t sort_col_idx = 0;
        for (std::size_t i = 0; i < out.columns.size(); ++i) {
            if (out.columns[i] == plan.order_by.column) { sort_col_idx = i; break; }
        }
        DataType sort_type = DataType::kVarchar;
        auto col_it = base.column_index.find(plan.order_by.column);
        if (col_it != base.column_index.end()) sort_type = base.columns[col_it->second].type;
        bool desc = plan.order_by.descending;
        std::stable_sort(out.rows.begin(), out.rows.end(),
            [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                const std::string& av = a[sort_col_idx];
                const std::string& bv = b[sort_col_idx];
                bool less = false;
                if (sort_type == DataType::kInt) { int64_t ai=0, bi=0; fast_parse_int64(av, ai); fast_parse_int64(bv, bi); less = ai < bi; }
                else if (sort_type == DataType::kDecimal) { double ad=0, bd=0; fast_parse_double(av, ad); fast_parse_double(bv, bd); less = ad < bd; }
                else less = av < bv;
                return desc ? !less : less;
            });
    }

    if (!skip_cache) cache_.put(cache_key, out, versions, binary_wire);
    return true;
}
bool SqlEngine::parse_create_table(const std::string& sql,
                                   std::string& table_name,
                                   std::vector<Column>& columns,
                                   int& primary_col,
                                   std::string& error) const {
    const std::string& s = sql;
    std::string upper = to_upper(s);
    if (!starts_with_keyword(upper, kCreateTableKw)) {
        error = "expected CREATE TABLE";
        return false;
    }

    std::size_t open_paren = s.find('(');
    std::size_t close_paren = s.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren) {
        error = "invalid CREATE TABLE syntax";
        return false;
    }

    std::string table_part_raw = trim(s.substr(std::strlen(kCreateTableKw), open_paren - std::strlen(kCreateTableKw)));
    std::string table_part_upper = to_upper(table_part_raw);
    std::string table_part;
    if (table_part_upper.rfind("IF NOT EXISTS", 0) == 0) {
        table_part = trim(table_part_raw.substr(std::strlen("IF NOT EXISTS")));
    } else {
        table_part = table_part_raw;
    }
    if (!is_identifier(table_part)) {
        error = "invalid table name";
        return false;
    }
    table_name = to_lower(table_part);

    std::string columns_part = s.substr(open_paren + 1, close_paren - open_paren - 1);
    auto defs = split_csv(columns_part);
    if (defs.empty()) {
        error = "table must contain at least one column";
        return false;
    }

    std::string pk_from_constraint;
    for (std::string def : defs) {
        def = trim(def);
        if (def.empty()) {
            continue;
        }
        std::string def_upper = to_upper(def);

        if (starts_with_keyword(def_upper, "PRIMARY KEY")) {
            std::size_t l = def.find('(');
            std::size_t r = def.find(')');
            if (l == std::string::npos || r == std::string::npos || r <= l + 1) {
                error = "invalid PRIMARY KEY constraint";
                return false;
            }
            pk_from_constraint = to_lower(trim(def.substr(l + 1, r - l - 1)));
            continue;
        }

        std::istringstream iss(def);
        std::string col_name;
        std::string type_token;
        if (!(iss >> col_name >> type_token)) {
            error = "invalid column definition: " + def;
            return false;
        }

        if (!is_identifier(col_name)) {
            error = "invalid column name: " + col_name;
            return false;
        }

        Column col;
        col.name = to_lower(col_name);
        std::string type_upper = to_upper(type_token);

        if (type_upper == "INT" || type_upper == "INTEGER") {
            col.type = DataType::kInt;
        } else if (type_upper == "DECIMAL") {
            col.type = DataType::kDecimal;
        } else if (type_upper == "DATETIME") {
            col.type = DataType::kDatetime;
        } else if (type_upper.rfind("VARCHAR", 0) == 0 || type_upper == "TEXT") {
            col.type = DataType::kVarchar;
            std::size_t l = type_token.find('(');
            std::size_t r = type_token.find(')');
            if (l != std::string::npos && r != std::string::npos && r > l + 1) {
                try {
                    col.varchar_limit = std::stoi(type_token.substr(l + 1, r - l - 1));
                } catch (...) {
                    error = "invalid VARCHAR length in: " + type_token;
                    return false;
                }
                if (col.varchar_limit <= 0) {
                    error = "VARCHAR length must be positive";
                    return false;
                }
            }
        } else {
            error = "unsupported type in column definition: " + type_token;
            return false;
        }

        std::string remainder;
        std::getline(iss, remainder);
        remainder = to_upper(trim(remainder));
        if (!remainder.empty()) {
            std::istringstream attrs(remainder);
            std::vector<std::string> tokens;
            std::string tok;
            while (attrs >> tok) {
                tokens.push_back(tok);
            }

            std::size_t i = 0;
            while (i < tokens.size()) {
                if (tokens[i] == "PRIMARY") {
                    if (i + 1 >= tokens.size() || tokens[i + 1] != "KEY") {
                        error = "unsupported column attributes: " + remainder;
                        return false;
                    }
                    col.primary_key = true;
                    col.not_null = true;
                    i += 2;
                    continue;
                }
                if (tokens[i] == "NOT") {
                    if (i + 1 >= tokens.size() || tokens[i + 1] != "NULL") {
                        error = "unsupported column attributes: " + remainder;
                        return false;
                    }
                    col.not_null = true;
                    i += 2;
                    continue;
                }
                if (tokens[i] == "NULL") {
                    i += 1;
                    continue;
                }
                error = "unsupported column attributes: " + remainder;
                return false;
            }
        }

        columns.push_back(col);
    }

    if (columns.empty()) {
        error = "table must contain at least one concrete column";
        return false;
    }

    std::set<std::string> seen;
    for (const Column& c : columns) {
        if (!seen.insert(c.name).second) {
            error = "duplicate column name: " + c.name;
            return false;
        }
    }

    primary_col = -1;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].primary_key) {
            if (primary_col != -1) {
                error = "multiple primary keys are not supported";
                return false;
            }
            primary_col = static_cast<int>(i);
        }
    }

    if (!pk_from_constraint.empty()) {
        if (primary_col != -1) {
            error = "primary key already specified inline";
            return false;
        }
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == pk_from_constraint) {
                columns[i].primary_key = true;
                columns[i].not_null = true;
                primary_col = static_cast<int>(i);
                break;
            }
        }
        if (primary_col == -1) {
            error = "PRIMARY KEY references unknown column: " + pk_from_constraint;
            return false;
        }
    }

    return true;
}

bool SqlEngine::parse_insert(const std::string& sql,
                             std::string& table_name,
                             std::vector<std::vector<std::string>>& rows,
                             std::vector<std::int64_t>& expires_at,
                             std::string& error) const {
    const std::string& s = sql;
    const char* p = s.c_str();
    while (*p != '\0' && is_space_char(*p)) {
        ++p;
    }
    if (!starts_with_ci(p, "INSERT")) {
        error = "expected INSERT INTO";
        return false;
    }
    p += 6;
    while (*p != '\0' && is_space_char(*p)) {
        ++p;
    }
    if (!starts_with_ci(p, "INTO")) {
        error = "expected INSERT INTO";
        return false;
    }
    p += 4;
    while (*p != '\0' && is_space_char(*p)) {
        ++p;
    }

    const char* tn = p;
    while (*p != '\0' && (std::isalnum(static_cast<unsigned char>(*p)) != 0 || *p == '_')) {
        ++p;
    }
    std::string table_part(tn, p);
    if (!is_identifier(table_part)) {
        error = "invalid table name in INSERT";
        return false;
    }
    table_name = to_lower(table_part);

    const char* values_kw = find_keyword_ci(p, "VALUES");
    if (values_kw == nullptr) {
        error = "INSERT must contain VALUES clause";
        return false;
    }
    p = values_kw + 6;
    while (*p != '\0' && is_space_char(*p)) {
        ++p;
    }

    std::size_t pos = static_cast<std::size_t>(p - s.c_str());
    rows.clear();
    expires_at.clear();

    auto skip_spaces = [&](std::size_t& p) {
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])) != 0) {
            ++p;
        }
    };

    skip_spaces(pos);
    if (pos >= s.size() || s[pos] != '(') {
        error = "INSERT VALUES missing opening parenthesis";
        return false;
    }

    while (pos < s.size() && s[pos] == '(') {
        int depth = 0;
        bool in_single = false;
        bool in_double = false;
        std::size_t close_paren = std::string::npos;
        for (std::size_t i = pos; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\'' && !in_double) {
                if (i + 1 < s.size() && s[i + 1] == '\'') {
                    ++i;
                    continue;
                }
                in_single = !in_single;
                continue;
            }
            if (c == '"' && !in_single) {
                in_double = !in_double;
                continue;
            }
            if (in_single || in_double) {
                continue;
            }
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    close_paren = i;
                    break;
                }
            }
        }

        if (close_paren == std::string::npos) {
            error = "INSERT VALUES missing closing parenthesis";
            return false;
        }

        std::vector<std::string> tuple_values;
        tuple_values.reserve(8);
        std::string token;
        token.reserve(close_paren - pos);
        bool tuple_in_single = false;
        bool tuple_in_double = false;
        for (std::size_t i = pos + 1; i < close_paren; ++i) {
            const char c = s[i];
            if (c == '\'' && !tuple_in_double) {
                token.push_back(c);
                if (i + 1 < close_paren && s[i + 1] == '\'') {
                    token.push_back(s[++i]);
                } else {
                    tuple_in_single = !tuple_in_single;
                }
                continue;
            }
            if (c == '"' && !tuple_in_single) {
                tuple_in_double = !tuple_in_double;
                token.push_back(c);
                continue;
            }
            if (c == ',' && !tuple_in_single && !tuple_in_double) {
                tuple_values.push_back(unquote_literal(token));
                token.clear();
                continue;
            }
            token.push_back(c);
        }
        tuple_values.push_back(unquote_literal(token));
        rows.push_back(std::move(tuple_values));

        pos = close_paren + 1;
        skip_spaces(pos);
        if (pos < s.size() && s[pos] == ',') {
            ++pos;
            skip_spaces(pos);
            if (pos >= s.size() || s[pos] != '(') {
                error = "INSERT VALUES has malformed tuple list";
                return false;
            }
            continue;
        }
        break;
    }

    if (rows.empty()) {
        error = "INSERT must provide at least one row";
        return false;
    }

    std::int64_t ttl_or_expiry = 0;
    std::string tail = trim(s.substr(pos));
    if (tail.empty()) {
        expires_at.assign(rows.size(), 0);
        return true;
    }

    std::string tail_upper = to_upper(tail);
    if (starts_with_keyword(tail_upper, "EXPIRES")) {
        std::string token = trim(tail.substr(std::strlen("EXPIRES")));
        token = unquote_literal(token);
        if (token.empty()) {
            error = "EXPIRES expects a unix timestamp or datetime";
            return false;
        }

        try {
            std::size_t consumed = 0;
            std::int64_t epoch = std::stoll(token, &consumed);
            if (consumed == token.size()) {
                ttl_or_expiry = epoch;
                expires_at.assign(rows.size(), ttl_or_expiry);
                return true;
            }
        } catch (...) {
        }

        std::int64_t parsed = 0;
        if (!parse_datetime_to_unix(token, parsed)) {
            error = "invalid EXPIRES value, expected unix timestamp or YYYY-MM-DD HH:MM:SS";
            return false;
        }
        ttl_or_expiry = parsed;
        expires_at.assign(rows.size(), ttl_or_expiry);
        return true;
    }

    if (starts_with_keyword(tail_upper, "TTL")) {
        std::string token = trim(tail.substr(std::strlen("TTL")));
        token = unquote_literal(token);
        if (token.empty()) {
            error = "TTL expects a number of seconds";
            return false;
        }

        std::int64_t ttl = 0;
        try {
            std::size_t consumed = 0;
            ttl = std::stoll(token, &consumed);
            if (consumed != token.size()) {
                error = "invalid TTL value";
                return false;
            }
        } catch (...) {
            error = "invalid TTL value";
            return false;
        }

        if (ttl < 0) {
            error = "TTL cannot be negative";
            return false;
        }
        ttl_or_expiry = now_unix() + ttl;
        expires_at.assign(rows.size(), ttl_or_expiry);
        return true;
    }

    error = "unsupported INSERT tail; use EXPIRES <unix|datetime> or TTL <seconds>";
    return false;
}

bool SqlEngine::parse_select(const std::string& sql, SelectPlan& plan, std::string& error) const {
    plan = SelectPlan{};

    const std::string& s = sql;
    std::string upper = to_upper(s);
    if (!starts_with_keyword(upper, kSelectKw)) {
        error = "expected SELECT statement";
        return false;
    }

    std::size_t from_pos = find_keyword(upper, "FROM", 0);
    if (from_pos == std::string::npos) {
        error = "SELECT missing FROM clause";
        return false;
    }

    std::string select_part = trim(s.substr(std::strlen(kSelectKw), from_pos - std::strlen(kSelectKw)));
    if (select_part.empty()) {
        error = "SELECT list cannot be empty";
        return false;
    }

    std::string from_remainder = trim(s.substr(from_pos + std::strlen("FROM")));
    if (from_remainder.empty()) {
        error = "SELECT missing table after FROM";
        return false;
    }

    std::string from_upper = to_upper(from_remainder);
    std::size_t join_pos = find_keyword(from_upper, "INNER JOIN", 0);
    std::size_t where_pos = find_keyword(from_upper, "WHERE", 0);

    std::string where_clause;

    if (join_pos == std::string::npos) {
        std::size_t order_pos = find_keyword(from_upper, "ORDER BY", 0);
        std::string table_part;

        std::size_t first_clause = std::string::npos;
        if (where_pos != std::string::npos) first_clause = where_pos;
        if (order_pos != std::string::npos && (first_clause == std::string::npos || order_pos < first_clause))
            first_clause = order_pos;

        if (first_clause == std::string::npos) {
            table_part = trim(from_remainder);
        } else {
            table_part = trim(from_remainder.substr(0, first_clause));
            if (where_pos != std::string::npos && (order_pos == std::string::npos || where_pos < order_pos)) {
                std::size_t where_end = where_pos + std::strlen("WHERE");
                std::size_t where_clause_end = (order_pos != std::string::npos && order_pos > where_pos)
                    ? order_pos : std::string::npos;
                if (where_clause_end == std::string::npos)
                    where_clause = trim(from_remainder.substr(where_end));
                else
                    where_clause = trim(from_remainder.substr(where_end, where_clause_end - where_end));
            }
            if (order_pos != std::string::npos) {
                std::string order_str = trim(from_remainder.substr(order_pos + std::strlen("ORDER BY")));
                std::string order_upper = to_upper(order_str);
                bool desc = false;
                if (order_upper.size() >= 5 &&
                    order_upper.substr(order_upper.size() - 5) == " DESC") {
                    order_str = trim(order_str.substr(0, order_str.size() - 5));
                    desc = true;
                } else if (order_upper.size() >= 4 &&
                           order_upper.substr(order_upper.size() - 4) == " ASC") {
                    order_str = trim(order_str.substr(0, order_str.size() - 4));
                }
                plan.order_by.column = to_lower(trim(order_str));
                plan.order_by.descending = desc;
                plan.order_by.present = true;
            }
        }

        if (!is_identifier(table_part)) {
            error = "invalid table name in FROM";
            return false;
        }

        plan.base_table = to_lower(table_part);
        plan.has_join = false;
        plan.touched_tables = {plan.base_table};
    } else {
        std::string left_table_part = trim(from_remainder.substr(0, join_pos));
        if (!is_identifier(left_table_part)) {
            error = "invalid left table in INNER JOIN";
            return false;
        }
        plan.base_table = to_lower(left_table_part);
        plan.has_join = true;

        std::string join_remainder = trim(from_remainder.substr(join_pos + std::strlen("INNER JOIN")));
        std::string join_upper = to_upper(join_remainder);
        std::size_t on_pos = find_keyword(join_upper, "ON", 0);
        if (on_pos == std::string::npos) {
            error = "INNER JOIN missing ON clause";
            return false;
        }

        std::string right_table_part = trim(join_remainder.substr(0, on_pos));
        if (!is_identifier(right_table_part)) {
            error = "invalid right table in INNER JOIN";
            return false;
        }
        plan.join_table = to_lower(right_table_part);

        std::string on_remainder = trim(join_remainder.substr(on_pos + std::strlen("ON")));
        std::string on_upper = to_upper(on_remainder);
        std::size_t where_on_pos = find_keyword(on_upper, "WHERE", 0);
        std::string join_condition;
        if (where_on_pos == std::string::npos) {
            join_condition = trim(on_remainder);
        } else {
            join_condition = trim(on_remainder.substr(0, where_on_pos));
            where_clause = trim(on_remainder.substr(where_on_pos + std::strlen("WHERE")));
        }

        // Support =, >=, <=, >, < in JOIN ON
        std::string join_op;
        std::size_t eq = std::string::npos;
        for (std::size_t i = 0; i + 1 < join_condition.size(); ++i) {
            if ((join_condition[i] == '>' || join_condition[i] == '<') && join_condition[i+1] == '=') {
                join_op = join_condition.substr(i, 2); eq = i; break;
            }
        }
        if (eq == std::string::npos) {
            for (std::size_t i = 0; i < join_condition.size(); ++i) {
                if (join_condition[i] == '=' || join_condition[i] == '>' || join_condition[i] == '<') {
                    join_op = join_condition.substr(i, 1); eq = i; break;
                }
            }
        }
        if (eq == std::string::npos) { error = "join condition must contain operator"; return false; }
        plan.join_left = trim(join_condition.substr(0, eq));
        plan.join_right = trim(join_condition.substr(eq + join_op.size()));
        plan.join_op = join_op;
        if (plan.join_left.empty() || plan.join_right.empty()) {
            error = "invalid join condition";
            return false;
        }
        std::string jl_tbl;
        std::string jl_col;
        std::string jr_tbl;
        std::string jr_col;
        if (!split_qualified(plan.join_left, jl_tbl, jl_col) ||
            !split_qualified(plan.join_right, jr_tbl, jr_col) ||
            jl_tbl.empty() || jl_col.empty() || jr_tbl.empty() || jr_col.empty() ||
            !is_identifier(jl_tbl) || !is_identifier(jl_col) ||
            !is_identifier(jr_tbl) || !is_identifier(jr_col)) {
            error = "invalid join condition, expected table.column = table.column";
            return false;
        }
        plan.join_left = to_lower(jl_tbl) + "." + to_lower(jl_col);
        plan.join_right = to_lower(jr_tbl) + "." + to_lower(jr_col);

        plan.touched_tables = {plan.base_table, plan.join_table};
    }

    if (select_part == "*") {
        plan.select_star = true;
    } else {
        plan.select_columns = split_csv(select_part);
        if (plan.select_columns.empty()) {
            error = "empty SELECT projection";
            return false;
        }
        for (std::string& c : plan.select_columns) {
            c = trim(c);
            if (c.empty()) {
                error = "invalid empty projection in SELECT";
                return false;
            }
            std::string tbl;
            std::string col;
            if (split_qualified(c, tbl, col)) {
                if (col.empty() || !is_identifier(col) || (!tbl.empty() && !is_identifier(tbl))) {
                    error = "invalid projected column: " + c;
                    return false;
                }
                c = tbl.empty() ? to_lower(col) : to_lower(tbl) + "." + to_lower(col);
            } else {
                if (!is_identifier(c)) {
                    error = "invalid projected column: " + c;
                    return false;
                }
                c = to_lower(c);
            }
        }
    }

    if (!where_clause.empty()) {
        Condition cond;
        if (!parse_condition(where_clause, cond, error)) {
            return false;
        }
        std::string tbl;
        std::string col;
        split_qualified(cond.column, tbl, col);
        cond.column = tbl.empty() ? to_lower(cond.column) : to_lower(tbl) + "." + to_lower(col);
        plan.where = cond;
    }

    plan.valid = true;
    return true;
}

bool SqlEngine::evaluate_where(const Table& table, const PageRow& row, const Condition& condition, std::string* error) const {
    if (!condition.present) return true;
    std::string cond_table, cond_col;
    split_qualified(condition.column, cond_table, cond_col);
    std::string raw_col = cond_col.empty() ? condition.column : cond_col;
    if (!cond_table.empty() && cond_table != table.name) { if (error) *error = "WHERE references unknown table: " + cond_table; return false; }
    std::size_t idx = 0; std::string lookup_error;
    const Column* col = lookup_column(table, raw_col, idx, lookup_error);
    if (!col) { if (error) *error = lookup_error; return false; }
    bool result = false;
    const std::string rhs = unquote_literal(condition.value);
    std::string cmp_error;
    if (!compare_values(cell_value_string(table, row, idx), rhs, col->type, condition.op, result, cmp_error)) {
        if (error) { *error = cmp_error; }
        return false;
    }
    return result;
}

bool SqlEngine::evaluate_where_join(const Table& left, const PageRow& left_row, const Table& right, const PageRow& right_row, const Condition& condition, std::string* error) const {
    if (!condition.present) return true;
    std::string cond_table, cond_col;
    split_qualified(condition.column, cond_table, cond_col);
    std::string raw_col = cond_col.empty() ? condition.column : cond_col;
    const Table* table = nullptr; const PageRow* row_ptr = nullptr;
    if (cond_table.empty()) {
        std::size_t dummy; std::string e1, e2;
        bool in_l = lookup_column(left, raw_col, dummy, e1) != nullptr;
        bool in_r = lookup_column(right, raw_col, dummy, e2) != nullptr;
        if (in_l && in_r) { if (error) *error = "ambiguous column"; return false; }
        if (!in_l && !in_r) { if (error) *error = "unknown column"; return false; }
        table = in_l ? &left : &right; row_ptr = in_l ? &left_row : &right_row;
    } else if (cond_table == left.name) { table = &left; row_ptr = &left_row;
    } else if (cond_table == right.name) { table = &right; row_ptr = &right_row;
    } else { if (error) *error = "unknown table"; return false; }
    
    std::size_t idx = 0; std::string lookup_error;
    const Column* col = lookup_column(*table, raw_col, idx, lookup_error);
    if (!col) { if (error) *error = lookup_error; return false; }
    bool result = false; std::string cmp_error;
    if (!compare_values(cell_value_string(*table, *row_ptr, idx), unquote_literal(condition.value), col->type, condition.op, result, cmp_error)) {
        if (error) { *error = cmp_error; }
        return false;
    }
    return result;
}

std::unordered_map<std::string, std::uint64_t>
SqlEngine::capture_versions(const std::vector<std::string>& table_names) const {
    std::unordered_map<std::string, std::uint64_t> out;
    out.reserve(table_names.size());
    for (const std::string& t : table_names) {
        auto it = tables_.find(t);
        if (it != tables_.end()) {
            std::shared_lock<std::shared_mutex> table_lock(it->second.mutex);
            out[t] = it->second.version;
        }
    }
    return out;
}

std::string SqlEngine::trim(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string SqlEngine::to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

std::string SqlEngine::to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string SqlEngine::normalize_sql_for_cache(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    bool in_single = false;
    bool in_double = false;

    for (char c : s) {
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            out.push_back(c);
            prev_space = false;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            out.push_back(c);
            prev_space = false;
            continue;
        }

        if (!in_single && !in_double && std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        prev_space = false;

        if (!in_single && !in_double) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            out.push_back(c);
        }
    }

    out = trim(out);
    if (!out.empty() && out.back() == ';') {
        out.pop_back();
        out = trim(out);
    }
    return out;
}

bool SqlEngine::is_identifier(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
        return false;
    }
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> SqlEngine::split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string token;
    int depth = 0;
    bool in_single = false;
    bool in_double = false;

    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !in_double) {
            if (i + 1 < s.size() && s[i + 1] == '\'') {
                token.push_back(c);
                token.push_back(s[++i]);
                continue;
            }
            in_single = !in_single;
            token.push_back(c);
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            token.push_back(c);
            continue;
        }

        if (!in_single && !in_double) {
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            } else if (c == ',' && depth == 0) {
                out.push_back(trim(token));
                token.clear();
                continue;
            }
        }
        token.push_back(c);
    }

    if (!token.empty() || !s.empty()) {
        out.push_back(trim(token));
    }

    return out;
}

std::string SqlEngine::unquote_literal(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && is_space_char(s[b])) ++b;
    std::size_t e = s.size();
    while (e > b && is_space_char(s[e - 1])) --e;

    if (e - b < 2) {
        return std::string(s.data() + b, e - b);
    }

    const char front = s[b];
    const char back  = s[e - 1];
    if ((front == '\'' && back == '\'') || (front == '"' && back == '"')) {
        char q = front;
        std::string out;
        out.reserve(e - b - 2);
        for (std::size_t i = b + 1; i < e - 1; ++i) {
            if (s[i] == q && i + 1 < e - 1 && s[i + 1] == q) {
                out.push_back(q);
                ++i;
                continue;
            }
            out.push_back(s[i]);
        }
        return out;
    }

    return std::string(s.data() + b, e - b);
}

std::int64_t SqlEngine::now_unix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool SqlEngine::parse_datetime_to_unix(const std::string& s, std::int64_t& out) {
    std::tm tm{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (iss.fail()) {
        iss.clear();
        iss.str(s);
        iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (iss.fail()) {
            return false;
        }
    }

    tm.tm_isdst = -1;
    std::time_t tt = std::mktime(&tm);
    if (tt == static_cast<std::time_t>(-1)) {
        return false;
    }

    out = static_cast<std::int64_t>(tt);
    return true;
}

bool SqlEngine::fast_parse_int64(const std::string& s, std::int64_t& out) {
    if (s.empty()) {
        return false;
    }
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    while (begin < end && is_space_char(*begin)) {
        ++begin;
    }
    while (end > begin && is_space_char(*(end - 1))) {
        --end;
    }
    if (begin == end) {
        return false;
    }
    auto parsed = std::from_chars(begin, end, out, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool SqlEngine::fast_parse_double(const std::string& s, double& out) {
    if (s.empty()) {
        return false;
    }

    const char* begin = s.data();
    const char* endp = s.data() + s.size();
    while (begin < endp && is_space_char(*begin)) {
        ++begin;
    }
    while (endp > begin && is_space_char(*(endp - 1))) {
        --endp;
    }
    if (begin == endp) {
        return false;
    }

    auto parsed_fast = std::from_chars(begin, endp, out, std::chars_format::general);
    if (parsed_fast.ec == std::errc{} && parsed_fast.ptr == endp) {
        return true;
    }

    std::string tmp(begin, endp);
    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(tmp.c_str(), &end);
    if (errno == ERANGE || end == tmp.c_str() || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

bool SqlEngine::is_null_literal_ci(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_space_char(s[b])) {
        ++b;
    }
    while (e > b && is_space_char(s[e - 1])) {
        --e;
    }
    if (e - b != 4) {
        return false;
    }
    return std::toupper(static_cast<unsigned char>(s[b])) == 'N' &&
           std::toupper(static_cast<unsigned char>(s[b + 1])) == 'U' &&
           std::toupper(static_cast<unsigned char>(s[b + 2])) == 'L' &&
           std::toupper(static_cast<unsigned char>(s[b + 3])) == 'L';
}

bool SqlEngine::parse_numeric_literal(const std::string& s, DataType type, double& out) {
    if (is_null_literal_ci(s)) {
        return false;
    }

    if (type == DataType::kInt) {
        std::int64_t n = 0;
        if (!fast_parse_int64(s, n)) {
            return false;
        }
        out = static_cast<double>(n);
        return true;
    }
    if (type == DataType::kDecimal) {
        return fast_parse_double(s, out);
    }
    if (type == DataType::kDatetime) {
        std::int64_t ts = 0;
        if (!parse_datetime_to_unix(trim(s), ts)) {
            return false;
        }
        out = static_cast<double>(ts);
        return true;
    }
    return false;
}

bool SqlEngine::eval_numeric_op(double lhs, double rhs, const std::string& op, bool& out) {
    if (op == "=") { out = (lhs == rhs); return true; }
    if (op == "!=") { out = (lhs != rhs); return true; }
    if (op == "<") { out = (lhs < rhs); return true; }
    if (op == "<=") { out = (lhs <= rhs); return true; }
    if (op == ">") { out = (lhs > rhs); return true; }
    if (op == ">=") { out = (lhs >= rhs); return true; }
    return false;
}

bool SqlEngine::compare_values(const std::string& lhs,
                               const std::string& rhs,
                               DataType type,
                               const std::string& op,
                               bool& out,
                               std::string& error) {
    const bool lhs_is_null = is_null_literal_ci(lhs);
    const bool rhs_is_null = is_null_literal_ci(rhs);
    if (lhs_is_null || rhs_is_null) {
        if (op == "=") { out = lhs_is_null && rhs_is_null; return true; }
        if (op == "!=") { out = lhs_is_null != rhs_is_null; return true; }
        out = false;
        return true;
    }

    auto apply_op = [&](auto a, auto b) {
        if (op == "=") { out = (a == b); }
        else if (op == "!=") { out = (a != b); }
        else if (op == "<") { out = (a < b); }
        else if (op == "<=") { out = (a <= b); }
        else if (op == ">") { out = (a > b); }
        else if (op == ">=") { out = (a >= b); }
        else { error = "unsupported operator in WHERE: " + op; return false; }
        return true;
    };

    if (type == DataType::kInt) {
        std::int64_t a = 0, b = 0;
        if (!fast_parse_int64(lhs, a) || !fast_parse_int64(rhs, b)) {
            error = "invalid integer comparison value";
            return false;
        }
        return apply_op(a, b);
    }
    if (type == DataType::kDecimal) {
        double a = 0.0, b = 0.0;
        if (!fast_parse_double(lhs, a) || !fast_parse_double(rhs, b)) {
            error = "invalid decimal comparison value";
            return false;
        }
        return apply_op(a, b);
    }
    if (type == DataType::kDatetime) {
        std::int64_t a = 0, b = 0;
        if (!parse_datetime_to_unix(lhs, a) || !parse_datetime_to_unix(rhs, b)) {
            return apply_op(lhs, rhs);
        }
        return apply_op(a, b);
    }
    return apply_op(lhs, rhs);
}

bool SqlEngine::parse_condition(const std::string& text, Condition& out, std::string& error) {
    std::string s = trim(text);
    if (s.empty()) {
        error = "invalid WHERE clause; expected a single condition";
        return false;
    }

    bool in_single = false;
    bool in_double = false;
    std::size_t op_pos = std::string::npos;
    std::string op;

    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;

        if (i + 1 < s.size()) {
            if (s[i] == '!' && s[i+1] == '=') { op_pos = i; op = "!="; break; }
            if (s[i] == '<' && s[i+1] == '=') { op_pos = i; op = "<="; break; }
            if (s[i] == '>' && s[i+1] == '=') { op_pos = i; op = ">="; break; }
        }
        if (c == '=' || c == '<' || c == '>') { op_pos = i; op.assign(1, c); break; }
    }

    if (op_pos == std::string::npos) {
        error = "invalid WHERE clause; expected a single condition";
        return false;
    }

    std::string lhs = trim(s.substr(0, op_pos));
    std::string rhs = trim(s.substr(op_pos + op.size()));
    if (lhs.empty() || rhs.empty()) {
        error = "invalid WHERE clause; expected a single condition";
        return false;
    }

    std::string tbl, col;
    if (!split_qualified(lhs, tbl, col)) {
        error = "invalid WHERE clause; expected a valid column reference";
        return false;
    }
    const std::string raw_col = col.empty() ? lhs : col;
    if (!is_identifier(raw_col) || (!tbl.empty() && !is_identifier(tbl))) {
        error = "invalid WHERE clause; expected a valid column reference";
        return false;
    }

    out.present = true;
    out.column = lhs;
    out.op = op;
    out.value = rhs;
    return true;
}

bool SqlEngine::split_qualified(const std::string& qualified,
                                std::string& table,
                                std::string& column) {
    table.clear();
    column.clear();
    std::size_t dot = qualified.find('.');
    if (dot == std::string::npos) {
        column = qualified;
        return true;
    }
    if (qualified.find('.', dot + 1) != std::string::npos) {
        return false;
    }
    table = trim(qualified.substr(0, dot));
    column = trim(qualified.substr(dot + 1));
    return !table.empty() && !column.empty();
}

bool SqlEngine::starts_with_keyword(const std::string& sql_upper, const std::string& kw) {
    if (sql_upper.rfind(kw, 0) != 0) return false;
    if (sql_upper.size() == kw.size()) return true;
    return std::isspace(static_cast<unsigned char>(sql_upper[kw.size()])) != 0;
}

std::size_t SqlEngine::find_keyword(const std::string& sql_upper,
                                    const std::string& kw,
                                    std::size_t start) {
    bool in_single = false;
    bool in_double = false;

    for (std::size_t i = start; i + kw.size() <= sql_upper.size(); ++i) {
        char c = sql_upper[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;
        if (sql_upper.compare(i, kw.size(), kw) != 0) continue;

        bool left_ok = (i == 0) || std::isspace(static_cast<unsigned char>(sql_upper[i-1])) || sql_upper[i-1] == ')';
        bool right_ok = (i + kw.size() == sql_upper.size()) ||
                        std::isspace(static_cast<unsigned char>(sql_upper[i + kw.size()])) ||
                        sql_upper[i + kw.size()] == '(';
        if (left_ok && right_ok) return i;
    }
    return std::string::npos;
}

bool SqlEngine::row_alive(const PageRow& row, std::int64_t now_ts) {
    return row.expires_at == 0 || row.expires_at > now_ts;
}


std::string SqlEngine::cell_value_string(const Table& table, const PageRow& row, std::size_t col_idx) const {
    if (col_idx >= table.columns.size() || col_idx >= row.values.size()) return {};
    return row.values[col_idx];
}


bool SqlEngine::validate_typed_value(const Column& col,
                                     std::string& value,
                                     std::string& error,
                                     double* numeric_out,
                                     bool* numeric_valid) const {
    if (numeric_valid != nullptr) *numeric_valid = false;
    trim_in_place(value);
    if (is_null_literal_ci(value)) {
        if (col.not_null || col.primary_key) {
            error = "NULL is not allowed for column " + col.name;
            return false;
        }
        return true;
    }

    if (col.type == DataType::kInt) {
        std::int64_t parsed = 0;
        if (!fast_parse_int64(value, parsed)) {
            error = "invalid INT value for column " + col.name;
            return false;
        }
        if (numeric_out) *numeric_out = static_cast<double>(parsed);
        if (numeric_valid) *numeric_valid = true;
        return true;
    }
    if (col.type == DataType::kDecimal) {
        double parsed = 0.0;
        if (!fast_parse_double(value, parsed)) {
            error = "invalid DECIMAL value for column " + col.name;
            return false;
        }
        if (numeric_out) *numeric_out = parsed;
        if (numeric_valid) *numeric_valid = true;
        return true;
    }
    if (col.type == DataType::kDatetime) {
        std::int64_t parsed = 0;
        if (!parse_datetime_to_unix(value, parsed)) {
            error = "invalid DATETIME for column " + col.name + " (use YYYY-MM-DD HH:MM:SS)";
            return false;
        }
        if (numeric_out) *numeric_out = static_cast<double>(parsed);
        if (numeric_valid) *numeric_valid = true;
        return true;
    }
    if (col.varchar_limit > 0 && static_cast<int>(value.size()) > col.varchar_limit) {
        error = "VARCHAR length exceeded for column " + col.name;
        return false;
    }
    return true;
}

const SqlEngine::Column* SqlEngine::lookup_column(const Table& table,
                                                   const std::string& col_ref,
                                                   std::size_t& idx,
                                                   std::string& error) const {
    auto it = table.column_index.find(col_ref);
    if (it == table.column_index.end()) {
        std::string key = to_lower(col_ref);
        it = table.column_index.find(key);
        if (it == table.column_index.end()) {
            error = "unknown column " + col_ref + " in table " + table.name;
            return nullptr;
        }
    }
    idx = it->second;
    return &table.columns[idx];
}

bool SqlEngine::execute_delete(const std::string& sql, std::string& error) {
    std::string upper = to_upper(sql);
    const std::string kw = "DELETE FROM";
    if (!starts_with_keyword(upper, kw)) {
        error = "expected DELETE FROM";
        return false;
    }
    std::string table_name = to_lower(trim(sql.substr(kw.size())));
    if (!table_name.empty() && table_name.back() == ';') {
        table_name.pop_back();
        table_name = trim(table_name);
    }
    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        error = "unknown table: " + table_name;
        return false;
    }
    Table& table = it->second;
    table.primary_index.clear();
    table.pk_robin_index = RobinHoodIndex(1024);
    if (table.disk_mgr) {
        // Since we are resetting the table, the easiest way is to create a new disk manager / buffer pool
        table.buf_pool->flush_all();
        std::filesystem::remove("data/pages/" + table.name + ".db");
        table.disk_mgr = std::make_unique<DiskManager>(table.name);
        table.buf_pool = std::make_unique<BufferPoolManager>(*table.disk_mgr);
        table.last_page_id = INVALID_PAGE_ID;
    }
    ++table.version;
    return true;
}

}  // namespace flexql
void flexql::SqlEngine::load_from_disk() {
    std::unique_lock<std::shared_mutex> lock(db_mutex_);
    for (auto& [name, table] : tables_) {
        if (!table.buf_pool || !table.disk_mgr) continue;
        page_id_t num_pages = table.disk_mgr->num_pages();
        if (num_pages > 0) {
            table.last_page_id = num_pages - 1;
            TableIterator it(*table.buf_pool, num_pages);
            while (it.valid()) {
                if (table.primary_key_col >= 0) {
                    const PageRow& pr = it.current();
                    if (static_cast<size_t>(table.primary_key_col) < pr.values.size()) {
                        const std::string& pkval = pr.values[table.primary_key_col];
                        uint64_t loc = (static_cast<uint64_t>(it.current_page()) << 16) | it.current_slot();
                        if (table.pk_is_int) {
                            int64_t pk = 0;
                            fast_parse_int64(pkval, pk);
                            table.pk_robin_index.insert(pk, loc);
                        } else {
                            table.primary_index[pkval] = loc;
                        }
                    }
                }
                it.next();
            }
        }
        ++table.version;
    }
}

void flexql::SqlEngine::checkpoint_to_disk() {
    std::shared_lock<std::shared_mutex> lock(db_mutex_);
    for (auto& [name, table] : tables_) {
        std::shared_lock<std::shared_mutex> tlock(table.mutex);
        if (table.buf_pool) {
            table.buf_pool->flush_all(/*sync=*/true);  // fdatasync for durability
        }
    }
}
