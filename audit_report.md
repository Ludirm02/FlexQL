# FlexQL Project Audit Report

Based on a strict review of your codebase (`FLEXQL`), the design doc (`design.md`), the PDF requirements, and specifically the **explicit TA communications**, here is a comprehensive list of flaws that will likely cause issues during evaluation.

## 🔴 1. Critical Failure: "Disk-Based" is Still Actually "In-Memory" (Fails the 1TB Test)

The TA explicitly warned you: *“if 1tb data is inserted on the db. then how you handle this you don't have this much ram.”* 

While you wrote `DiskStore::append_rows` to save a binary file to disk, **your engine still requires all data to fit in RAM**. 
If you look at `SqlEngine::load_from_disk()` and `SqlEngine::execute_insert()`, you are pushing every single row into `table.rows` (which is a `std::vector<Row>`). When `execute_select()` runs, it loops over `base.rows.size()`. 
The `BufferPoolManager` you added is only appending pages; queries don't fetch missed pages from the buffer pool—they just read from the RAM vector. If the benchmark attempts to insert data beyond your RAM limit, `std::vector::reserve` will trigger an **OOM (Out of Memory) crash**, instantly failing the TA's threshold.

**To fix this for strict evaluation:** You need true disk-based B-tree or Heap file integration. Queries must resolve from `BufferPoolManager->FetchPage(page_id)` with an eviction policy (like LRU clock), rather than scanning an unbounded in-memory `std::vector<Row>`.

## 🔴 2. Critical Data Corruption Bug: Double Insertions on Crash Recovery

In `server_main.cpp`, you have an architectural conflict between your new `.bin` storage and your old `WAL` implementation:
1. `DiskStore::AsyncWriter` constantly appends to `.bin` in the background.
2. Synchronously, you `WAL::instance().log(sql)` every INSERT into `wal.log`.
3. If the server is killed, on restart, you first call `engine.load_from_disk()` (loading all rows from `.bin`).
4. Immediately after, you call `wal.replay(wal_path)`.

**The Bug:** The `wal.log` is never truncated during runtime, only on startup. If you insert 10 million rows, both the `.bin` and the `wal.log` will contain 10 million rows. If the server crashes mid-benchmark, your startup sequence loads 10M rows from `.bin` entirely, and then the WAL replays the exact same 10M `INSERT` queries, resulting in **20 million rows (duplicates)**. 

## 🟠 3. Partial Requirement Miss: JOIN Comparisons on Strings

The problem update states: *“The SQL conditions supported in WHERE and JOIN clauses should include the following operators: `=`, `>`, `<`, `>=`, `<=`.”*

In `SqlEngine::execute_select` (around line 1250), your non-equality JOIN implementation works by parsing both `lhs` and `rhs` as `double` via `fast_parse_double()`. If either value fails to parse as a number (`!lhs_ok || !rhs_ok`), you execute `continue;` and skip the row.
**The Flaw:** If the evaluation script tries to do a non-equality JOIN on `VARCHAR` columns (e.g., `ON tableA.name > tableB.name`), your database will silently return 0 rows. It fails to fall back to `std::string` lexicographical comparisons.

## 🟠 4. Design Document Inconsistencies

Your `design.md` was not updated to reflect the TA's rejection of your old architecture. 
In `design.md` Section 8.1, you wrote:
> *"Trade-off: RAM is used as primary working storage for fast query performance, with WAL providing durability. This follows the approach used by Redis (AOF mode) and early PostgreSQL designs."*

The TA already told you via WhatsApp that this specific trade-off is **unacceptable** for this assignment if it leads to OOM. Submitting a design document that highlights an architecture the TA explicitly called out as problematic will lead to heavily penalized marks.

## 🟡 5. Unsafe Batch Memory Reservations

In `execute_insert`, to accommodate batch parsing, you calculate `needed_rows_capacity` and double the vector sizes:
```cpp
    std::size_t new_capacity = std::max<std::size_t>(1024, table.rows.capacity() * 2);
    while (new_capacity < needed_rows_capacity) {
        new_capacity *= 2;
    }
```
If an evaluator runs a massive benchmark, this `* 2` doubling will attempt to allocate massive contiguous memory chunks, triggering early OOM crashes long before RAM is actually full, due to memory fragmentation.

## Summary Checklist Against Requirements

- [x] Client C/C++ APIs implemented (`flexql_open`, `flexql_close`, etc.).
- [x] Multithreading (thread pool and concurrent readers implemented).
- [x] Expiration timestamp logic.
- [x] Single-condition WHERE logic / No AND/OR limitations.
- [ ] **Disk Persistence Constraints** (Failed: Still reliant fully on bounded RAM).
- [ ] **Data Correctness** (Failed: WAL logic causes duplicate entries on crash).
- [ ] **String JOIN operations** (Failed: Silent drop of string non-equality JOINs).
