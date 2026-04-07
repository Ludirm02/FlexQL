# Compilation and Execution Instructions

This document summarizes exactly how to build, run, and test FlexQL.

## 1. Clean Compile & Setup
FlexQL uses a robust C++17 architecture requiring absolutely zero external libraries. To compile the entire system dynamically into your local `bin` folder:
```bash
cd ~/Desktop/FLEXQL
make clean && make -j$(nproc)
mkdir -p data/wal data/pages data/tables
```

## 2. Running the Database (Two-Terminal Workflow)
FlexQL operates as a persistent daemon. You must run the server in one terminal and connect via the client in another.

**Terminal 1 (The Server):**
Start the database server. **You must keep this terminal open.**
```bash
./bin/flexql_server 9000
```

**Terminal 2 (The Interactive Client):**
Open a new terminal window and connect to the server:
```bash
./bin/flexql-client 127.0.0.1 9000
```
*You can type standard SQL commands here ending with a semicolon (`;`), or `.exit` to quit.*

## 3. Strict Final Submission Run Block (Verification)
To absolutely guarantee the database meets all rubric requirements, run these sequential tests from your second terminal:

**1. Run 21/21 Unit Tests (Validates SQL Logic & TTL Filters):**
```bash
./bin/flexql_benchmark --unit-test
```
*Expected Pass Condition:* `Unit Test Summary: 21/21 passed, 0 failed.`

**2. 1-Million Row Speed Benchmark:**
```bash
./bin/flexql_benchmark 1000000
```
*Expected Pass Condition:* Console reads `[PASS] INSERT benchmark complete` and logs ~800,000 throughput natively.

**3. 10-Million Row Extreme Benchmark:**
```bash
./bin/flexql_benchmark 10000000
```
*Expected Pass Condition:* Memory stays entirely stable through the disk Buffer Pool eviction cycle.

**4. Crash / Recovery Test:**
Proves the asynchronous Write-Ahead Log recovers un-flushed data during a power failure.
```bash
# In Terminal 1 (Server):
./bin/flexql_server 9000

# In Terminal 2 (Tests):
./bin/flexql_benchmark 1000000 &
sleep 1
killall -9 flexql_server

# In Terminal 1 (Restart Server):
./bin/flexql_server 9000

# In Terminal 2 (Verify Recovery):
./bin/flexql_smoke_test
```
*Expected Pass Condition:* `[PASS] survived crash` Output.

## 4. Troubleshooting & Connection Fixes

### Issue: "Cannot open FlexQL" or "failed to connect"
If the client cannot connect, the server is either not running, or the port is blocked. Run these checks:
```bash
# Check if the server process is actually alive:
pgrep -af flexql_server

# Check if port 9000 is correctly listening:
ss -ltnp | grep 9000

# View the last 80 lines of the server logs to spot errors:
tail -n 80 /tmp/flexql_server.log
```

### Issue: Server crashes instantly on boot ("Killed")
**Root Cause:** If you repeatedly run massive performance benchmarks without a clean shutdown, your `data/wal/wal.log` file will grow exponentially (upwards of 7GB+). During startup, the server tries to load this massive file into RAM for replay recovery, causing the OS to OOM-kill it.

**Fix:** Backup or wipe the massive WAL log file.
```bash
# Stop any broken servers trying to load
pkill -9 flexql_server 2>/dev/null || true

# Backup the massive WAL file so it doesn't try to load again
mv data/wal/wal.log data/wal/wal.log.bak.$(date +%s)
# (Or strictly run `rm -rf data/wal/* data/pages/* data/tables/*` if you want a clean database)

# Start server fresh
./bin/flexql_server 9000
```
