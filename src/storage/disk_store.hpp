#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <filesystem>

struct DiskRow {
    std::vector<std::string> values;
    std::int64_t expires_at = 0;
};

namespace DiskStore {

inline void write_schema(const std::string& table_name, const std::string& sql) {
    std::string path = "data/tables/" + table_name + ".schema";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::uint32_t len = sql.size();
    f.write(reinterpret_cast<const char*>(&len), sizeof(len));
    f.write(sql.data(), len);
}

inline std::vector<std::string> load_schemas() {
    std::vector<std::string> sqls;
    if (!std::filesystem::exists("data/tables")) return sqls;
    for (const auto& entry : std::filesystem::directory_iterator("data/tables")) {
        if (entry.path().extension() != ".schema") continue;
        std::ifstream f(entry.path(), std::ios::binary);
        std::uint32_t len = 0;
        if (!f.read(reinterpret_cast<char*>(&len), sizeof(len))) continue;
        std::string sql(len, '\0');
        if (!f.read(sql.data(), len)) continue;
        sqls.push_back(sql);
    }
    return sqls;
}

inline void append_rows(const std::string& table_name,
                        const std::vector<std::vector<std::string>>& rows,
                        const std::vector<std::int64_t>& expires_at) {
    std::string path = "data/tables/" + table_name + ".bin";
    std::ofstream f(path, std::ios::binary | std::ios::app);
    std::vector<char> diskbuf(4 * 1024 * 1024);
    f.rdbuf()->pubsetbuf(diskbuf.data(), diskbuf.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::int64_t exp = r < expires_at.size() ? expires_at[r] : 0;
        f.write(reinterpret_cast<const char*>(&exp), sizeof(exp));
        std::uint16_t ncols = static_cast<std::uint16_t>(rows[r].size());
        f.write(reinterpret_cast<const char*>(&ncols), sizeof(ncols));
        for (const auto& val : rows[r]) {
            std::uint16_t vlen = static_cast<std::uint16_t>(val.size());
            f.write(reinterpret_cast<const char*>(&vlen), sizeof(vlen));
            f.write(val.data(), val.size());
        }
    }
}

inline std::vector<DiskRow> load_rows(const std::string& table_name) {
    std::vector<DiskRow> rows;
    std::string path = "data/tables/" + table_name + ".bin";
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return rows;
    while (f) {
        std::int64_t exp = 0;
        if (!f.read(reinterpret_cast<char*>(&exp), sizeof(exp))) break;
        std::uint16_t ncols = 0;
        if (!f.read(reinterpret_cast<char*>(&ncols), sizeof(ncols))) break;
        DiskRow row;
        row.expires_at = exp;
        for (int i = 0; i < ncols; ++i) {
            std::uint16_t vlen = 0;
            if (!f.read(reinterpret_cast<char*>(&vlen), sizeof(vlen))) break;
            std::string val(vlen, '\0');
            if (!f.read(val.data(), vlen)) break;
            row.values.push_back(std::move(val));
        }
        if (static_cast<int>(row.values.size()) == ncols)
            rows.push_back(std::move(row));
    }
    return rows;
}

inline void truncate_table(const std::string& table_name) {
    std::string path = "data/tables/" + table_name + ".bin";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::string schema_path = "data/tables/" + table_name + ".schema";
    std::filesystem::remove(schema_path);
}

} // namespace DiskStore
