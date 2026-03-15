# FlexQL

FlexQL is a lightweight SQL-like database driver implemented in C++.
It provides:

- A multithreaded TCP server that stores data in-memory.
- A C-compatible client API (`flexql_open`, `flexql_close`, `flexql_exec`, `flexql_free`).
- An interactive REPL client.
- Support for core SQL-like operations required in the assignment.

## Features

- `CREATE TABLE` with schema validation.
- `INSERT INTO ... VALUES (...)` with row expiration support.
- Multi-row insert batching: `INSERT ... VALUES (...), (...), ...`.
- `SELECT *` and `SELECT col1, col2`.
- Single-condition `WHERE` (`=`, `!=`, `<`, `<=`, `>`, `>=`).
- `INNER JOIN ... ON ...` with optional `WHERE`.
- Primary-key indexing (`PRIMARY KEY`) via hash index.
- Numeric range index acceleration for numeric `WHERE` predicates.
- LRU query cache for `SELECT` result reuse.
- Thread-safe execution for concurrent clients.

## Project Layout

- `bin/`: runnable symlinks to built binaries.
- `build/`: compiled artifacts.
- `config/`: configuration directory placeholder.
- `include/`: public API and modular include namespaces.
- `src/client/`: client API and REPL frontend.
- `src/server/`: server entrypoint.
- `src/network/`: wire protocol implementation.
- `src/query/`: SQL parser + execution engine.
- `tests/`: smoke test and benchmark driver.
- `data/`: data/tables, data/indexes, data/wal placeholders.
- `docs/`: design decisions and performance notes.

## Build

`cmake` is supported, but a `Makefile` is also included.

### Option A: Make (recommended in this environment)

```bash
make -j$(nproc)
```

### Option B: CMake

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

Start server:

```bash
./flexql-server 9000
# or
./build/flexql_server 9000
```

Start REPL client:

```bash
./flexql-client 127.0.0.1 9000
# or
./build/flexql-client 127.0.0.1 9000
# or
./bin/flexql-client 127.0.0.1 9000
```

Run smoke test:

```bash
./build/flexql_smoke_test 127.0.0.1 9000
```

## Supported SQL Syntax

### CREATE TABLE

```sql
CREATE TABLE users (
  id INT PRIMARY KEY,
  name VARCHAR(64),
  score DECIMAL,
  created_at DATETIME
);
```

Also supports table-level primary key:

```sql
CREATE TABLE users (
  id INT,
  name VARCHAR(64),
  PRIMARY KEY(id)
);
```

### INSERT

```sql
INSERT INTO users VALUES (1, 'alice', 92.5, '2026-03-11 10:00:00');
INSERT INTO users VALUES (2, 'bob', 88.0, '2026-03-11 10:05:00'), (3, 'carol', 95.0, '2026-03-11 10:10:00');
```

Expiration options:

```sql
INSERT INTO users VALUES (2, 'bob', 88.0, '2026-03-11 10:05:00') TTL 3600;
INSERT INTO users VALUES (3, 'carol', 95.0, '2026-03-11 10:10:00') EXPIRES 1767225600;
INSERT INTO users VALUES (4, 'dave', 75.0, '2026-03-11 10:15:00') EXPIRES '2026-12-31 23:59:59';
```

Rows with expiration in the past are automatically ignored by `SELECT`.

### SELECT + WHERE

```sql
SELECT * FROM users;
SELECT id, name FROM users WHERE score >= 90;
```

### INNER JOIN

```sql
SELECT users.id, users.name, orders.amount
FROM users
INNER JOIN orders ON users.id = orders.user_id
WHERE orders.amount > 10;
```

## C API Usage

```c
#include "flexql.h"

static int cb(void* arg, int cols, char** vals, char** names) {
    (void)arg;
    for (int i = 0; i < cols; ++i) {
        printf("%s=%s\n", names[i], vals[i]);
    }
    return 0;
}

int main() {
    FlexQL* db = NULL;
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) return 1;

    char* err = NULL;
    flexql_exec(db, "SELECT * FROM users;", cb, NULL, &err);
    if (err) {
        fprintf(stderr, "%s\n", err);
        flexql_free(err);
    }

    flexql_close(db);
    return 0;
}
```

## Benchmark

Run with default rows:

```bash
./scripts/run_benchmark.sh 9000 10000000
```

Or direct benchmark execution:

```bash
./build/flexql_benchmark 127.0.0.1 9000 10000
```

Output includes insert throughput, indexed point-query latency, full-scan time, and cached query timings.
