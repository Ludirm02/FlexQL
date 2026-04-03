#include "flexql.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct Counter {
    std::size_t rows = 0;
};

int count_rows(void* data, int, char**, char**) {
    auto* c = static_cast<Counter*>(data);
    ++c->rows;
    return 0;
}

bool exec_sql(flexql* db, const std::string& sql, flexql_callback cb, void* cb_arg) {
    char* errmsg = nullptr;
    int rc = flexql_exec(db, sql.c_str(), cb, cb_arg, &errmsg);
    if (rc != FLEXQL_OK) {
        std::cerr << "SQL failed: " << sql << "\n";
        std::cerr << "Error: " << (errmsg != nullptr ? errmsg : "unknown") << "\n";
        if (errmsg != nullptr) {
            flexql_free(errmsg);
        }
        return false;
    }
    if (errmsg != nullptr) {
        flexql_free(errmsg);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = argc >= 3 ? std::atoi(argv[2]) : 9000;
    std::size_t rows_to_insert =
        argc >= 4 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 10000000;

    flexql* db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK) {
        std::cerr << "failed to connect to server\n";
        return 1;
    }

    if (!exec_sql(db, "CREATE TABLE bench_users (id INT PRIMARY KEY, name VARCHAR(32), score DECIMAL);", nullptr,
                  nullptr)) {
        flexql_close(db);
        return 1;
    }

    std::vector<std::string> score_lut(1000);
    for (int i = 0; i < 1000; ++i) {
        score_lut[static_cast<std::size_t>(i)] =
            std::to_string(i / 10) + "." + static_cast<char>('0' + (i % 10));
    }

    auto t0 = std::chrono::steady_clock::now();
    constexpr std::size_t kBatchSize = 16384;
    for (std::size_t base = 1; base <= rows_to_insert; base += kBatchSize) {
        const std::size_t end = std::min(rows_to_insert + 1, base + kBatchSize);
        std::string sql;
        sql.reserve(64 + (end - base) * 42);
        sql = "INSERT INTO bench_users VALUES ";
        for (std::size_t i = base; i < end; ++i) {
            if (i > base) {
                sql += ", ";
            }
            sql.push_back('(');
            sql += std::to_string(i);
            sql += ", 'user_";
            sql += std::to_string(i);
            sql += "', ";
            sql += score_lut[i % 1000];
            sql.push_back(')');
        }
        sql.push_back(';');

        if (!exec_sql(db, sql, nullptr, nullptr)) {
            flexql_close(db);
            return 1;
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::size_t> dist(1, rows_to_insert);

    const std::size_t point_queries = 5000;
    auto t2_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < point_queries; ++i) {
        std::string sql = "SELECT id, name FROM bench_users WHERE id = " + std::to_string(dist(rng)) + ";";
        Counter c;
        if (!exec_sql(db, sql, count_rows, &c)) {
            flexql_close(db);
            return 1;
        }
    }
    auto t2_end = std::chrono::steady_clock::now();

    Counter full_scan_count;
    auto t3_start = std::chrono::steady_clock::now();
    if (!exec_sql(db, "SELECT * FROM bench_users WHERE score >= 50.0;", count_rows, &full_scan_count)) {
        flexql_close(db);
        return 1;
    }
    auto t3_end = std::chrono::steady_clock::now();

    Counter cached_count_1;
    auto t4_start = std::chrono::steady_clock::now();
    if (!exec_sql(db, "SELECT * FROM bench_users WHERE score >= 80.0;", count_rows, &cached_count_1)) {
        flexql_close(db);
        return 1;
    }
    auto t4_mid = std::chrono::steady_clock::now();
    Counter cached_count_2;
    if (!exec_sql(db, "SELECT * FROM bench_users WHERE score >= 80.0;", count_rows, &cached_count_2)) {
        flexql_close(db);
        return 1;
    }
    auto t4_end = std::chrono::steady_clock::now();

    flexql_close(db);

    double insert_sec = std::chrono::duration<double>(t1 - t0).count();
    double point_sec = std::chrono::duration<double>(t2_end - t2_start).count();
    double full_scan_sec = std::chrono::duration<double>(t3_end - t3_start).count();
    double cached_first_sec = std::chrono::duration<double>(t4_mid - t4_start).count();
    double cached_second_sec = std::chrono::duration<double>(t4_end - t4_mid).count();

    std::cout << "rows_inserted=" << rows_to_insert << "\n";
    std::cout << "insert_total_seconds=" << insert_sec << "\n";
    std::cout << "insert_rows_per_second=" << (rows_to_insert / insert_sec) << "\n";
    std::cout << "point_queries=" << point_queries << "\n";
    std::cout << "point_query_total_seconds=" << point_sec << "\n";
    std::cout << "point_query_avg_ms=" << (point_sec * 1000.0 / point_queries) << "\n";
    std::cout << "full_scan_rows_returned=" << full_scan_count.rows << "\n";
    std::cout << "full_scan_seconds=" << full_scan_sec << "\n";
    std::cout << "cached_query_rows_first=" << cached_count_1.rows << "\n";
    std::cout << "cached_query_rows_second=" << cached_count_2.rows << "\n";
    std::cout << "cached_query_first_seconds=" << cached_first_sec << "\n";
    std::cout << "cached_query_second_seconds=" << cached_second_sec << "\n";

    return 0;
}
