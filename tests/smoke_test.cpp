#include "flexql.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct RowCollector {
    std::vector<std::vector<std::string>> rows;
};

int collect_row(void* data, int columnCount, char** values, char**) {
    auto* collector = static_cast<RowCollector*>(data);
    std::vector<std::string> row;
    for (int i = 0; i < columnCount; ++i) {
        row.emplace_back(values[i]);
    }
    collector->rows.push_back(std::move(row));
    return 0;
}

bool exec_sql(flexql* db, const std::string& sql, RowCollector* collector = nullptr) {
    char* errmsg = nullptr;
    int rc = flexql_exec(db,
                         sql.c_str(),
                         collector != nullptr ? collect_row : nullptr,
                         collector,
                         &errmsg);
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

    flexql* db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK) {
        std::cerr << "Unable to connect to FlexQL server\n";
        return 1;
    }

    bool ok = true;
    ok = ok && exec_sql(db, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age DECIMAL);");
    ok = ok && exec_sql(db, "CREATE TABLE orders (oid INT PRIMARY KEY, user_id INT, amount DECIMAL);");
    ok = ok && exec_sql(db, "INSERT INTO users VALUES (1, 'alice', 20);");
    ok = ok && exec_sql(db, "INSERT INTO users VALUES (2, 'bob', 30) TTL 3600;");
    ok = ok && exec_sql(db, "INSERT INTO orders VALUES (100, 1, 7.5);");
    ok = ok && exec_sql(db, "INSERT INTO orders VALUES (101, 2, 11.2);");

    RowCollector rows;
    ok = ok && exec_sql(db,
                        "SELECT users.id, users.name, orders.amount FROM users INNER JOIN orders ON users.id = orders.user_id WHERE orders.amount > 8;",
                        &rows);

    if (ok && rows.rows.size() != 1) {
        std::cerr << "Unexpected result count: " << rows.rows.size() << "\n";
        ok = false;
    }

    flexql_close(db);
    if (!ok) {
        return 1;
    }

    std::cout << "Smoke test passed\n";
    return 0;
}
