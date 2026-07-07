# AdaptTree

> **A complete key-value storage engine built from scratch in C++20** — disk-backed pages, write-ahead logging, MVCC snapshot isolation, and a per-leaf learned index layer that cuts lookup latency by 16% on sequential workloads.
> Validated by differential fuzzing against SQLite. **160/160 tests passing.**

---

## Highlights

| Metric | Value |
|---|---|
| Lines of C++ (src + include) | ~10,000 |
| Test cases | **160 / 160 passing** |
| Lookup latency improvement (sequential keys) | **−16%** (197 → 165 ns/op) |
| Comparison reduction (sequential, learned index ON) | **4.7×** (5.82 → 1.23 avg cmps/lookup) |
| Fuzz validation | Two libFuzzer targets vs. live SQLite oracle |
| Build system | CMake ≥ 3.20, C++20, zero runtime dependencies |

---

## What I Built

AdaptTree is a from-scratch, layered storage engine. Every layer was built to be **correct before being made fast**, and each one is independently unit-tested:

```
┌─────────────────────────────────────────────────────────────┐
│                        Public API                           │
│              BPlusTree<K,V>  ·  MVCC<K,V>                   │
├──────────────────────────────┬──────────────────────────────┤
│       Learned Index          │          WAL                  │
│  PGM segment per leaf page   │  Append-only, fsync-ordered   │
├──────────────────────────────┴──────────────────────────────┤
│                   B+ Tree (ORDER = 200)                     │
│        insert / get / delete / range scan                   │
│        leaf + internal split and merge                      │
├─────────────────────────────────────────────────────────────┤
│                     Buffer Pool                             │
│        LRU-K (K=2) eviction · pin/unpin · dirty tracking   │
├─────────────────────────────────────────────────────────────┤
│                     Disk Manager                            │
│        Page-aligned pread/pwrite · POSIX · EINTR retry      │
├─────────────────────────────────────────────────────────────┤
│                       Page Layer                            │
│        4 KB fixed pages · CRC32 checksum · 32-byte header   │
└─────────────────────────────────────────────────────────────┘
```

| Layer | Implementation Notes |
|---|---|
| **Page** | 4 KB fixed-size pages, CRC32 integrity checksum, 32-byte header |
| **Disk Manager** | Page-aligned `pread`/`pwrite`, POSIX, full EINTR retry loop |
| **Buffer Pool** | LRU-K eviction (K=2), pin/unpin reference counting, dirty-page tracking |
| **B+ Tree** | Insert, get, delete, range scan; ORDER=200; leaf/internal split and merge |
| **WAL** | Append-only write-ahead log, checkpoint mechanism, `fsync` ordering |
| **MVCC** | Timestamp-based snapshot isolation, GC of old versions past oldest active reader |
| **PGM Builder** | Greedy O(n) piecewise-linear segment fitter, ε=4 mathematical error bound |
| **Learned Index** | Per-leaf linear model stored in the page header itself (24 bytes); bounds binary search to `[pred ± ε]` |

---

## The Learned Index

Each leaf page carries an optional `LearnedSegment{slope, intercept, error_bound}` fitted by PGMBuilder after every leaf split. On lookup:

```
predict(key) → approx_slot
search [approx_slot − ε, approx_slot + ε]   ← bounded binary search
if miss → full binary search                 ← fallback, always correct
```

The error bound `ε=4` is a **hard mathematical guarantee** from the greedy shrinking-cone algorithm — not an empirical estimate. A misprediction costs extra comparisons; it **cannot return a wrong answer**.

The model (24 bytes) is stored in the 32-byte reserved area of the leaf page header — no separate index structure, no synchronization problem.

### Measured Results (Release -O2, 10,000 keys, WSL2)

| Distribution | Model | ns/op | avg\_cmp/iter | fallback rate |
|---|---|---:|---:|---:|
| Sequential | Off | 197.5 | 5.82 | — |
| Sequential | **On** | **165.1** | **1.23** | 2.0% |
| Uniform | Off | 307.0 | 7.34 | — |
| Uniform | **On** | **287.2** | 7.38 | 5.8% |
| Zipfian | Off | 236.6 | 6.16 | — |
| Zipfian | On | 247.2 | 5.22 | 10.9% |

**Sequential: −16% latency, 4.7× fewer comparisons.**
**Zipfian: +4% regression** — the ε=4 window is too tight for skewed within-leaf distributions; the 10.9% fallback rate erodes the savings. This is the honest result, not a filtered one.

Full methodology: [`LEARNED_INDEX_RESULTS.md`](LEARNED_INDEX_RESULTS.md)

---

## Key Engineering Decisions

- **No external ML libraries.** PGM construction is ~50 lines of standard C++ geometry (shrinking-cone greedy sweep). No training loop, no dependency.
- **Model lives in the page header.** `LearnedSegment` (24 bytes) uses the 32-byte reserved area in each leaf node. No separate index to keep in sync; the model is inherently co-located with the data it indexes.
- **Single-segment guard.** A model is only written when the fitter returns exactly 1 segment. Non-linear distributions fall back silently to full binary search — safe by construction.
- **Correctness first.** The B+ tree is always the ground truth. The learned index only narrows the search window; a stale or wrong prediction degrades gracefully to full search, never corrupts.
- **Differential fuzzing proves correctness.** Both the model-off and model-on fuzz configs run the same operation stream against a live SQLite oracle. Any divergence triggers `abort()` and saves a minimized reproducer.

---

## Testing

19 test files, one per subsystem or integration boundary:

| Suite | Coverage |
|---|---|
| `test_page` | CRC32, page serialization, header round-trips |
| `test_disk_manager` | pread/pwrite correctness, alignment |
| `test_eintr` | EINTR retry correctness |
| `test_buffer_pool` | LRU-K eviction, pin/unpin, dirty tracking |
| `test_bplus_tree` | Full CRUD, range scan, split/merge |
| `test_wal` | Append, checkpoint, fsync ordering |
| `test_wal_crash_recovery` | Crash/recovery scenarios |
| `test_mvcc` | Snapshot isolation, concurrent readers, GC |
| `test_mvcc_bptree_integration` | MVCC + B+ tree together |
| `test_mvcc_full_integration` | Full-stack MVCC integration |
| `test_pgm_builder` | Segment fitting, cone algorithm |
| `test_learned_integration` | Learned index end-to-end correctness |
| `test_learned_integration_gaps` | Boundary conditions, non-linear distributions |
| `test_bplustree_counters` | cmp\_count and fallback\_count instrumentation |

```bash
# Run all 160 tests
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

**160 / 160 passing.**

---

## Differential Fuzzer

Two libFuzzer targets share a decode loop ([`fuzz/fuzzer_common.hpp`](fuzz/fuzzer_common.hpp)) and differ only in `use_learned_index_`. Both replay the same byte-encoded operation stream against a live in-process SQLite database. Any divergence calls `abort()` and saves a minimized reproducer.

```bash
# Build (requires Clang)
cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DADAPTTREE_FUZZING=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz --target differential_fuzzer differential_fuzzer_learned

# Base tree (model off)
./build-fuzz/fuzz/differential_fuzzer fuzz/corpus/ -max_total_time=300

# Learned index (model on)
UBSAN_OPTIONS=silence_unsigned_overflow=1 \
  ./build-fuzz/fuzz/differential_fuzzer_learned fuzz/corpus/ -max_total_time=300
```

---

## Build

**Requirements:** CMake ≥ 3.20, GCC or Clang with C++20, Linux or WSL2 (POSIX `pread`/`pwrite`).

```bash
# Tests only
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# With benchmark harness
cmake -B build -DCMAKE_BUILD_TYPE=Release -DADAPTTREE_BENCH=ON
cmake --build build -j$(nproc)

# Run benchmark
./build/bench/learned_index_bench \
  --benchmark_format=json \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

---

## Project Layout

```
include/adapttree/         Public headers: page, buffer_pool, bplus_tree, wal, mvcc, pgm_builder
src/                       Implementations (one .cpp per header)
tests/                     Google Test suites — one file per subsystem
fuzz/                      libFuzzer harnesses, SQLite oracle, seed corpus
bench/                     Google Benchmark harness (6 variants × 3 distributions)
LEARNED_INDEX_RESULTS.md   Full benchmark methodology and measured numbers
bench_results.json         Raw Google Benchmark JSON output
```

---

## References

- Ferragina & Vinciguerra, *The PGM-index: a fully-dynamic compressed learned index with provable worst-case bounds* (VLDB 2020)
- Kraska et al., *The Case for Learned Index Structures* (SIGMOD 2018)
