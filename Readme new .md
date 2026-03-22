# FlexQL

FlexQL is a lightweight, high-performance SQL-like database driver implemented in C++17.
It provides a multithreaded TCP server, a C-compatible client API, and an interactive REPL.

## Features

- `CREATE TABLE` with `INT`, `DECIMAL`, `VARCHAR(n)`, `TEXT`, `DATETIME` column types
- `INSERT INTO ... VALUES (...)` with single and multi-row batch support
- Row expiration via `TTL <seconds>` or `EXPIRES <unix|datetime>`
- `SELECT *` and `SELECT col1, col2` with optional `WHERE` clause
- `INNER JOIN` between two tables with optional `WHERE`
- Single-condition `WHERE` with operators `=`, `!=`, `<`, `<=`, `>`, `>=`
- Primary key indexing via Robin Hood open-addressing hash map (O(1) lookup)
- LRU query cache with version-based invalidation
- Multithreaded server with reader-writer locking
- Numeric columnar cache for fast scan acceleration

## Project Layout

```
FlexQL/
├── build/              # Compiled binaries
├── bin/                # Symlinks to binaries
├── include/            # Public API header (flexql.h)
├── src/
│   ├── client/         # C API implementation + REPL
│   ├── server/         # Server entry point
│   ├── network/        # Wire protocol
│   └── query/          # SQL parser + execution engine
├── tests/              # Smoke test + benchmark client
├── scripts/            # Benchmark script
└── docs/               # Design document + performance results
```

## Build

```bash
make -j$(nproc)
```

## Run

**Start server:**
```bash
./build/flexql_server 9000
```

**Start REPL client:**
```bash
./build/flexql-client 127.0.0.1 9000
```

**Run smoke test:**
```bash
./build/flexql_smoke_test 127.0.0.1 9000
```

**Run benchmark (10M rows):**
```bash
./scripts/run_benchmark.sh 9000 10000000
```

## Supported SQL

```sql
-- Create table
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), score DECIMAL, created DATETIME);

-- Insert single row
INSERT INTO users VALUES (1, 'alice', 92.5, '2026-03-11 10:00:00');

-- Insert multiple rows
INSERT INTO users VALUES (2, 'bob', 88.0, '2026-03-11 10:05:00'), (3, 'carol', 95.0, '2026-03-11 10:10:00');

-- Insert with expiry
INSERT INTO users VALUES (4, 'dave', 75.0, '2026-03-11 10:15:00') TTL 3600;
INSERT INTO users VALUES (5, 'eve', 80.0, '2026-03-11 10:20:00') EXPIRES 1767225600;

-- Select all
SELECT * FROM users;

-- Select specific columns
SELECT id, name FROM users;

-- With WHERE
SELECT * FROM users WHERE score >= 90;

-- Inner join
SELECT users.id, users.name, orders.amount
FROM users
INNER JOIN orders ON users.id = orders.user_id
WHERE orders.amount > 10;
```

## C API

```c
#include "flexql.h"

int callback(void *data, int ncols, char **values, char **colnames) {
    for (int i = 0; i < ncols; i++)
        printf("%s = %s\n", colnames[i], values[i]);
    printf("\n");
    return 0; // return 1 to abort
}

int main() {
    FlexQL *db = NULL;
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) return 1;

    char *err = NULL;
    flexql_exec(db, "SELECT * FROM users;", callback, NULL, &err);
    if (err) { fprintf(stderr, "%s\n", err); flexql_free(err); }

    flexql_close(db);
    return 0;
}
```

### Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `FLEXQL_OK` | 0 | Success |
| `FLEXQL_ERROR` | 1 | General error |
| `FLEXQL_NOMEM` | 2 | Out of memory |
| `FLEXQL_NETWORK_ERROR` | 3 | Network failure |
| `FLEXQL_PROTOCOL_ERROR` | 4 | Protocol violation |
| `FLEXQL_SQL_ERROR` | 5 | SQL execution error |

## Performance (10M rows)

| Metric | Result |
|--------|--------|
| Insert throughput | ~890,000 rows/sec |
| Point query (PK lookup) | ~0.023 ms avg |
| Full scan (5M rows) | ~0.91 seconds |
| Cached query (repeated) | ~0.30 seconds |

## Design

See [`docs/design.md`](docs/design.md) for full design documentation covering storage layout, indexing, caching, expiration, multithreading, and protocol design.
