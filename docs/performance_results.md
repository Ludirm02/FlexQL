# FlexQL Performance Results

## Environment

- Date: March 11, 2026
- Machine: local development machine (Linux)
- Build: `make -j$(nproc)`
- Server: `./build/flexql_server 9000`
- Benchmark (assignment-scale default): `./scripts/run_benchmark.sh 9000 10000000`
- Benchmark (sample run captured below): `./build/flexql_benchmark 127.0.0.1 9000 10000`

## Raw Benchmark Output (Post-Optimization Sample, 10K Rows)

```text
rows_inserted=10000
insert_total_seconds=0.0567577
insert_rows_per_second=176188
point_queries=5000
point_query_total_seconds=0.214633
point_query_avg_ms=0.0429267
full_scan_rows_returned=5000
full_scan_seconds=0.00571581
cached_query_rows_first=2000
cached_query_rows_second=2000
cached_query_first_seconds=0.00300211
cached_query_second_seconds=0.00161521
```

## Notes

- Point-query timings reflect primary-index usage (`WHERE id = ...`).
- Repeated cached query is faster than first execution, showing cache benefit.
- Insert throughput improved substantially after enabling multi-row batched INSERT execution in the benchmark client.
- Full-scan class queries improved due numeric typed fast-paths and numeric range index usage.
- `scripts/run_benchmark.sh` now defaults to `10,000,000` rows to match assignment-scale evaluation.
