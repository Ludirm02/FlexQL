# FlexQL Design Document

Repository Link: `https://github.com/Ludirm02/FlexQL`

---

## 1. System Overview

FlexQL is a lightweight, high-performance SQL-like database driver implemented entirely in C++17. It follows a client-server architecture:

- **Server (`flexql_server`)**: A multithreaded TCP server that manages persistent, paged disk storage and executes SQL-like queries.
- **Client API (`flexql_client`)**: Exposes the required C-compatible API (`flexql_open`, `flexql_close`, `flexql_exec`, `flexql_free`) and communicates with the server via TCP.
- **REPL (`flexql-client`)**: An interactive terminal built on top of the client API for typing and executing SQL statements.

---

## 2. Storage Design

### 2.1 Physical Storage: Buffer Pool + Paged Files

**Architecture:** Each table's data is stored in a binary heap file (`data/pages/<table>.db`) managed by a **Buffer Pool Manager** with LRU eviction. Table schemas are persisted separately in `data/tables/<table>.schema`.

**Page format:** Fixed 8192-byte pages. Each page has a 16-byte header (`PageHeader`) containing a row count and the next-page pointer. Rows are packed sequentially after the header.

**Row serialisation:** Each row is stored as:
```
[expires_at: int64][ncols: uint16][col_len: uint16][col_data: bytes]...
```
This is a compact, cache-friendly binary format. All values are stored as strings, normalised at insert time.

**Why paged heap files:** They allow datasets larger than available RAM. The buffer pool handles the working set in memory and evicts cold pages to disk automatically when the pool fills.

### 2.2 Buffer Pool Manager (LRU Eviction)

The `BufferPoolManager` manages a fixed pool of frames in RAM:
- **LRU list**: Tracks recently used pages; the least recently used unpinned page is evicted first.
- **Dirty-page write-back**: When a dirty page is evicted, it is written to disk via `pwrite()` before the frame is reused. This ensures data is never silently lost during eviction.
- **Pin/Unpin protocol**: Pages are pinned before reading/writing and unpinned afterwards, preventing eviction of in-use pages.
- **Page allocation**: New pages are allocated with an atomic `fetch_add` on the page count and immediately written to disk to reserve disk space.

### 2.3 Disk Manager

`DiskManager` wraps a raw file descriptor and uses `pread`/`pwrite` for all I/O:
- **No C++ stream buffering**: `pread`/`pwrite` are thread-safe syscalls — multiple threads can issue concurrent I/O to different page offsets without locking.
- **`fdatasync`**: Called only during checkpoints (not on every insert), ensuring high throughput while maintaining durability at checkpoint boundaries.

### 2.4 Schema Persistence

Column definitions are stored as binary-length-prefixed CREATE TABLE SQL strings in `data/tables/<table>.schema`. On server startup, schemas are loaded first, tables are created in memory, then `load_from_disk()` reads all pages and rebuilds the primary key index. This gives full crash recovery.

---

## 3. Durability and Crash Recovery

### 3.1 Two-Level Durability

FlexQL implements a two-level durability model:

**Level 1 — Process crash safety (always guaranteed):**
Every `pwrite()` call writes data to the OS page cache. If the FlexQL server process crashes (SIGKILL, segfault), the OS page cache survives intact and the kernel eventually flushes it to physical storage. This means data written via `pwrite()` survives all process-level crashes.

**Level 2 — Power-loss safety (guaranteed within 10 seconds):**
A **background checkpoint thread** calls `checkpoint_to_disk()` every 10 seconds. This flushes all dirty buffer pool pages to disk and calls `fdatasync()` to force the OS page cache to physical storage. In the worst case, at most 10 seconds of committed data can be lost on hard power failure.

A final `checkpoint_to_disk()` is also called on clean shutdown (SIGINT/SIGTERM).

### 3.2 Background Checkpoint Thread

```cpp
// Runs every 10 seconds in a dedicated thread
engine.checkpoint_to_disk();  // flush_all + fdatasync per table
```

The checkpoint thread does **not** hold `table.mutex` during disk I/O. It only holds `db_mutex_` (shared) to prevent the table map from changing. The `BufferPoolManager::flush_all()` uses its own internal mutex (held briefly to collect dirty page IDs), then writes and syncs outside any table lock. This ensures checkpoints do not block concurrent INSERTs.

### 3.3 Startup Recovery

On server start:
1. Schema files are read and tables are recreated.
2. `load_from_disk()` scans all page files, deserialises every row, and rebuilds the primary key index.
3. Legacy WAL entries (from earlier server versions) are replayed if present, then the WAL is cleared.

This gives full crash recovery — after any crash, restoring the server state from the disk files is sufficient to resume operations.

### 3.4 Known Durability Trade-off

The durability window is bounded by the checkpoint interval (default 10 seconds). For strict "every committed operation survives power loss" semantics, this would need to be reduced to 0 seconds (i.e., `fdatasync` after every INSERT). This was not implemented because it reduces insert throughput by approximately 10×. The 10-second checkpoint interval is a deliberate balance between throughput and durability, consistent with the approach used by PostgreSQL's `checkpoint_timeout`.

---

## 4. Scalability Analysis

### 4.1 Data Size (Pages) — Handles Any Size

The buffer pool correctly handles datasets larger than available RAM. As the pool fills, LRU eviction writes cold pages to disk and loads hot pages from disk. For the 10M row benchmark (~500MB of data) with a 256MB buffer pool, approximately half the pages are evicted to disk during inserts, and the LRU list naturally keeps the hot pages in memory for reads.

### 4.2 Primary Key Index — RAM-Bound

The Robin Hood index stores all primary key-to-location mappings in a flat in-memory array. For INT primary keys:
- Each entry: ~24 bytes (`key: int64`, `val: size_t`, `dist: int`)
- 10M rows × 24 bytes = **~240 MB** of index (fits comfortably in RAM)
- 100M rows × 24 bytes = ~2.4 GB (feasible on modern machines)

**Limitation:** For truly massive datasets (1 TB+, ~20 billion rows), the primary key index would require ~480 GB of RAM, which is not feasible. A disk-based B-tree index would be required for that scale. This is a known architectural trade-off; the current design is optimised for the assignment target of 10M rows.

---

## 5. Indexing

### 5.1 Primary Key Index: Robin Hood Open-Addressing Hash Map

**Choice:** A custom Robin Hood open-addressing hash map (`RobinHoodIndex`) for integer primary keys.

**Rationale:**
- All slots stored in a single flat `std::vector<Slot>` — zero pointer chasing, maximum cache locality.
- Robin Hood probing minimises average probe distance (<2 probes at 75% load).
- O(1) amortised insert and lookup.
- For integer PKs, hashing takes one instruction (Murmur3 finaliser).
- Outperforms `std::unordered_map` which uses linked-list buckets with 2–3 pointer dereferences per lookup.

For VARCHAR primary keys, `std::unordered_map<std::string, std::size_t>` is used as a fallback.

**Hash function:** Murmur3 finaliser — `x ^= x >> 33; x *= 0xff51afd7ed558ccd; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53; x ^= x >> 33`

**Index value encoding:** The index maps `key → (page_id << 16 | slot_index)`, giving O(1) direct page access for point queries.

### 5.2 Equality Lookup Optimisation

For `SELECT ... WHERE pk = value`, the engine uses the Robin Hood index for O(1) page access, bypassing the full table scan entirely. This path also skips the LRU query cache to avoid polluting it with unique point queries.

---

## 6. Caching Strategy

**Choice:** LRU (Least Recently Used) query cache with version-based invalidation.

### 6.1 Cache Key

SQL text is normalised before use as a cache key:
- Whitespace collapsed to single spaces
- Keywords uppercased (outside string literals)
- Trailing semicolons removed

This ensures `SELECT * FROM users` and `select *  from users` share the same cache entry.

### 6.2 Cache Invalidation

Each table maintains a monotonic `version` counter. Every `INSERT` increments the version. A cached result stores a snapshot of all referenced table versions at the time of caching. On cache lookup, the stored versions are compared against current versions — if any mismatch, the entry is treated as a miss and evicted.

### 6.3 Cache Capacity

The cache holds up to 2048 entries with a 512MB total memory cap and a 256MB per-entry limit. Pre-serialised wire bytes are cached in both text and binary format to avoid repeated serialisation.

### 6.4 Benefits

Repeated identical `SELECT` queries (e.g., dashboard queries, range scans) are served in microseconds without re-executing the scan — the benchmark shows a **4–13× speedup** on cache hits.

---

## 7. Multithreading Design

### 7.1 Thread Pool

The server uses a fixed thread pool (`hardware_concurrency() × 2` workers). Each accepted TCP connection is dispatched to a worker thread via a `std::queue` protected by a `std::mutex` and `std::condition_variable`. This avoids the overhead of spawning a new thread per connection.

### 7.2 Concurrency Control

Two levels of read-write locking:

| Lock | Type | Held by |
|------|------|---------|
| `db_mutex_` (global) | `std::shared_mutex` | Shared by SELECT/INSERT; exclusive by CREATE TABLE |
| `table.mutex` (per-table) | `std::shared_mutex` | Shared by SELECT; exclusive by INSERT |

**JOIN deadlock prevention:** When locking two tables for a JOIN, locks are acquired in lexicographic name order, preventing circular deadlocks.

**Checkpoint non-interference:** The background checkpoint thread holds only `db_mutex_` (shared) and does **not** hold `table.mutex` during disk I/O, ensuring checkpoints never block concurrent INSERT operations.

### 7.3 Design Rationale

This reader-writer lock design allows N concurrent `SELECT` queries on the same or different tables with zero contention between them. Write operations (`INSERT`) are serialised per table but are fast. For the benchmark workload (bulk insert followed by read queries), this provides optimal throughput.

---

## 8. SQL Execution Design

| Statement | Notes |
|-----------|-------|
| `CREATE TABLE` | `INT`, `DECIMAL`, `VARCHAR(n)`, `TEXT`, `DATETIME`; `PRIMARY KEY` constraint |
| `INSERT INTO ... VALUES (...)` | Single and multi-row batch inserts |
| `INSERT INTO ... VALUES (...) TTL <sec>` | Row expiry via TTL |
| `INSERT INTO ... VALUES (...) EXPIRES <unix\|datetime>` | Row expiry via timestamp |
| `SELECT * FROM ...` | Full table scan |
| `SELECT col1, col2 FROM ...` | Projected scan |
| `SELECT ... WHERE col op value` | Single-condition filter (=, !=, <, <=, >, >=) |
| `SELECT ... INNER JOIN ... ON ...` | Hash join O(N+M) |
| `SELECT ... INNER JOIN ... ON ... WHERE ...` | Hash join with filter |

### 8.1 JOIN Implementation: Hash Join O(N+M)

1. **Build phase:** Iterate the smaller table and insert `join_key → [row_locations]` into an `std::unordered_map`.
2. **Probe phase:** Iterate the larger table and look up each row's join key in the hash map.

For integer join keys, `unordered_map<int64_t, vector<uint64_t>>` is used to avoid string hashing overhead.

### 8.2 Expiration Handling

Each row carries `int64_t expires_at`. At read time, expired rows are filtered inline:
```cpp
if (row.expires_at != 0 && row.expires_at <= now_unix()) continue;
```
Lazy-delete strategy — zero background thread overhead, no lock contention during cleanup.

---

## 9. Network and Protocol Design

### 9.1 Wire Protocol

**Request (client → server):**
```
Q <length>\n<sql-bytes>      (text result)
QB <length>\n<sql-bytes>     (binary result)
```

**Success response:**
```
OK <ncols>\n
COL\t<col1>\t<col2>\t...\n
ROW\t<val1>\t<val2>\t...\n
...
END\n
```

Special characters (`\t`, `\n`, `\\`) in field values are escaped.

### 9.2 Network Optimisations

- `TCP_NODELAY` on all sockets — disables Nagle's algorithm
- 16MB `SO_RCVBUF`/`SO_SNDBUF` — reduces kernel buffer pressure
- 65536-byte receive ring buffer per connection — reduces `recv()` syscall count
- Responses accumulated in memory and sent in large `send()` calls

### 9.3 Client API

```c
int flexql_open(const char *host, int port, FlexQL **db);
int flexql_close(FlexQL *db);
int flexql_exec(FlexQL *db, const char *sql,
                int (*callback)(void*, int, char**, char**),
                void *arg, char **errmsg);
void flexql_free(void *ptr);
```

`FlexQL` is an opaque struct — its internals are hidden from the user. `flexql_exec` invokes `callback` once per result row with column names and values as `char**` arrays.

---

## 10. Build and Execution Instructions

### Prerequisites
- `g++` ≥ 9 with C++17 support
- `make`
- POSIX-compliant Linux system

### Build
```bash
make -j$(nproc)
```

### Run Server
```bash
./build/flexql_server 9000
```

### Run REPL Client (in separate terminal)
```bash
./build/flexql-client 127.0.0.1 9000
```

### Run Benchmark (Bivas format)
```bash
# Unit tests only
./build/flexql_benchmark --unit-test

# Insertion benchmark (N rows) + unit tests
./build/flexql_benchmark 10000000
```

---

## 11. Performance Results

Benchmark environment: Linux, `g++` with `-O3 -DNDEBUG -flto -march=native`.

### Live Benchmark Results (verified)

| Metric | Batch=1000 | Batch=16384 |
|--------|-----------|-------------|
| 1M row INSERT | **261K rows/sec** | **1.45M rows/sec** |
| Point query (PK index) | **0.022 ms avg** | — |
| Full scan (50K rows returned of 100K) | ~32 ms | — |
| Cached query (first) | 21 ms | — |
| Cached query (second, cache hit) | **1.7 ms** (4-13× speedup) | — |
| Crash recovery after SIGKILL | ✅ Data survives | — |
| Unit tests (21/21) | ✅ PASS | — |

### Key Performance Techniques

| Technique | Impact |
|-----------|--------|
| `-O3 -march=native -flto` | 20–40% speed improvement vs `-O2` |
| Batch INSERT (1000–16384 rows/statement) | Reduces TCP round trips by 94–99% |
| Robin Hood hash index | O(1) PK lookup, 2× better cache locality than `std::unordered_map` |
| LRU buffer pool with dirty-page eviction | Handles datasets larger than RAM |
| LRU query cache (version-invalidated) | Cache hits served in microseconds |
| Background checkpoint (every 10s) | Power-loss safe without hot-path fdatasync cost |
| `checkpoint_to_disk()` without table lock | Checkpoints never block concurrent INSERTs |
| Shared mutex for reads | N concurrent SELECTs with zero contention |
| `TCP_NODELAY` + large socket buffers | Minimises network latency |
| Thread pool | Eliminates per-connection thread creation overhead |
