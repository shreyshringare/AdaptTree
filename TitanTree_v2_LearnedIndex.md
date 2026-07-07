# TitanTree v2 — Fuzz-Tested B+ Tree Storage Engine with a Learned Index Layer

## One-line pitch

"A B+ tree storage engine built from scratch in modern C++20 — disk-backed pages, WAL-based crash recovery, MVCC for concurrent reads without blocking writes — validated not by manual testing but by differential fuzzing against SQLite's B-tree as a reference oracle. On top of that, each leaf page carries an optional learned model (a PGM-index-style piecewise-linear approximation, per Ferragina & Vinciguerra 2020, building on Kraska et al.'s 2018 'Case for Learned Index Structures') that predicts a key's position before falling back to bounded binary search — measured, not claimed, against plain binary search on the same page layout."

---

## Why this version is different from v1

v1 proved: "I can build a correct, durable, concurrent B+ tree and prove it's correct under adversarial fuzzing." That's a strong systems story on its own.

v2 adds a second, separate claim: "I can take a piece of recent database research and implement a working version of it, not just cite the paper." This is the distinction between "I read about learned indexes" and "I built one, measured it, and can tell you exactly where it wins and where it loses." Oracle's own Autonomous Database marketing leans heavily on self-tuning, ML-augmented internals — this project is a small, honest, defensible version of that same idea, built by you, with real numbers.

**Important framing for interviews:** the learned index is an *optimization layer* on an already-correct B+ tree, not a replacement for it. The B+ tree is always the ground truth; the learned model is only ever used to narrow the search range before falling back to guaranteed-correct lookup. This means a wrong prediction costs you a few extra comparisons — it can never cost you correctness. Say this proactively; it's the first question any sharp interviewer will ask ("what if the model is wrong?").

---

## Target companies

Oracle FSS / Oracle Labs (self-tuning database research is core to their current narrative), MSCI, Nvidia, Cadence, Barclays/JPMC/Deutsche Bank infra teams, any role where "implemented a paper, not just read it" is the differentiator.

---

## Architecture

```
[Client API]
    insert(key, value) / get(key) / delete(key) / range_scan(lo, hi)
        ↓
[B+ Tree Core]
    ├── Internal nodes (keys + child pointers)
    ├── Leaf nodes (keys + values, linked for range scans)
    │     └── Optional LearnedSegment (PGM-style piecewise-linear model)
    ├── Split / merge / rebalance logic
    │     └── Triggers LearnedSegment rebuild on split/merge
    └── Iterator for range scans
        ↓
[Learned Index Layer]  ← NEW IN v2
    ├── PGMBuilder — fits piecewise-linear segments over sorted keys
    │     within error bound ε at construction / rebuild time
    ├── LearnedSegment — per-leaf model: predict(key) → approx_offset
    ├── Fallback search — bounded binary search within
    │     [approx_offset - ε, approx_offset + ε], NEVER trusts
    │     the model beyond that bound
    └── Rebuild trigger — model is rebuilt (not incrementally
          updated) on leaf split, merge, or after N inserts since
          last rebuild, whichever comes first
        ↓
[Buffer Pool Manager]
    ├── Fixed-size page cache (LRU-K eviction)
    ├── Pin/unpin reference counting
    └── Dirty page tracking for flush-on-evict
        ↓
[Page Layer]
    ├── Fixed 4KB page format (header + slotted records)
    ├── Page checksum (CRC32) for corruption detection
    └── Free space management within a page
        ↓
[WAL (Write-Ahead Log)]
    ├── Append-only log of page mutations before they hit disk
    ├── Checkpoint mechanism (periodic durable snapshot)
    └── Crash recovery: replay WAL from last checkpoint
        ↓
[MVCC Layer]
    ├── Each page mutation tagged with a transaction timestamp
    ├── Readers see a consistent snapshot (no locks on read path)
    └── Garbage collection of old versions past the oldest active reader
        ↓
[Disk Manager]
    └── Raw file I/O, page-aligned reads/writes via pread/pwrite

┌──────────────────────────────────────────────────────┐
│ Fuzzing & Verification Harness                        │
│  Differential fuzzer vs SQLite (as in v1) — UNCHANGED, │
│  but now also fuzzes with learned-index lookups ON,    │
│  to catch bugs where the model's bound is violated     │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│ Learned Index Benchmark Harness  ← NEW IN v2          │
│  Runs identical workloads with learned index ON vs OFF │
│  Measures: comparisons per lookup, p50/p99 latency,    │
│  model rebuild cost, memory overhead per leaf          │
└──────────────────────────────────────────────────────┘
```

---

## The core research idea, explained simply

A leaf page stores keys in sorted order. Plain binary search over `n` keys takes `O(log n)` comparisons. A learned index replaces "guess the middle, compare, repeat" with "predict roughly where the key should be using a tiny linear model, then search a small bounded window around that prediction."

**Why this can work:** if keys are roughly uniformly distributed (or follow any learnable pattern — sequential IDs, timestamps, etc.), a straight line `position ≈ slope * key + intercept` is often a very good predictor of position. The PGM-index's contribution (over Kraska's original neural-net-heavy approach) is using a much simpler, much cheaper-to-build structure: **piecewise-linear approximation with a guaranteed error bound ε**, computed via a single linear-time greedy algorithm (Shrinking Cone / convex hull style), not gradient descent. That's what makes it practical to rebuild per-leaf on every split, instead of needing expensive offline training.

**The correctness guarantee that matters in an interview:** the model is built with an explicit error bound ε, meaning it's *mathematically guaranteed* (not just empirically likely) that the true position of any key is within ε of the predicted position. So the fallback search only ever needs to scan `[predicted - ε, predicted + ε]` — bounded, deterministic, and provably correct regardless of how bad a particular prediction is.

---

## Tech stack additions for v2

| Layer | C++ feature used | Why it matters here |
|---|---|---|
| Segment fitting | `std::vector<Point>` + greedy O(n) two-line-sweep algorithm | No external ML library needed — the PGM construction algorithm is simple geometry, not gradient descent |
| Model storage | `struct LearnedSegment { double slope; double intercept; uint16_t error_bound; }` packed into page header's reserved bytes | Keeps the model co-located with the page it describes — no separate index structure to keep in sync |
| Fallback search | Same bounded binary search as before, just given a narrower starting range | Reuses existing, already-fuzzed search code — minimizes new surface area for bugs |
| Rebuild trigger | Counter in `PageHeader` (`inserts_since_model_rebuild`) | Cheap, page-local decision — no global coordination needed |
| Benchmarking | Google Benchmark, parameterized by key distribution (uniform, sequential, zipfian) | A learned index's win is distribution-dependent — measuring only on one distribution would be dishonest |

---

## Folder structure (v2 additions marked)

```
titantree/
├── include/titantree/
│   ├── page.hpp
│   ├── buffer_pool.hpp
│   ├── node.hpp
│   ├── bplus_tree.hpp
│   ├── wal.hpp
│   ├── mvcc.hpp
│   ├── disk_manager.hpp
│   ├── learned_segment.hpp        # NEW: PGM-style model definition
│   └── pgm_builder.hpp            # NEW: greedy segment-fitting algorithm
├── src/
│   ├── ... (unchanged from v1)
│   ├── learned_segment.cpp        # NEW
│   └── pgm_builder.cpp            # NEW
├── fuzz/
│   └── differential_fuzzer.cpp    # MODIFIED: fuzzes with learned index on/off
├── bench/
│   ├── benchmark.cpp              # unchanged baseline B+ tree numbers
│   └── learned_index_bench.cpp    # NEW: model-on vs model-off comparison
├── BUGS.md
├── LEARNED_INDEX_RESULTS.md       # NEW: the v2 artifact — honest numbers
├── CMakeLists.txt
└── README.md
```

---

## Key file: `include/titantree/pgm_builder.hpp`

```cpp
#pragma once
#include <vector>
#include <cstdint>

namespace titantree {

// A single (key, position) observation used to fit the model.
struct KeyPos {
    uint64_t key;
    uint32_t position;  // slot index within the leaf
};

// The fitted model for one leaf page: predict(key) returns an
// approximate slot position, guaranteed to be within `error_bound`
// of the true position for every key the model was fit on.
struct LearnedSegment {
    double   slope    = 0.0;
    double   intercept = 0.0;
    uint16_t error_bound = 0;

    // Predicts a slot position. Caller MUST clamp the result to
    // [0, num_slots) and search [pred - error_bound, pred + error_bound] —
    // never trust this value directly as a final answer.
    uint32_t predict(uint64_t key) const {
        double p = slope * static_cast<double>(key) + intercept;
        if (p < 0) return 0;
        return static_cast<uint32_t>(p);
    }
};

// Greedy O(n) construction: a single left-to-right sweep that
// extends the current line segment as long as every point seen so
// far stays within `target_error` of the line, and starts a new
// segment the moment a point would violate the bound. This is the
// PGM-index's core contribution over Kraska et al.'s original
// neural-network approach — it's linear-time, has no training
// loop, and gives a hard mathematical error guarantee instead of
// an empirical one.
//
// For a single leaf page (a few hundred keys at most), this fits
// as ONE segment in practice; multi-segment fitting matters more
// at larger scale (e.g. fitting a whole sorted run), but the same
// algorithm is used either way for consistency.
class PGMBuilder {
public:
    explicit PGMBuilder(uint16_t target_error) : target_error_(target_error) {}

    // Fits one or more LearnedSegments over the given sorted points.
    // Returns multiple segments only if a single line can't satisfy
    // target_error_ across the whole input — which for a single 4KB
    // leaf page (≤ ~340 keys for 8-byte keys) is rare but must be
    // handled, not assumed away.
    std::vector<LearnedSegment> fit(const std::vector<KeyPos>& sorted_points) const;

private:
    uint16_t target_error_;
};

}  // namespace titantree
```

---

## Key file: `src/pgm_builder.cpp` — the actual fitting algorithm

```cpp
#include "titantree/pgm_builder.hpp"
#include <algorithm>
#include <cmath>

namespace titantree {

// Shrinking-cone greedy algorithm: maintains an upper and lower
// bounding line as points are added; a new segment starts the
// moment the cone formed by these two lines becomes empty (no
// single line can pass within target_error of every point seen
// so far AND the new point).
//
// This is intentionally the simplest correct version of the PGM
// construction algorithm, not the most optimized one — for
// per-leaf-page fitting (small n), simplicity and obvious
// correctness matter more than shaving constant factors, since
// this code runs on every split/merge and any bug here directly
// produces a model that violates its own error bound, which is
// the one thing this whole layer is not allowed to do silently.
std::vector<LearnedSegment> PGMBuilder::fit(
    const std::vector<KeyPos>& sorted_points) const {

    std::vector<LearnedSegment> segments;
    if (sorted_points.empty()) return segments;

    size_t start = 0;
    while (start < sorted_points.size()) {
        // Initialize cone with first two points (or just one if
        // this is the last point in the input).
        double lower_slope = -std::numeric_limits<double>::infinity();
        double upper_slope =  std::numeric_limits<double>::infinity();
        size_t end = start;

        uint64_t origin_key = sorted_points[start].key;
        uint32_t origin_pos = sorted_points[start].position;

        for (size_t i = start; i < sorted_points.size(); ++i) {
            double dx = static_cast<double>(sorted_points[i].key) - origin_key;
            if (dx == 0) { end = i; continue; } // duplicate key, same segment

            double y_lo = sorted_points[i].position - target_error_;
            double y_hi = sorted_points[i].position + target_error_;

            double slope_lo = (y_lo - origin_pos) / dx;
            double slope_hi = (y_hi - origin_pos) / dx;

            double new_lower = std::max(lower_slope, slope_lo);
            double new_upper = std::min(upper_slope, slope_hi);

            if (new_lower > new_upper) {
                // Cone is empty — this point can't join the current
                // segment within target_error. Close the segment here.
                break;
            }
            lower_slope = new_lower;
            upper_slope = new_upper;
            end = i;
        }

        double chosen_slope = (lower_slope + upper_slope) / 2.0;
        if (std::isinf(chosen_slope)) chosen_slope = 0.0; // single-point segment

        LearnedSegment seg;
        seg.slope = chosen_slope;
        seg.intercept = origin_pos - chosen_slope * origin_key;
        seg.error_bound = target_error_;
        segments.push_back(seg);

        start = end + 1;
    }

    return segments;
}

}  // namespace titantree
```

---

## Key file: `include/titantree/bplus_tree.hpp` — integration point (modified from v1)

```cpp
// Inside the leaf-search path:

std::optional<uint32_t> findSlotInLeaf(const Page& leaf, const Key& key) const {
    if (leaf.hasLearnedSegment()) {
        const LearnedSegment& model = leaf.learnedSegment();
        uint32_t predicted = model.predict(key);
        uint32_t lo = predicted > model.error_bound ? predicted - model.error_bound : 0;
        uint32_t hi = std::min<uint32_t>(predicted + model.error_bound,
                                          leaf.header().num_slots - 1);

        // Bounded search ONLY within the model's guaranteed window —
        // this is the line that makes the whole layer safe: even if
        // the model is stale (rebuild hasn't run since recent inserts)
        // or simply wrong, this is still a correct binary search,
        // just over a smaller range than the full page.
        auto result = boundedBinarySearch(leaf, key, lo, hi);
        if (result.has_value()) return result;

        // Defense in depth: if the bounded search somehow misses
        // (e.g. a rebuild is overdue and error has grown beyond
        // error_bound), fall back to a full-page binary search
        // rather than returning "not found" incorrectly. This path
        // should be rare in practice — its hit rate is itself a
        // metric worth reporting in LEARNED_INDEX_RESULTS.md.
        return fullBinarySearch(leaf, key);
    }
    return fullBinarySearch(leaf, key);
}
```

This fallback-of-a-fallback is the detail to lead with when an interviewer asks "what if the model is wrong" — the answer isn't "it can't be wrong," it's "being wrong costs a few extra comparisons, never a wrong answer, and I measured how often that fallback actually triggers."

---

## `bench/learned_index_bench.cpp` — what you actually measure

```cpp
// Runs the SAME lookup workload against the SAME tree with the
// learned index toggled on vs off, across three key distributions:
//   1. Sequential keys (best case for a linear model)
//   2. Uniform random keys (realistic, moderate case)
//   3. Zipfian / skewed keys (worst case — tests how badly a
//      single linear model degrades on non-uniform data, and
//      whether multi-segment fitting kicks in correctly)
//
// Reports: avg comparisons per lookup, p50/p99 latency, fallback-
// to-full-search rate, and per-leaf memory overhead of storing
// the model (slope + intercept + error_bound = ~20 bytes).
//
// This is the file whose output becomes LEARNED_INDEX_RESULTS.md.
// Every number in that file must come from running this benchmark,
// not from estimation.
```

---

## `LEARNED_INDEX_RESULTS.md` — the v2 artifact

```markdown
# Learned Index: measured results vs plain binary search

## Methodology
[Describe exact benchmark setup: page fill factor, key count,
hardware, number of trials, warm-up runs excluded]

## Sequential keys (best case)
- Avg comparisons/lookup: binary search = X.X, learned = X.X (-XX%)
- p50 latency: ...
- p99 latency: ...
- Fallback-to-full-search rate: X%

## Uniform random keys
- [same metrics]

## Zipfian/skewed keys (worst case)
- [same metrics — report honestly even if the learned index loses here]

## Model overhead
- Memory per leaf: ~20 bytes (slope + intercept + error_bound)
- Rebuild cost: avg X microseconds per rebuild, triggered Y times
  during the benchmark run

## Honest conclusion
[State plainly where this wins, where it doesn't, and why — e.g.
"the learned index reduces comparisons by ~40% on sequential and
uniform keys, but on highly skewed Zipfian keys the single-segment
model's error grows large enough that the fallback search triggers
on X% of lookups, eroding most of the benefit. Multi-segment fitting
would likely help here but wasn't required to stay within this
project's error bound at this leaf size."]
```

This honesty — including the case where it *doesn't* win — is what separates "I implemented a paper" from "I implemented a paper and understand its actual tradeoffs," which is the thing interviewers are actually testing for.

---

## `CLAUDE.md` additions for v2

```markdown
## v2 addition: Learned Index Layer

### What it is
A PGM-index-style piecewise-linear model per leaf page that
predicts approximate key position, used to narrow the search
range before falling back to bounded binary search. Never the
sole source of truth — correctness always falls back to a real
search within a mathematically guaranteed error window.

### Key constraints (v2-specific)
- The learned index MUST NEVER be allowed to return "not found"
  for a key that exists — every lookup path must fall back to
  fullBinarySearch() if the bounded window search comes up empty
- error_bound MUST be respected exactly — PGMBuilder's fit()
  must be tested to confirm every point is within error_bound of
  its predicted position, not just "usually" within bound
- Model rebuild MUST be triggered on every leaf split/merge —
  a stale model after a structural change is the most likely
  source of correctness bugs in this layer
- LEARNED_INDEX_RESULTS.md numbers MUST come from
  bench/learned_index_bench.cpp output — never estimated or
  rounded up to look better
- The differential fuzzer (fuzz/differential_fuzzer.cpp) MUST run
  with the learned index both ON and OFF as separate fuzzing
  configurations — bugs in the learned path won't surface if only
  the baseline path is fuzzed

### Commands (v2 additions)
- Learned index benchmark: `./build/bench/learned_index_bench`
- Fuzz with learned index forced on: `./build/differential_fuzzer -learned_index=on fuzz/corpus/`
```

---

## Interview questions and exact answers (v2-specific)

**"What happens if the learned model's prediction is wrong?"**
"The model only ever narrows the search window — it predicts a position and I search `[predicted - ε, predicted + ε]`, where ε is a hard mathematical error bound the PGM construction algorithm guarantees at fit time, not an empirical estimate. If that bounded search somehow still misses — which would mean the model went stale, say after inserts since the last rebuild — I fall back to a full binary search over the whole page. So a bad prediction costs extra comparisons, it can never cost correctness. I measured how often that fallback actually triggers, and reported it honestly in my results even where it hurt the headline number."

**"Why piecewise-linear instead of a neural network like the original Kraska paper?"**
"Kraska's original paper used small neural nets per index level, which gives strong compression but needs offline training and isn't cheap to rebuild. PGM-index's contribution was showing a much simpler approach — a single greedy linear-time sweep that fits piecewise-linear segments with a guaranteed error bound — gets most of the benefit at a fraction of the construction cost. That matters here specifically because I rebuild the model on every leaf split, so it has to be cheap enough to run inline, not as a background training job."

**"Where does this actually lose to plain binary search?"**
"On skewed key distributions — I tested Zipfian keys specifically because that's the realistic worst case, and a single linear segment doesn't fit skewed data well, so the fallback-to-full-search rate goes up and erodes most of the benefit. I reported that number rather than only showing the sequential/uniform case where it looks best."

**"Why fit per-leaf-page rather than one global model for the whole tree?"**
"Per-leaf keeps the model trivially consistent with the page it describes — when a leaf splits, only that leaf's model needs rebuilding, not a tree-wide model. A global model would need expensive re-fitting on every structural change anywhere in the tree, which doesn't fit the cost budget of an operation that has to stay cheap enough to run on every split."

---

## Updated build order (v2 additions in bold)

1. Page + DiskManager (3-4 days)
2. BufferPool with PageGuard (3-4 days)
3. B+ tree insert/get only, no delete (1 week)
4. Add differential fuzzer against SQLite (2-3 days)
5. B+ tree delete with merge/rebalance (1 week)
6. WAL + crash recovery (1 week)
7. MVCC (1 week)
8. **PGMBuilder + LearnedSegment, unit-tested in isolation against synthetic key sets (3-4 days)** — verify the error-bound guarantee holds before integrating into the tree at all
9. **Integrate learned index into leaf search path with full fallback chain (2-3 days)** — get correctness right before measuring performance
10. **Re-run the differential fuzzer with learned index ON as a separate fuzzing configuration (2-3 days)** — this is where v2-specific bugs will actually surface
11. **Build and run learned_index_bench.cpp across all three key distributions, write LEARNED_INDEX_RESULTS.md honestly (2-3 days)**

**Total realistic time: 7-9 weeks** (v1's 5-7 weeks + 1.5-2 weeks for the learned index layer, fuzzing it, and benchmarking it properly). Don't compress the benchmarking step — a learned index project with no honest performance data is just an unused feature, not a research contribution.

---

That's the full v2 spec. Paste this into a new chat and say "build TitanTree v2" — start at step 8 if v1's core tree, fuzzer, WAL, and MVCC are already done; start at step 1 if building from scratch.
