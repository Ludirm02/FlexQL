# Compilation and Execution Instructions

This document summarizes exactly how to build and execute FlexQL, ranging from basic unit tests to aggressive high-load 100-million row benchmarks and brutal crash recovery tests.

## 1. Compilation
FlexQL uses a robust C++17 architecture requiring absolutely zero external libraries. To compile the entire system dynamically into your local `bin` folder, execute:
```bash
make clean && make -j$(nproc)
```

## 2. Setting Up the Database
Before running any tests, it is heavily recommended to start the server from a clean slate. This prepares the active physical `data/` structures.
```bash
# 1. Kill any existing instances of the server gracefully or forcefully:
killall -9 flexql_server 2>/dev/null

# 2. Build the directory layout and flush out the old data:
mkdir -p data/wal data/pages data/tables
rm -rf data/tables/* data/wal/* data/pages/*

# 3. Boot the server continuously into the background stream:
./bin/flexql_server > server.log 2>&1 & 

# Give the server a few seconds to spin up port listeners on TCP 9000
sleep 2
```

## 3. Running Unit Tests
Once the server is booted into the background, verify the AST String Parser and basic operations (e.g. `CREATE TABLE`, TTL Timestamps, `<`/`>` ops) by triggering the test script:
```bash
./bin/flexql_benchmark --unit-test
```
You should expect to see `Unit Test Summary: 21/21 passed, 0 failed.` output cleanly.

## 4. Running Benchmarks (Throughput Performance)
FlexQL is massively fast. To test insertive ingestion over raw `.db` pages, trigger the row commands. Wait for the engine to spit out `Throughput: ... rows/sec`.
```bash
# Light Benchmark
./bin/flexql_benchmark 1000000

# Medium Load 
./bin/flexql_benchmark 10000000

# Stress Load (Proving Out-Of-Core Memory survives 6GB)
./bin/flexql_benchmark 100000000
```


## 5. Crash Recovery & Durability Testing
To guarantee your un-flushed bits survive a complete machine hardware failure (via WAL asynchronous recovery logging), utilize this process block:

```bash
# 1. Recompile the crash test utility specifically
g++ -std=c++17 -Wall -Wextra tests/crash_test.cpp src/client/flexql_client.cpp src/network/protocol.cpp -Iinclude -Isrc -Isrc/network -o bin/crash_test

# 2. Fire an intense heavy bulk insertion into the background thread pool
./bin/flexql_benchmark 1000000 &
BENCH_PID=$!

# 3. Brutally execute process failure mid-flight (simulated power-loss)
sleep 1
killall -9 flexql_server

# 4. Prove the WAL buffered the mid-flight data! You will see massive file bytes here:
ls -lh data/wal/wal.log

# 5. Bring the Database server immediately back online to trigger WAL Replay Logic
./bin/flexql_server > server_recovery.log 2>&1 &
sleep 3

# 6. Check data survival
# Should output `[PASS] survived crash`!
```

## 6. Master Automated Testing Script
If you wish to bypass all manual commands, a comprehensive automated bash script has been provided. It seamlessly executes a clean wipe, boots the daemon, tests the 21/21 AST structures, runs the 10-Million row throughput metric, and executes absolute Fault-Tolerance (kill-9 simulations) sequentially in one single command.

```bash
./scripts/run_all_tests.sh
```
