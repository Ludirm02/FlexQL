#  FlexQL - The High-Performance C++ Database Engine

**Repository Link:** [https://github.com/Ludirm02/FlexQL](https://github.com/Ludirm02/FlexQL)

FlexQL is a lightweight SQL-like database engine implemented entirely in C++17. Engineered to avoid Out-of-Memory (OOM) failures by pushing data to a multithreaded disk Buffer Pool, it efficiently manages larger-than-RAM datasets (100 Million+ rows), achieving insertion speeds of **~807,000 operations per second**.

It provides a multithreaded TCP server bounded by Readers-Writer locks, a standard C-compatible client API (`flexql.h`), and an interactive database terminal REPL.

---

##  Core Technical Features

- **Standard SQL DDL/DML:** `CREATE TABLE` and `INSERT INTO ... VALUES (...)` with multi-row batch insert support.
- **Data Primitives:** Fully supports `INT`, `DECIMAL`, `VARCHAR(n)`, `TEXT`, and structured `DATETIME` inputs.
- **Automated Row Expiration:** Native parsing for `TTL <seconds>` or `EXPIRES <unix|datetime>`.
- **Querying & Filtering:** Supports `SELECT *`, scoped `SELECT col1, col2`, and `INNER JOIN` logic between distinct tables.
- **Relational Operators:** Fully supports single-condition `WHERE` filters with `=`, `!=`, `<`, `<=`, `>`, `>=` calculations.
- **O(1) Primary Key Indexing:** Uses custom **Robin Hood open-addressing hash maps** for O(1) primary key lookups.
- **LRU Query Caching:** Smart LRU bounds with table-version temporal invalidation limits `SELECT` repetition.
- **Fault-Tolerant Persistence:** A background Write-Ahead Log (WAL) thread explicitly protects unflushed memory from total power-loss failures (`kill -9`).

---

##  Project Layout

```text
FlexQL/
├── build/              # Compiled binaries
├── bin/                # Explicit execution symlinks
├── include/            # Public API header (flexql.h)
├── src/
│   ├── client/         # C API implementation + Interactive REPL
│   ├── server/         # ThreadPool Server daemon entry point
│   ├── network/        # Asynchronous TCP Wire protocol definitions
│   └── query/          # SQL AST parser + core Execution engine
├── tests/              # Native unit/crash test validation bindings
├── data/               # Persistent isolated storage arrays (pages, wal)
└── docs/               # Advanced architecture documentation
```

---

##  Build and Run Commands

**Compile:** (Pure C++17, NO external libraries required)
```bash
make clean && make -j$(nproc)
mkdir -p data/wal data/pages data/tables
```

**Start the Database Server daemon:**
```bash
./bin/flexql_server &
```

**Connect identically from the Client Terminal REPL:**
```bash
./bin/flexql-client 127.0.0.1 9000
```

**Execute 1-Million Row Benchmark:**
```bash
./bin/flexql_benchmark 1000000
```

**Execute the heavy 10-Million Row Benchmark:**
```bash
./bin/flexql_benchmark 10000000 
```

**Run Unit Tests Only:**
```bash
./bin/flexql_benchmark --unit-test
```

**Run Manual Crash Recovery Check:**
*(Simulates a sudden power loss during a massive ingestion)*
```bash
# In terminal A:
./bin/flexql_server 9000

# In terminal B:
./bin/flexql_benchmark 1000000 &
sleep 1
killall -9 flexql_server

# In terminal A (restart server and verify data survival):
./bin/flexql_server 9000
./bin/flexql_smoke_test
```

**Run the Master Auto-Validation Script (Validates tests + bounds + crashes):**
```bash
./scripts/run_all_tests.sh
```

**Troubleshooting: Reset Database (Wipe all data and WAL logs)**
If you repeatedly run massive benchmarks without shutting down properly, the `wal.log` file will grow continuously. Use this to reset everything safely if the server crashes on boot:
```bash
pkill -9 -f flexql_server
rm -rf data/wal/* data/pages/* data/tables/*
./bin/flexql_server &
```

---

##  Supported SQL Syntax

```sql
-- Create table with varying structural primitive types
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), score DECIMAL, created DATETIME);

-- Insert a standard single row parameter
INSERT INTO users VALUES (1, 'alice', 92.5, '2026-03-11 10:00:00');

-- Insert multiple row bounds synchronously (Batch injection)
INSERT INTO users VALUES (2, 'bob', 88.0, '2026-03-11 10:05:00'), (3, 'carol', 95.0, '2026-03-11 10:10:00');

-- Insert mapping logical time-based expiration commands (TTL)
INSERT INTO users VALUES (4, 'dave', 75.0, '2026-03-11 10:15:00') TTL 3600;
INSERT INTO users VALUES (5, 'eve', 80.0, '2026-03-11 10:20:00') EXPIRES 1767225600;

-- Standard table scans
SELECT * FROM users;

-- Filter isolated unique mapped columns efficiently
SELECT id, name FROM users;

-- Execute calculation filtering operators dynamically
SELECT * FROM users WHERE score >= 90;

-- Execute relational inner table joining mappings
SELECT users.id, users.name, orders.amount
FROM users
INNER JOIN orders ON users.id = orders.user_id
WHERE orders.amount > 10;
```

---

## Using the native C API 

FlexQL provides a clean C API via the `flexql.h` header, encapsulating internal connection and execution logic safely.

```c
#include "flexql.h"
#include <stdio.h>

// Dynamic callback natively invoked mechanically for every single distinct row successfully parsed inside a generic SELECT.
int callback(void *data, int ncols, char **values, char **colnames) {
    for (int i = 0; i < ncols; i++)
        printf("%s = %s\n", colnames[i], values[i]);
    printf("\n");
    return 0; // return 1 specifically to gracefully abort iteration sequence 
}

int main() {
    FlexQL *db = NULL;
    // Attempt standard TCP integration mapped upon port integers
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) return 1;

    char *err = NULL;
    // Route mapped generic text payloads
    flexql_exec(db, "SELECT * FROM users;", callback, NULL, &err);
    
    if (err) { 
        fprintf(stderr, "%s\n", err); 
        flexql_free(err); // Natively block standard leak boundaries safely
    }

    // Gracefully dispatch engine teardown structures safely
    flexql_close(db);
    return 0;
}
```

### Protocol Response Codes

| Status Code | Integer Value | Internal Engine Meaning |
|------|-------|---------|
| `FLEXQL_OK` | 0 | Success (Mapped cleanly) |
| `FLEXQL_ERROR` | 1 | General structural execution error |
| `FLEXQL_NOMEM` | 2 | Target execution strictly encountered memory limitations |
| `FLEXQL_NETWORK_ERROR` | 3 | Generic TCP validation handshake failure / Pipe disconnect |
| `FLEXQL_PROTOCOL_ERROR` | 4 | Severe connection packet protocol alignment violation |
| `FLEXQL_SQL_ERROR` | 5 | Ambiguous/Invalid SQL logical string generation |

---

##  Deep-Dive Technical Documentation
To understand the internal architecture, including how it supports `O(1)` query lookups and dataset sizes exceeding physical RAM:

1.  **[The FlexQL Deep-Dive Design Document (designdoc.md)](./designdoc.md)** - Detailed explanations covering the system's Buffer Pool memory allocation, Readers-Writer concurrency, Primary Key indexing, and architecture tradeoffs.
2.  **[Compilation & Stress-Test Guide (compilation.md)](./compilation.md)** - Instructions on how to test the database via unit tests, large datasets, and simulated power-loss validation.
3.  **[Peak Performance Test Results (performance_results.md)](./performance_results.md)** - Raw data logs demonstrating performance benchmarks.
