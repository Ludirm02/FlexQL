#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <filesystem>

namespace DiskStore {

inline void write_schema(const std::string& table_name, const std::string& sql) {
    std::filesystem::create_directories("data/tables");
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

} // namespace DiskStore
