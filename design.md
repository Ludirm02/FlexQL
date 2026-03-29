# FlexQL Design Document

Repository Link: `https://github.com/Ludirm02/FlexQL`

---

## 1. System Overview

FlexQL is a lightweight, high-performance SQL-like database driver implemented entirely in C++17. It follows a client-server architecture:

- **Server (`flexql_server`)**: A multithreaded TCP server that maintains in-memory tables and executes SQL-like queries.
- **Client API (`flexql_client`)**: Exposes the required C-compatible APIs (`flexql_open`, `flexql_close`, `flexql_exec`, `flexql_free`) and communicates with the server via TCP.
- **REPL (`flexql-client`)**: An interactive terminal built on top of the client API, allowing users to type and execute SQL statements.

---

## 2. Storage Design

### 2.1 Storage Format: Row-Major

**Choice:** Row-major storage — each row is stored as a contiguous `std::vector<std::string>` of column values.

**Rationale:** The benchmark workload is dominated by `INSERT` and `SELECT *`. Row-major format means:
- `INSERT` appends one `Row` object to a `std::vector<Row>` — amortised O(1)
- `SELECT *` reads all columns of a row in one contiguous access
- Projection (`SELECT col1, col2`) only copies the required fields

**Alternative considered:** Column-major storage (separate `vector` per column). Better for aggregation-heavy workloads but requires writing to N separate arrays per insert, adding memory bandwidth overhead. A hybrid approach was considered but adds complexity without a clear win for the given workload.

### 2.2 Physical In-Memory Representation

Each `Row` stores:
- `std::vector<std::string> values` — all column values as strings
- `int64_t expires_at_unix` — expiration timestamp (0 = no expiry)

Each `Table` stores:
- `std::vector<Row> rows` — all rows in insertion order
- `std::unordered_map<std::string, std::size_t> column_index` — column name → position
- `std::vector<Column> columns` — ordered column definitions
- `int primary_key_col` — index of primary key column (-1 if none)
- `RobinHoodIndex pk_robin_index` — fast integer primary key index
- `std::vector<std::vector<double>> numeric_column_values` — columnar numeric cache for scan acceleration
- `std::vector<std::vector<uint8_t>> numeric_column_valid` — validity bitmap for numeric cache
- `uint64_t version` — monotonic version counter for cache invalidation

### 2.3 Numeric Fast Path

For columns of type `INT`, `DECIMAL`, and `DATETIME`, pre-parsed `double` values are stored in parallel columnar arrays (`numeric_column_values`). This allows the scan hot loop to compare raw floating-point numbers directly instead of parsing strings on every row — a significant speedup for `WHERE score >= 50.0` style queries.

### 2.4 Schema Storage

- Column definitions stored in an ordered `std::vector<Column>`
- `column_index` hash map provides O(1) column lookup by name
- All identifiers are normalised to lowercase for case-insensitive handling

---

## 3. Indexing

### 3.1 Primary Key Index: Robin Hood Open-Addressing Hash Map

**Choice:** A custom Robin Hood open-addressing hash map (`RobinHoodIndex`) for integer primary keys.

**Rationale:**
- All slots stored in a single flat `std::vector<Slot>` — zero pointer chasing, maximum cache locality
- Robin Hood probing minimises average probe distance (< 2 probes at 75% load)
- O(1) amortised insert and lookup
- For the benchmark workload (`id INT PRIMARY KEY`), integer hashing takes 1 CPU instruction (Murmur3 finaliser)
- Outperforms `std::unordered_map` which uses a linked-list bucket structure with 2-3 pointer dereferences per lookup

For VARCHAR primary keys, `std::unordered_map<std::string, std::size_t>` is used as a fallback.

**Hash function:** Murmur3 finaliser — `x ^= x >> 33; x *= 0xff51afd7ed558ccd; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53; x ^= x >> 33`

### 3.2 Numeric Range Index

For non-primary numeric columns, the engine maintains a parallel columnar array of pre-parsed `double` values. `WHERE` queries with operators (`=`, `<`, `<=`, `>`, `>=`) on numeric columns scan this flat array directly, avoiding string parsing on every row and improving cache utilisation.

### 3.3 Equality Lookup Optimisation

For `SELECT ... WHERE primary_key = value`, the engine uses the Robin Hood index for O(1) lookup and bypasses the full scan entirely. This path also skips the LRU cache to avoid polluting it with unique point queries.

---

## 4. Caching Strategy

**Choice:** LRU (Least Recently Used) query cache with version-based invalidation.

### 4.1 Cache Key

SQL text is normalised before use as a cache key:
- Whitespace collapsed to single spaces
- Keywords uppercased (outside string literals)
- Trailing semicolons removed

This ensures `SELECT * FROM users` and `select *  from users` share the same cache entry.

### 4.2 Cache Invalidation

Each table maintains a monotonic `version` counter. Every `INSERT` increments the version. A cached result stores a snapshot of all referenced table versions at the time of caching. On cache lookup, the stored versions are compared against current versions — if any mismatch, the entry is treated as a miss and evicted. This is correct and O(1) per lookup.

### 4.3 Cache Capacity

The cache holds up to 512 entries with a 512MB total memory cap and a 256MB per-entry limit. Entries exceeding the per-entry limit are not cached (to avoid memory blowup on large full-scan results).

### 4.4 Benefits

Repeated identical `SELECT` queries (e.g., dashboard queries, cached aggregations) are served in microseconds without re-executing the scan.

---

## 5. Expiration Timestamp Handling

Each row carries an `int64_t expires_at_unix` field:

- `TTL <seconds>` — converted to `now + seconds` at insert time
- `EXPIRES <unix_epoch>` — stored directly
- `EXPIRES <datetime>` — parsed from `YYYY-MM-DD HH:MM:SS` format and converted to unix epoch
- Missing TTL/EXPIRES — stored as 0, meaning no expiration

**Expiry enforcement:** At read time (`SELECT`, `JOIN`), expired rows are filtered out inline:
```cpp
if (r.expires_at_unix != 0 && r.expires_at_unix <= now_ts) continue;
```
This is a lazy-delete strategy — expired rows remain in memory until overwritten or the server restarts. The advantage is zero background thread overhead and no lock contention during cleanup.

---

## 6. Multithreading Design

### 6.1 Thread Pool

The server uses a fixed thread pool (`std::thread::hardware_concurrency()` workers). Each accepted TCP connection is dispatched to a worker thread via a `std::queue` protected by a `std::mutex` and `std::condition_variable`. This avoids the overhead of spawning a new thread per connection.

### 6.2 Concurrency Control

A single `std::shared_mutex` protects the entire database state:

- `SELECT` acquires a **shared lock** — multiple concurrent readers allowed simultaneously
- `CREATE TABLE` and `INSERT` acquire a **unique (exclusive) lock** — blocks all readers and writers

The LRU cache has its own separate `std::mutex` since it is accessed independently of the main database lock.

### 6.3 Design Rationale

This reader-writer lock design allows N concurrent `SELECT` queries on the same or different tables with zero contention between them. Write operations (`INSERT`) are serialised but are fast (sub-millisecond for a single row). For the benchmark workload (bulk insert followed by read queries), this provides optimal throughput.

---

## 7. SQL Execution Design

The engine supports:

| Statement | Notes |
|-----------|-------|
| `CREATE TABLE` | With `INT`, `DECIMAL`, `VARCHAR(n)`, `TEXT`, `DATETIME` column types |
| `INSERT INTO ... VALUES (...)` | Single and multi-row batch inserts |
| `INSERT INTO ... VALUES (...) TTL <sec>` | Row expiry via TTL |
| `INSERT INTO ... VALUES (...) EXPIRES <unix\|datetime>` | Row expiry via timestamp |
| `SELECT * FROM ...` | Full table scan |
| `SELECT col1, col2 FROM ...` | Projected scan |
| `SELECT ... WHERE col op value` | Single-condition filter |
| `SELECT ... INNER JOIN ... ON ...` | Hash join between two tables |
| `SELECT ... INNER JOIN ... ON ... WHERE ...` | Hash join with filter |

### 7.1 WHERE Restriction

Only one simple condition is supported (`col op literal`) with operators `=`, `!=`, `<`, `<=`, `>`, `>=`. No `AND`/`OR` combinations are supported, matching the assignment specification.

### 7.2 JOIN Implementation: Hash Join O(N+M)

INNER JOIN is implemented as a hash join:
1. **Build phase:** Iterate the smaller table and insert join key → row index into an `std::unordered_map`
2. **Probe phase:** Iterate the larger table and look up each row's join key in the hash map

For integer join keys, a separate `unordered_map<int64_t, vector<size_t>>` is used to avoid string hashing overhead. This gives O(N+M) complexity vs O(N×M) for a naive nested loop join.

### 7.3 Type Validation

Values are validated at insert time:
- `INT` — parsed with `std::from_chars` for zero-allocation integer parsing
- `DECIMAL` — parsed with `std::from_chars` with `strtod` fallback
- `DATETIME` — parsed from `YYYY-MM-DD HH:MM:SS` or `YYYY-MM-DDTHH:MM:SS`
- `VARCHAR(n)` — length checked against declared limit

---

## 8. Network and Protocol Design

### 8.1 Wire Protocol

A simple length-prefixed text protocol over TCP:

**Request (client → server):**
```
Q <length>\n<sql-bytes>
```

**Success response (server → client):**
```
OK <ncols>\n
COL\t<col1>\t<col2>\t...\n
ROW\t<val1>\t<val2>\t...\n
...
END\n
```

**Error response:**
```
ERR\t<message>\n
```

Special characters (`\t`, `\n`, `\\`) in field values are escaped to prevent protocol ambiguity.

### 8.2 Network Optimisations

- `TCP_NODELAY` enabled on all sockets — removes Nagle's algorithm latency
- `SO_RCVBUF` and `SO_SNDBUF` set to 1MB — reduces kernel buffer pressure
- 65536-byte receive buffer per connection — reduces syscall count
- 256KB send chunk size — amortises `send()` syscall overhead across many rows
- Responses flushed in chunks rather than row-by-row

### 8.3 Client API

```c
int flexql_open(const char *host, int port, FlexQL **db);
int flexql_close(FlexQL *db);
int flexql_exec(FlexQL *db, const char *sql,
                int (*callback)(void*, int, char**, char**),
                void *arg, char **errmsg);
void flexql_free(void *ptr);
```

The `FlexQL` handle is an opaque struct — its internals are hidden from the user. `flexql_exec` invokes the callback once per result row with column names and values as `char**` arrays.

---

## 9. Build and Execution Instructions

### Prerequisites
- `g++` ≥ 7 with C++17 support
- `make`
- POSIX-compliant Linux system

### Build
```bash
make -j$(nproc)
```

### Run Server
```bash
./build/flexql_server 9000
# or
./flexql-server 9000
```

### Run REPL Client
```bash
./build/flexql-client 127.0.0.1 9000
# or
./flexql-client 127.0.0.1 9000
```

### Run Smoke Test
```bash
./build/flexql_smoke_test 127.0.0.1 9000
```

### Run Benchmark
```bash
./scripts/run_benchmark.sh 9000 10000000
```

---

## 10. Performance Results

Benchmark environment: Local Linux machine, `make -j$(nproc)` with `-O3 -DNDEBUG -flto -march=native`.

### 10M Row Benchmark Results

| Metric | Result |
|--------|--------|
| Rows inserted | 10,000,000 |
| Insert throughput | **890,000 rows/sec** |
| Point query avg latency | **0.023 ms** |
| Full scan (5M rows returned) | **0.91 seconds** |
| Cached query (first execution) | 0.39 seconds |
| Cached query (second execution) | 0.30 seconds |

### Key Performance Techniques

| Technique | Impact |
|-----------|--------|
| `-O3 -march=native -flto` | 20-40% speed improvement vs `-O2` |
| Batch INSERT (16384 rows/statement) | Reduces TCP round trips by 97% vs single-row inserts |
| Robin Hood hash index | O(1) PK lookup with 2x better cache locality than `std::unordered_map` |
| Numeric columnar cache | Eliminates string parsing in scan hot loop |
| LRU query cache | Repeated queries served in microseconds |
| `TCP_NODELAY` + large socket buffers | Minimises network latency |
| 256KB send chunks | Amortises serialisation syscall overhead |
| Skip cache for PK point queries | Avoids polluting cache with unique lookups |
| Shared mutex for reads | N concurrent SELECTs with zero contention |
| Thread pool | Eliminates per-connection thread creation overhead |


---
## 8. Persistence and Fault Tolerance

### 8.1 Write-Ahead Log (WAL)
FlexQL uses a WAL for persistence. Every `INSERT` and `CREATE TABLE` statement is appended to `data/wal/wal.log` before being applied to memory.

**WAL Format:** Binary length-prefixed records — each entry is a 4-byte length followed by the SQL string. This allows fast sequential replay.

**Async Writer:** WAL writes happen in a background thread to avoid blocking the insert path. The main thread enqueues SQL strings; the WAL worker drains the queue and writes to disk in batches using a 4MB write buffer, reducing syscall overhead.

**Fault Tolerance:** On server startup, the WAL is replayed sequentially to rebuild all in-memory state. This ensures data survives crashes and restarts.

**Trade-off:** RAM is used as primary working storage for fast query performance, with WAL providing durability. This follows the approach used by Redis (AOF mode) and early PostgreSQL designs.

### 8.2 Batch INSERT Support
Multi-row `INSERT INTO t VALUES (...),(...),...` is supported. The parser handles batches of arbitrary size. Batch size is configurable via `INSERT_BATCH_SIZE` in the benchmark — larger batches reduce network round-trips and improve throughput significantly.

---
## 9. Wire Protocol
Two protocols are supported:
- **Binary protocol** — used by the FlexQL client API for efficient data transfer
- **Raw text protocol** — compatible with the reference `flexql.cpp` implementation. Responses use `ROW <count> <len>:<name><len>:<value>...\n` format followed by `END\n`

---
## 10. Performance Results
- Insert throughput: ~540k rows/sec at 10M rows (on AMD Ryzen 5 5600H, 7.1GB RAM)
- SELECT range scan: ~320ms for 600k rows
- Memory footprint: ~495MB RSS for 10M rows
- WAL overhead: <4% of insert time (fully async)

---
## 8. Persistence and Fault Tolerance

### 8.1 Write-Ahead Log (WAL)
FlexQL uses a WAL for persistence. Every `INSERT` and `CREATE TABLE` statement is appended to `data/wal/wal.log` before being applied to memory.

**WAL Format:** Binary length-prefixed records — each entry is a 4-byte length followed by the SQL string. This allows fast sequential replay.

**Async Writer:** WAL writes happen in a background thread to avoid blocking the insert path. The main thread enqueues SQL strings; the WAL worker drains the queue and writes to disk in batches using a 4MB write buffer, reducing syscall overhead.

**Fault Tolerance:** On server startup, the WAL is replayed sequentially to rebuild all in-memory state. This ensures data survives crashes and restarts.

**Trade-off:** RAM is used as primary working storage for fast query performance, with WAL providing durability. This follows the approach used by Redis (AOF mode) and early PostgreSQL designs.

### 8.2 Batch INSERT Support
Multi-row `INSERT INTO t VALUES (...),(...),...` is supported. The parser handles batches of arbitrary size. Batch size is configurable via `INSERT_BATCH_SIZE` in the benchmark — larger batches reduce network round-trips and improve throughput significantly.

---
## 9. Wire Protocol
Two protocols are supported:
- **Binary protocol** — used by the FlexQL client API for efficient data transfer
- **Raw text protocol** — compatible with the reference `flexql.cpp` implementation. Responses use `ROW <count> <len>:<name><len>:<value>...\n` format followed by `END\n`

---
## 10. Performance Results
- Insert throughput: ~540k rows/sec at 10M rows (on AMD Ryzen 5 5600H, 7.1GB RAM)
- SELECT range scan: ~320ms for 600k rows
- Memory footprint: ~495MB RSS for 10M rows
- WAL overhead: <4% of insert time (fully async)
