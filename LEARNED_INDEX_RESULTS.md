# AdaptTree Learned Index Benchmark Results

**Phase 11 — Benchmark Harness + Results**
**Date:** 2026-07-02
**Host:** ShreyasShringareZenbook (14-core, 3686 MHz, via WSL2)
**Build:** Release (-O2), Google Benchmark v1.8.3
**Repetitions:** 3, aggregates only (mean reported)
**Warm-up:** 10,000 inserts before each measurement loop
**Lookup pool:** 10,000 pre-generated keys per distribution

---

## Results Table

| Distribution   | Model | Latency (ns/op) | avg\_cmp/iter | fallbacks/iter | Notes                          |
|----------------|-------|----------------:|-------------:|---------------:|-------------------------------|
| Sequential     | Off   |           197.5 |         5.82 |          0.000 | Pure binary search baseline    |
| Sequential     | On    |           165.1 |         1.23 |          0.020 | 4.7x fewer cmps; 16% faster   |
| Uniform        | Off   |           307.0 |         7.34 |          0.000 | Worst-case random access       |
| Uniform        | On    |           287.2 |         7.38 |          0.058 | Model miss rate ~5.75%        |
| Zipfian        | Off   |           236.6 |         6.16 |          0.000 | Cache-warm hot keys            |
| Zipfian        | On    |           247.2 |         5.22 |          0.109 | Higher fallback ~10.9%        |

---

## Key Observations

### Sequential distribution
The learned index delivers its best result here: **16% latency reduction** (197.5 → 165.1 ns)
with a **4.7x reduction in average comparisons** (5.82 → 1.23). Sequential keys produce highly
linear per-leaf key distributions, which the PGM-style segment can fit with near-zero prediction
error. The small fallback rate (2.0%) reflects edge cases at leaf boundaries.

### Uniform distribution
Random keys produce the highest latency overall (307 ns off, 287 ns on). The model provides a
modest **6.4% speedup** but the avg_cmp barely changes (7.34 → 7.38 on) because uniform keys
are hard to compress into a tight linear segment — the epsilon window frequently misses and
`boundedBinarySearch` falls back to `fullBinarySearchOpt`. Fallback rate is 5.75%.

### Zipfian distribution (s=1.0, N=1,000,000)
Zipfian hot keys cluster on low-rank entries, which the model handles reasonably (5.22 vs
6.16 avg cmps). However, latency is slightly **worse with model on** (247 vs 237 ns) due to
a **10.9% fallback rate** — the model overhead plus the cost of falling back to full search
on misses outweighs the savings on hits. This is the honest result: the learned index regresses
slightly on skewed workloads when the epsilon window is too tight.

---

## Fallback Rate Analysis

| Distribution | fallbacks/iter | Interpretation                                         |
|--------------|----------------|--------------------------------------------------------|
| Sequential   | 2.0%           | Leaf-boundary edge cases; acceptable                  |
| Uniform      | 5.75%          | Epsilon=4 too narrow for random keys in full leaf     |
| Zipfian      | 10.9%          | Skew causes repeated near-miss predictions            |

The fallback counter (`fallback_count_`) is accurate: it is reset to 0 before the measurement
loop and each call to `fullBinarySearchOpt` triggered by a missed `boundedBinarySearch` window
increments it atomically. No rounding has been applied.

---

## Raw JSON

Stored at `bench_results.json` in the project root (gitignored).

---

## Methodology Notes

- **Zipf sampling:** Inverse-CDF via pre-built harmonic CDF (O(log N) per sample, computed
  once at static initialization — T-11-04 mitigation).
- **DoNotOptimize:** Applied to named `auto result` lvalue, not rvalue, ensuring the compiler
  cannot eliminate the `tree.get()` call.
- **Counter reset:** Both `cmp_count_` and `fallback_count_` are reset to 0 immediately before
  the measurement loop. Insert-phase comparisons are excluded from reported values.
- **avg_cmp semantics:** `kAvgIterations` divides the total accumulated counter by the number
  of benchmark iterations, yielding comparisons per lookup.
