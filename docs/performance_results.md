# FlexQL Performance Results

## Environment

- Date: March 19, 2026
- Machine: local development machine (Linux)
- Build: `make -j$(nproc)`
- Script: `./scripts/run_benchmark.sh <port> <rows> <out-file>`
- Assignment-scale command: `./scripts/run_benchmark.sh 9000 10000000`

## Raw Benchmark Output (Latest Post-Optimization Runs)

### 100K rows

```text
rows_inserted=10000000
insert_total_seconds=19.5357
insert_rows_per_second=511884
point_queries=5000
point_query_total_seconds=0.147789
point_query_avg_ms=0.0295577
full_scan_rows_returned=5000000
full_scan_seconds=1.43346
cached_query_rows_first=2000000
cached_query_rows_second=2000000
cached_query_first_seconds=0.649223
cached_query_second_seconds=0.534223

```

### 1M rows

```text
rows_inserted=1000000
insert_total_seconds=1.66257
insert_rows_per_second=601478
point_queries=5000
point_query_total_seconds=0.130255
point_query_avg_ms=0.026051
full_scan_rows_returned=500000
full_scan_seconds=0.296228
cached_query_rows_first=200000
cached_query_rows_second=200000
cached_query_first_seconds=0.0603823
cached_query_second_seconds=0.0208144
```

## Notes

- Point-query timings reflect primary-index lookup (`WHERE id = ...`).
- Insert throughput improved significantly due larger batched multi-row insert statements and reduced insert-side numeric re-parsing.
- Full-scan/cached query timings improved from lower-allocation protocol serialization/parsing and numeric fast-path execution.
- For submission, run and attach the full 10M output file from `scripts/run_benchmark.sh`.
