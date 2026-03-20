#include "flexql.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

struct PrintContext {
    int rows = 0;
};

std::string trim(const std::string& s) {
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

bool starts_with_select(const std::string& sql) {
    std::string t = trim(sql);
    if (t.size() < 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (std::toupper(static_cast<unsigned char>(t[i])) != "SELECT"[i]) {
            return false;
        }
    }
    return t.size() == 6 || std::isspace(static_cast<unsigned char>(t[6]));
}

int print_row(void* data, int columnCount, char** values, char** columnNames) {
    auto* ctx = static_cast<PrintContext*>(data);
    for (int i = 0; i < columnCount; ++i) {
        std::string col = columnNames[i] != nullptr ? columnNames[i] : "";
        for (char& c : col) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        std::cout << col << " = " << (values[i] != nullptr ? values[i] : "NULL") << "\n";
    }
    std::cout << "\n";
    ++ctx->rows;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    int port = 9000;

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
    }

    flexql* db = nullptr;
    int rc = flexql_open(host, port, &db);
    if (rc != FLEXQL_OK) {
        std::cerr << "failed to connect to server at " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "Connected to FlexQL server\n";
    std::cout << "Enter SQL statements ending with ';'. Type '.exit' to quit.\n";

    std::string buffer;
    std::string line;

    for (;;) {
        std::cout << (buffer.empty() ? "flexql> " : "   ...> ");
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::string tline = trim(line);
        if (buffer.empty() && (tline == ".exit" || tline == ".quit")) {
            break;
        }

        buffer += line;
        buffer.push_back('\n');

        if (line.find(';') == std::string::npos) {
            continue;
        }

        std::string sql = trim(buffer);
        buffer.clear();
        if (sql.empty()) {
            continue;
        }

        PrintContext ctx;
        char* errmsg = nullptr;
        bool is_select = starts_with_select(sql);

        rc = flexql_exec(db, sql.c_str(), print_row, &ctx, &errmsg);
        if (rc != FLEXQL_OK) {
            std::cerr << "ERROR: " << (errmsg != nullptr ? errmsg : "unknown") << "\n";
            if (errmsg != nullptr) {
                flexql_free(errmsg);
            }
            continue;
        }

        if (!is_select) {
            std::cout << "OK\n";
        }
    }

    flexql_close(db);
    std::cout << "Connection closed\n";
    return 0;
}
