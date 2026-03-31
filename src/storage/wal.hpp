#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstdint>
#include <thread>
#include <condition_variable>
#include <queue>
#include <atomic>

class WAL {
public:
    static WAL& instance() { static WAL w; return w; }

    bool open(const std::string& path) {
        file_.open(path, std::ios::app | std::ios::binary);
        if (!file_.is_open()) return false;
        file_.rdbuf()->pubsetbuf(walbuf_, sizeof(walbuf_));
        worker_ = std::thread([this]() { run(); });
        return true;
    }

    void log(const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(sql);
        cv_.notify_one();
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

    void flush_all() {
        // Signal stop and wait for queue to drain
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            cv_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
        if (file_.is_open()) file_.flush();
    }

    ~WAL() { flush_all(); }

private:
    char walbuf_[4 * 1024 * 1024];
    int flush_count_ = 0;
    void run() {
        while (true) {
            std::vector<std::string> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]{ return !queue_.empty() || stopping_; });
                while (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop();
                }
                if (stopping_ && queue_.empty() && batch.empty()) return;
            }
            std::string buf;
            buf.reserve(1 << 20);
            for (const auto& sql : batch) {
                std::uint32_t len = static_cast<std::uint32_t>(sql.size());
                buf.append(reinterpret_cast<const char*>(&len), sizeof(len));
                buf.append(sql.data(), len);
            }
            file_.write(buf.data(), buf.size());
            if (stopping_) { file_.flush(); file_.sync_with_stdio(false); return; }
        }
    }

    std::ofstream file_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::thread worker_;
    bool stopping_ = false;
};
