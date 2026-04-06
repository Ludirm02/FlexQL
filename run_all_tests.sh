#!/bin/bash
killall -9 flexql_server 2>/dev/null
mkdir -p data/wal data/pages data/tables
rm -rf data/tables/* data/wal/* data/pages/*

echo -e "\n=== STARTING SERVER ==="
./bin/flexql_server > server_all.log 2>&1 &
SERVER_PID=$!
sleep 2

echo -e "\n=== UNIT TESTS ==="
./bin/flexql_benchmark --unit-test

echo -e "\n=== 10 MILLION BENCHMARK ==="
./bin/flexql_benchmark 10000000

echo -e "\n=== CRASH TEST BENCHMARK ==="
./bin/flexql_benchmark 1000000 &
BENCH_PID=$!
sleep 1
echo "Killing Server Mid-Flight!"
kill -9 $SERVER_PID
wait $SERVER_PID 2>/dev/null
wait $BENCH_PID 2>/dev/null

echo -e "\n=== CHECKING WAL ==="
ls -lh data/wal/wal.log

echo -e "\n=== REBOOTING & RECOVERING ==="
./bin/flexql_server > server_recovery.log 2>&1 &
NEW_SERVER_PID=$!
sleep 3

./bin/crash_test

kill -9 $NEW_SERVER_PID
echo "Done!"
