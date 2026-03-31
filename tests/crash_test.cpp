#include "flexql.h"
#include <iostream>
#include <string>
#include <sstream>

int row_count = 0;
int print_cb(void* data, int argc, char** values, char** names) {
    (void)data;
    for (int i = 0; i < argc; ++i) {
        std::cout << (names[i] ? names[i] : "") << "=" << (values[i] ? values[i] : "NULL") << " ";
    }
    std::cout << "\n";
    row_count++;
    return 0;
}

int main() {
    FlexQL* db = nullptr;
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) {
        std::cerr << "Cannot connect\n";
        return 1;
    }

    // Try to query data that should have survived the crash
    char* err = nullptr;
    row_count = 0;
    
    // Query TEST_USERS (from unit tests)
    int rc = flexql_exec(db, "SELECT * FROM TEST_USERS;", print_cb, nullptr, &err);
    if (rc != FLEXQL_OK) {
        std::cout << "[FAIL] TEST_USERS not found after crash: " << (err ? err : "") << "\n";
        if (err) flexql_free(err);
    } else {
        std::cout << "[PASS] TEST_USERS survived crash with " << row_count << " rows\n";
    }

    // Query BIG_USERS count
    row_count = 0;
    rc = flexql_exec(db, "SELECT ID FROM BIG_USERS WHERE ID = 1;", print_cb, nullptr, &err);
    if (rc != FLEXQL_OK) {
        std::cout << "[FAIL] BIG_USERS not found after crash: " << (err ? err : "") << "\n";
        if (err) flexql_free(err);
    } else {
        std::cout << "[PASS] BIG_USERS survived crash, found " << row_count << " row(s) for ID=1\n";
    }

    flexql_close(db);
    return 0;
}
