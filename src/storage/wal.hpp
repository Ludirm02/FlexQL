#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstdint>

class WAL {
public:
    static WAL& instance() {
        static WAL w;
        return w;
    }

    bool open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        file_.open(path, std::ios::app | std::ios::binary);
        return file_.is_open();
    }

    void log(const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) return;
        std::uint32_t len = static_cast<std::uint32_t>(sql.size());
        file_.write(reinterpret_cast<const char*>(&len), sizeof(len));
        file_.write(sql.data(), len);
        file_.flush();
    }

    std::vector<std::string> replay(const std::string& path) {
        std::vector<std::string> sqls;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return sqls;
        std::uint32_t len = 0;
        while (in.read(reinterpret_cast<char*>(&len), sizeof(len))) {
            std::string sql(len, '\0');
            if (!in.read(sql.data(), len)) break;
            sqls.push_back(std::move(sql));
        }
        return sqls;
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
    std::string path_;
};
