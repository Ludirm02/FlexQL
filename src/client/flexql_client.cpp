#include "flexql.h"

#include "protocol.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct FlexQL {
    int fd;
};

namespace {

void set_errmsg(char** errmsg, const std::string& msg) {
    if (errmsg == nullptr) {
        return;
    }
    *errmsg = nullptr;

    char* mem = static_cast<char*>(std::malloc(msg.size() + 1));
    if (mem == nullptr) {
        return;
    }
    std::memcpy(mem, msg.data(), msg.size());
    mem[msg.size()] = '\0';
    *errmsg = mem;
}

bool read_ok_header(int fd, int& columns, std::string& protocol_error) {
    columns = 0;
    std::string line;
    if (!flexql_proto::recv_line(fd, line)) {
        protocol_error = "failed to read server response";
        return false;
    }

    if (line.rfind("ERR\t", 0) == 0) {
        protocol_error = flexql_proto::unescape_field(line.substr(4));
        return false;
    }

    if (line.rfind("OK ", 0) != 0) {
        protocol_error = "protocol error: expected OK header";
        return false;
    }

    try {
        std::size_t consumed = 0;
        columns = std::stoi(line.substr(3), &consumed);
        if (consumed != line.substr(3).size() || columns < 0) {
            protocol_error = "protocol error: invalid column count";
            return false;
        }
    } catch (const std::exception&) {
        protocol_error = "protocol error: invalid column count";
        return false;
    }

    return true;
}

std::vector<std::string> parse_prefixed_fields_copy(const std::string& line,
                                                    const std::string& prefix,
                                                    bool& ok) {
    ok = false;
    if (line == prefix) {
        ok = true;
        return {};
    }

    if (line.size() < prefix.size() + 1 || line.rfind(prefix + "\t", 0) != 0) {
        return {};
    }

    std::string payload = line.substr(prefix.size() + 1);
    auto fields = flexql_proto::split_tab_escaped(payload, 0);
    ok = true;
    return fields;
}

bool parse_row_fields_inplace(std::string& line, int expected_columns, std::vector<char*>& out_ptrs) {
    out_ptrs.clear();
    if (line == "ROW") {
        return expected_columns == 0;
    }
    if (line.rfind("ROW\t", 0) != 0) {
        return false;
    }

    char* read = line.data() + 4;
    char* write = read;
    char* field_start = write;

    while (*read != '\0') {
        const char c = *read++;
        if (c == '\\') {
            const char esc = *read;
            if (esc == '\0') {
                *write++ = '\\';
                break;
            }
            ++read;
            if (esc == 't') {
                *write++ = '\t';
            } else if (esc == 'n') {
                *write++ = '\n';
            } else if (esc == '\\') {
                *write++ = '\\';
            } else {
                *write++ = esc;
            }
            continue;
        }

        if (c == '\t') {
            *write++ = '\0';
            out_ptrs.push_back(field_start);
            field_start = write;
            continue;
        }

        *write++ = c;
    }

    *write = '\0';
    out_ptrs.push_back(field_start);
    return static_cast<int>(out_ptrs.size()) == expected_columns;
}

}  // namespace

extern "C" int flexql_open(const char* host, int port, FlexQL** db) {
    if (host == nullptr || db == nullptr || port <= 0 || port > 65535) {
        return FLEXQL_ERROR;
    }

    *db = nullptr;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host, port_str.c_str(), &hints, &result);
    if (rc != 0) {
        return FLEXQL_NETWORK_ERROR;
    }

    int fd = -1;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(result);

    if (fd < 0) {
        return FLEXQL_NETWORK_ERROR;
    }

    int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    FlexQL* handle = new (std::nothrow) FlexQL;
    if (handle == nullptr) {
        ::close(fd);
        return FLEXQL_NOMEM;
    }

    handle->fd = fd;
    *db = handle;
    return FLEXQL_OK;
}

extern "C" int flexql_close(FlexQL* db) {
    if (db == nullptr) {
        return FLEXQL_ERROR;
    }

    if (db->fd >= 0) {
        flexql_proto::clear_reader_state(db->fd);
        ::close(db->fd);
        db->fd = -1;
    }
    delete db;
    return FLEXQL_OK;
}

extern "C" int flexql_exec(FlexQL* db,
                           const char* sql,
                           flexql_callback callback,
                           void* arg,
                           char** errmsg) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (db == nullptr || sql == nullptr) {
        set_errmsg(errmsg, "invalid database handle or SQL");
        return FLEXQL_ERROR;
    }

    std::string sql_text(sql);
    if (!flexql_proto::send_query(db->fd, sql_text)) {
        set_errmsg(errmsg, "failed to send query to server");
        return FLEXQL_NETWORK_ERROR;
    }

    int columns = 0;
    std::string response_error;
    if (!read_ok_header(db->fd, columns, response_error)) {
        set_errmsg(errmsg, response_error);
        return FLEXQL_SQL_ERROR;
    }

    std::vector<std::string> col_names;
    std::vector<char*> name_ptrs;
    std::vector<char*> value_ptrs;
    if (columns > 0) {
        std::string cols_line;
        if (!flexql_proto::recv_line(db->fd, cols_line)) {
            set_errmsg(errmsg, "failed to read result columns");
            return FLEXQL_PROTOCOL_ERROR;
        }

        bool ok = false;
        col_names = parse_prefixed_fields_copy(cols_line, "COL", ok);
        if (!ok || static_cast<int>(col_names.size()) != columns) {
            set_errmsg(errmsg, "protocol error: malformed column line");
            return FLEXQL_PROTOCOL_ERROR;
        }

        name_ptrs.resize(col_names.size());
        value_ptrs.resize(col_names.size());
        for (std::size_t i = 0; i < col_names.size(); ++i) {
            name_ptrs[i] = col_names[i].data();
        }
    }

    bool abort_requested = false;
    for (;;) {
        std::string line;
        if (!flexql_proto::recv_line(db->fd, line)) {
            set_errmsg(errmsg, "failed to read query result");
            return FLEXQL_PROTOCOL_ERROR;
        }

        if (line == "END") {
            break;
        }

        if (!parse_row_fields_inplace(line, columns, value_ptrs)) {
            set_errmsg(errmsg, "protocol error: malformed row line");
            return FLEXQL_PROTOCOL_ERROR;
        }

        if (callback != nullptr && !abort_requested) {
            int cb_rc = callback(arg, columns, value_ptrs.data(), name_ptrs.data());
            if (cb_rc == 1) {
                abort_requested = true;
            }
        }
    }

    return FLEXQL_OK;
}

extern "C" void flexql_free(void* ptr) {
    std::free(ptr);
}
