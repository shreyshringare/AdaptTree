# AdaptTree

A B+ tree storage engine built from scratch in C++20. Disk-backed pages, write-ahead logging, MVCC for concurrent reads, and a per-leaf learned index layer — validated by differential fuzzing against SQLite and benchmarked honestly across three key distributions.

---

## What it is

AdaptTree is a complete key-value storage engine, not a toy. Every layer was built to be correct before being made fast:

| Layer | What it does |
|-------|-------------|
| **Page** | 4 KB fixed-size pages, CRC32 checksum, 32-byte header |
| **Disk Manager** | Page-aligned `pread`/`pwrite`, POSIX-only |
| **Buffer Pool** | LRU-K eviction (K=2), pin/unpin reference counting, dirty-page tracking |
| **B+ Tree** | Insert, get, delete, range scan; ORDER=200; leaf/internal split and merge |
| **WAL** | Append-only write-ahead log, checkpoint mechanism, fsync ordering |
| **MVCC** | Timestamp-based snapshot isolation, GC of old versions past oldest active reader |
| **PGM Builder** | Greedy O(n) piecewise-linear segment fitter, ε=4 error bound |
| **Learned Index** | Per-leaf linear model stored in page header; bounds binary search to `[pred±ε]` |
| **Differential Fuzzer** | Two libFuzzer targets vs SQLite oracle — model OFF and model ON |
| **Benchmark** | Google Benchmark harness, 6 variants (model ON/OFF × 3 distributions) |

---

## The learned index

Each leaf page carries an optional `LearnedSegment{slope, intercept, error_bound}` fitted by PGMBuilder after every leaf split. On lookup:

```
predict(key) → approx_slot
search [approx_slot - ε, approx_slot + ε]   ← bounded binary search
if miss → full binary search                 ← fallback, always correct
```

The error bound `ε=4` is a hard mathematical guarantee from the greedy cone algorithm, not an empirical estimate. A wrong prediction costs extra comparisons — it cannot return a wrong answer.

### Benchmark results (Release -O2, 10,000 keys, WSL2)

| Distribution | Model | ns/op | avg\_cmp/iter | fallback/iter |
|---|---|---:|---:|---:|
| Sequential | Off | 197.5 | 5.82 | — |
| Sequential | **On** | **165.1** | **1.23** | 2.0% |
| Uniform | Off | 307.0 | 7.34 | — |
| Uniform | **On** | **287.2** | 7.38 | 5.8% |
| Zipfian | Off | 236.6 | 6.16 | — |
| Zipfian | **On** | 247.2 | 5.22 | **10.9%** |

Sequential: **−16% latency, 4.7× fewer comparisons.** Uniform: modest −6.4%. Zipfian: +4% regression — the ε=4 window is too tight for skewed within-leaf distributions; 10.9% fallback rate erodes the savings. Honest result, not filtered.

Full methodology and analysis: [`LEARNED_INDEX_RESULTS.md`](LEARNED_INDEX_RESULTS.md)

---

## Build

Requires: CMake ≥ 3.20, GCC/Clang with C++20, POSIX (Linux or WSL2).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

With benchmark:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DADAPTTREE_BENCH=ON
cmake --build build -j$(nproc)
```

---

## Tests

```bash
# Run all test binaries directly (160 tests)
for bin in build/tests/page_tests build/tests/disk_manager_tests \
           build/tests/eintr_tests build/tests/buffer_pool_tests \
           build/tests/bplus_tree_tests build/tests/wal_tests \
           build/tests/test_mvcc build/tests/pgm_builder_tests \
           build/tests/test_learned_integration \
           build/tests/test_learned_integration_gaps \
           build/tests/test_mvcc_bptree_integration \
           build/tests/test_bplustree_counters; do
  $bin
done
```

**160/160 passing.**

---

## Differential fuzzer

Requires Clang (libFuzzer is Clang-only):

```bash
cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DADAPTTREE_FUZZING=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz --target differential_fuzzer differential_fuzzer_learned

# Base tree (model off)
./build-fuzz/fuzz/differential_fuzzer fuzz/corpus/ -max_total_time=300

# Learned index (model on) — any divergence from SQLite is a learned-index bug
UBSAN_OPTIONS=silence_unsigned_overflow=1 \
  ./build-fuzz/fuzz/differential_fuzzer_learned fuzz/corpus/ -max_total_time=300
```

The two fuzzers share a decode loop (`fuzz/fuzzer_common.hpp`) and differ only in `use_learned_index_`. Divergence from the SQLite oracle calls `abort()` and saves a minimized reproducer.

---

## Benchmark

```bash
./build/bench/learned_index_bench \
  --benchmark_format=json \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

---

## Project layout

```
include/adapttree/   public headers (page, buffer_pool, bplus_tree, wal, mvcc, pgm_builder)
src/                 implementations
tests/               Google Test — one file per subsystem
fuzz/                libFuzzer harnesses + SQLite oracle + seed corpus
bench/               Google Benchmark harness
LEARNED_INDEX_RESULTS.md   measured numbers from the benchmark run
```

---

## Key design decisions

- **No external ML libraries.** PGM construction is ~50 lines of standard C++ geometry — a shrinking-cone greedy sweep. No training loop, no external dependencies.
- **Model stored in page header.** `LearnedSegment` (24 bytes) lives in a 32-byte reserved area of the leaf node. No separate index structure to keep in sync with the page.
- **Single-segment guard.** Only write the model when the fitter returns exactly 1 segment. Non-linear key distributions get no model and fall back silently to full binary search — correct and safe.
- **Correctness first.** The B+ tree is always the ground truth. The learned index only narrows the search window; a wrong or stale prediction degrades to full search, never corrupts a lookup.
- **Differential fuzzing proves correctness.** Both model-off and model-on fuzz configs run against the same SQLite oracle. Any divergence is a bug, not a measurement.

---

## References

- Ferragina & Vinciguerra, *The PGM-index: a fully-dynamic compressed learned index with provable worst-case bounds* (VLDB 2020)
- Kraska et al., *The Case for Learned Index Structures* (SIGMOD 2018)
