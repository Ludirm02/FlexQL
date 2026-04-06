# FlexQL - A Flexible SQL-like Database Driver

**Source Code Repository:** [https://github.com/Ludirm02/FlexQL](https://github.com/Ludirm02/FlexQL)

FlexQL is a remarkably fast, fault-tolerant, multithreaded SQL-like database engine implemented completely in pure C/C++17. Designed to handle hundreds of millions of inserted rows far beyond standard OS memory constraints (out-of-core persistence), it leverages a fully bespoke Disk-Map Buffer Pool and an asynchronous Write-Ahead Log (WAL) layout capable of surviving extreme environment process crashes.

## Project Structure Highlights
* `src/client/` - Houses the interactive REPL client and `flexql_client.cpp`, translating dynamic SQL interactions strictly over proper `flexql.h` C-APIs without cheating connections.
* `src/query/` - The core AST Parser and SQL Evaluation engine (`sql_engine.cpp`). Safely intercepts disjointed JOIN columns, maps types across LRU Cache hierarchies mapped directly in RAM, and enforces memory mutexes for highly parallel multithreaded readers via `RobinHoodIndex`.
* `src/server/` - Natively bounds raw CPU-core thread limits using a unified background `ThreadPool`. Serves custom protocol connections asynchronously so throughput is mathematically stable. 
* `src/storage/` - Handles rigid mathematical disk I/O via `BufferPoolManager`. Maps 4K/8K binary pages onto `.db` OS files automatically so 1TB databases can comfortably exist exclusively on hard-disks without OOM crashes.
* `docs/` - Contains the absolute full specification logic of this application.

## Documentation
It is highly recommended you inspect the official deep-specification documents describing the true inner trade-offs and logical boundaries of this implementation framework.

1. **[Design Document (design.md)](./design.md)** - Extensive, descriptive explanations of Caching logic, Buffer Pool structures, Column/Row mappings, Index hashing structures, Multithreading safety, and Tradeoffs selected throughout the project's entire lifecycle.
2. **[Compilation Instructions (compilation.md)](./compilation.md)** - Concrete, explicit terminal inputs displaying how to compile cleanly, test 21/21 Unit validations, and stress-test the Write-Ahead Log's fault survivability metrics natively.
3. **[Performance Results (performance_results.md)](./performance_results.md)** - Raw data outputs logging the 1M, 10M, and ultra-high 100M benchmarks achieving over 680,000+ row insertions per second. 

## Quick Start
```bash
make clean && make -j$(nproc)
mkdir -p data/wal data/pages data/tables
./bin/flexql_server & 
./bin/flexql-client 127.0.0.1 9000
```
