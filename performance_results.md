# FlexQL — Performance Results

## Environment
- Machine: AMD Ryzen 5 5600H, 7.1GB RAM, Linux
- Compiler: g++ with `-O3 -DNDEBUG -flto -march=native -fopenmp`
- Dataset: 10,000,000 rows
- Table schema: `ID DECIMAL, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL`
- Benchmark: `./benchmark 10000000`

## Results

| Metric | Result |
|--------|--------|
| Insert throughput | ~540,000 rows/sec |
| Memory footprint | ~495 MB RSS |
| SELECT range scan (600k rows) | ~320 ms |
| WAL overhead | <4% of insert time |
| Unit tests | 21/21 passing |

## Key Optimisations
| Technique | Impact |
|-----------|--------|
| Async WAL writer | <4% disk overhead |
| Batch INSERT parsing | Reduces TCP round trips |
| Robin Hood hash index | O(1) PK lookup |
| Numeric columnar cache + AVX2 | Fast range scans |
| LRU query cache | Repeated queries in microseconds |
| Shared mutex per table | Concurrent SELECTs |
| Binary wire protocol | Efficient data transfer |
