# FlexQL — Compilation and Execution Instructions

## Prerequisites

- `g++` version 7 or higher with C++17 support
- `make`
- POSIX-compliant Linux system

---

## Build

```bash
make -j$(nproc)
```

This compiles the server, client, smoke test, and benchmark with full optimisations (`-O3 -DNDEBUG -flto -march=native`).

---

## Run Server

```bash
./build/flexql_server 9000
```

Or using the root launcher script:

```bash
./flexql-server 9000
```

---

## Run REPL Client

```bash
./build/flexql-client 127.0.0.1 9000
```

Or using the root launcher script:

```bash
./flexql-client 127.0.0.1 9000
```

The REPL accepts SQL statements ending with `;`. Type `.exit` to quit.

---

## Run Smoke Test

```bash
./build/flexql_server 9000 &
sleep 0.5
./build/flexql_smoke_test 127.0.0.1 9000
```

Expected output:
```
Smoke test passed
```

---

## Run Benchmark (10M rows)

```bash
./scripts/run_benchmark.sh 9000 10000000
```

This will:
1. Build the project
2. Start the server
3. Insert 10 million rows
4. Run point queries, full scan, and cached query tests
5. Print results and save to `docs/performance_results.txt`

---
## WAL Persistence
The server automatically creates `data/wal/wal.log` for persistence.
To start fresh (clear all data):
```bash
rm -f data/wal/wal.log
```
Data survives server restarts automatically via WAL replay.
