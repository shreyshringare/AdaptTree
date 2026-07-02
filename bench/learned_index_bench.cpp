// learned_index_bench.cpp — Phase 11 Benchmark Harness
// 6 variants: Sequential/Uniform/Zipfian x ModelOff/ModelOn
// Measures ns/op, avg comparisons, and fallback rate.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "adapttree/bplus_tree.hpp"
#include "adapttree/wal.hpp"

namespace adapttree {
extern template class BPlusTree<NullWAL>;
}  // namespace adapttree

// ─────────────────────────────────────────────────────────────────────────────
// Named constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint64_t kWarmupInserts = 10'000;
static constexpr uint64_t kLookupPoolSize = 10'000;
static constexpr int      kPoolPages      = 512;
static constexpr uint64_t kZipfN          = 1'000'000;
static constexpr double   kZipfSkew       = 1.0;
static constexpr uint64_t kKeyStride      = 1000ULL;
static constexpr uint64_t kRngSeedInsert  = 42;
static constexpr uint64_t kRngSeedLookup  = 99;
static constexpr uint64_t kRngSeedZipf    = 123;

// ─────────────────────────────────────────────────────────────────────────────
// Key generators (pre-computed to avoid in-loop distribution overhead)
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint64_t> gen_sequential_keys(uint64_t n) {
    std::vector<uint64_t> keys(n);
    for (uint64_t i = 0; i < n; ++i) keys[i] = i * kKeyStride;
    return keys;
}

static std::vector<uint64_t> gen_uniform_keys(uint64_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> uid(0, UINT64_MAX);
    std::vector<uint64_t> keys(n);
    for (auto& k : keys) k = uid(rng);
    return keys;
}

// Zipf inverse-CDF sampling: P(k) ∝ 1/k^s, s = kZipfSkew
// CDF is pre-built once so per-key cost is O(log kZipfN), not O(kZipfN).
static std::vector<uint64_t> gen_zipfian_keys(uint64_t n) {
    // Build normalized CDF over [1, kZipfN]
    std::vector<double> cdf(kZipfN + 1, 0.0);
    double H = 0.0;
    for (uint64_t k = 1; k <= kZipfN; ++k) H += 1.0 / k;  // s=1 harmonic number
    double cumul = 0.0;
    for (uint64_t k = 1; k <= kZipfN; ++k) {
        cumul += 1.0 / static_cast<double>(k) / H;
        cdf[k] = cumul;
    }

    std::mt19937_64 rng(kRngSeedZipf);
    std::uniform_real_distribution<double> udist(0.0, 1.0);
    std::vector<uint64_t> keys(n);
    for (auto& key : keys) {
        double u = udist(rng);
        // Binary search on CDF (O(log kZipfN) per sample)
        uint64_t lo = 1, hi = kZipfN;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (cdf[mid] < u) lo = mid + 1;
            else              hi = mid;
        }
        key = lo * kKeyStride;
    }
    return keys;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tree environment (RAII wrapper)
// ─────────────────────────────────────────────────────────────────────────────

struct TreeEnv {
    std::string                             tmppath;
    NullWAL                                 wal;
    adapttree::DiskManager                  dm;
    adapttree::BufferPool<NullWAL>          pool;
    adapttree::BPlusTree<NullWAL>           tree;

    TreeEnv()
        : tmppath(make_tmp())
        , dm(tmppath)
        , pool(&dm, &wal, static_cast<size_t>(kPoolPages))
        , tree(pool, wal)
    {}

    ~TreeEnv() {
        std::error_code ec;
        std::filesystem::remove(tmppath, ec);
    }

    static std::string make_tmp() {
        char buf[] = "/tmp/bench_XXXXXX";
        int fd = ::mkstemp(buf);
        if (fd >= 0) ::close(fd);
        return buf;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Core benchmark function
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Lookup(benchmark::State& state, bool learned,
                      const std::vector<uint64_t>& insert_keys,
                      const std::vector<uint64_t>& lookup_keys) {
    TreeEnv env;

    // Warm-up: insert all keys before measurement loop
    for (uint64_t i = 0; i < insert_keys.size(); ++i) {
        env.tree.insert(insert_keys[i], i);
    }

    env.tree.use_learned_index_ = learned;
    env.tree.cmp_count_         = 0;
    env.tree.fallback_count_    = 0;

    const uint64_t nkeys = lookup_keys.size();
    uint64_t idx = 0;
    for (auto _ : state) {
        // DoNotOptimize on named lvalue (not rvalue) — required pattern
        auto result = env.tree.get(lookup_keys[idx % nkeys]);
        benchmark::DoNotOptimize(result);
        ++idx;
    }

    state.counters["avg_cmp"] = benchmark::Counter(
        static_cast<double>(env.tree.cmp_count_.load()),
        benchmark::Counter::kAvgIterations);
    state.counters["fallbacks"] = benchmark::Counter(
        static_cast<double>(env.tree.fallback_count_.load()),
        benchmark::Counter::kAvgIterations);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pre-generate all key sets at static init (avoids in-benchmark allocation)
// T-11-04 mitigation: Zipf CDF is computed once, not per benchmark iteration
// ─────────────────────────────────────────────────────────────────────────────

static const std::vector<uint64_t> kSeqKeys  = gen_sequential_keys(kWarmupInserts);
static const std::vector<uint64_t> kUniInsert = gen_uniform_keys(kWarmupInserts, kRngSeedInsert);
static const std::vector<uint64_t> kUniLookup = gen_uniform_keys(kLookupPoolSize, kRngSeedLookup);
static const std::vector<uint64_t> kZipKeys   = gen_zipfian_keys(kWarmupInserts);

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark registrations: Sequential / Uniform / Zipfian x Off / On
// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK_CAPTURE(BM_Lookup, Sequential_ModelOff, false, kSeqKeys,  kSeqKeys)
    ->UseRealTime()->MinTime(2.0);
BENCHMARK_CAPTURE(BM_Lookup, Sequential_ModelOn,  true,  kSeqKeys,  kSeqKeys)
    ->UseRealTime()->MinTime(2.0);
BENCHMARK_CAPTURE(BM_Lookup, Uniform_ModelOff,    false, kUniInsert, kUniLookup)
    ->UseRealTime()->MinTime(2.0);
BENCHMARK_CAPTURE(BM_Lookup, Uniform_ModelOn,     true,  kUniInsert, kUniLookup)
    ->UseRealTime()->MinTime(2.0);
BENCHMARK_CAPTURE(BM_Lookup, Zipfian_ModelOff,    false, kZipKeys,  kZipKeys)
    ->UseRealTime()->MinTime(2.0);
BENCHMARK_CAPTURE(BM_Lookup, Zipfian_ModelOn,     true,  kZipKeys,  kZipKeys)
    ->UseRealTime()->MinTime(2.0);

BENCHMARK_MAIN();
