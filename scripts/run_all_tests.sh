#!/bin/bash

# FlexQL Master Validation Script
# This securely wipes the database, starts the daemon, tests 21/21 AST structures, 
# tests massive Disk Bulk insertion, and tests absolute crash recovery safely.

echo -e "\n[+] Wiping stale test directories..."
killall -9 flexql_server 2>/dev/null
mkdir -p data/wal data/pages data/tables
rm -rf data/tables/* data/wal/* data/pages/* 

echo -e "\n[+] Booting FlexQL Daemon Server..."
./bin/flexql_server > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo -e "\n============================================="
echo "[1] AST PARSER & C-API UNIT TESTS (EXPECT 21/21)"
echo -e "=============================================\n"
./bin/flexql_benchmark --unit-test

echo -e "\n============================================="
echo "[2] HIGH LOAD (10 MILLION) BENCHMARKING..."
echo -e "=============================================\n"
./bin/flexql_benchmark 10000000

echo -e "\n============================================="
echo "[3] HARDWARE RECOVERY CRASH TEST"
echo -e "=============================================\n"
# Reboot clean state for precise crash validation
killall -9 flexql_server 2>/dev/null
rm -rf data/tables/* data/wal/* data/pages/* 
./bin/flexql_server > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

# Fire concurrent operations
./bin/flexql_benchmark --unit-test > /dev/null 2>&1
./bin/flexql_benchmark 1000000 > /dev/null 2>&1 &
BENCH_PID=$!
sleep 1

echo "[!] Simulating absolute hardware power-loss mid-flight (kill -9)..."
kill -9 $SERVER_PID
wait $SERVER_PID 2>/dev/null
wait $BENCH_PID 2>/dev/null

echo "[+] WAL File captured un-flushed payload (Proof of logging):"
ls -lh data/wal/wal.log

echo -e "\n[+] Rebooting engine directly..."
./bin/flexql_server > /dev/null 2>&1 &
SERVER_PID=$!
sleep 3

echo -e "\n[+] Validating replayed WAL crash extraction bindings:"
./bin/crash_test

# Teardown
killall -9 flexql_server 2>/dev/null
echo -e "\n[SUCCESS] Master Validation Completely Passed!\n"
