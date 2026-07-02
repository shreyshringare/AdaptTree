#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>
#include <map>
#include <string>
#include <cstdio>
#include <filesystem>

#include "sqlite3.h"
#include "adapttree/wal.hpp"
#include "adapttree/bplus_tree.hpp"

// ── Wire format constants ──────────────────────────────────────────────────────
// Each operation is exactly 17 bytes: op_type(1) | key(8 LE) | value(8 LE)
static constexpr size_t  kOpSize      = 17;
static constexpr uint8_t kOpInsert    = 0x01;
static constexpr uint8_t kOpGet       = 0x02;
static constexpr uint8_t kOpFullScan  = 0x03;

// ── LLVMFuzzerTestOneInput ────────────────────────────────────────────────────
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {

    // ── Fresh AdaptTree instance per call ─────────────────────────────────────
    // mkstemp creates a unique temp file; we pass its path to DiskManager.
    char tmppath[] = "/tmp/fuzz_adapttree_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return 0;
    ::close(fd);

    NullWAL wal;
    adapttree::DiskManager dm(tmppath);
    adapttree::BufferPool<NullWAL> pool(&dm, &wal, 64);
    adapttree::BPlusTree<NullWAL> tree(pool, wal);

    // ── Fresh in-memory SQLite oracle per call ────────────────────────────────
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        std::filesystem::remove(tmppath);
        return 0;
    }
    sqlite3_exec(db,
        "CREATE TABLE kv(k INTEGER PRIMARY KEY, v INTEGER)",
        nullptr, nullptr, nullptr);

    // Prepare statements once per call; finalize before close.
    sqlite3_stmt* insert_stmt = nullptr;
    sqlite3_stmt* get_stmt    = nullptr;
    sqlite3_stmt* scan_stmt   = nullptr;

    sqlite3_prepare_v2(db, "INSERT INTO kv(k, v) VALUES(?, ?)", -1,
                       &insert_stmt, nullptr);
    sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?", -1,
                       &get_stmt, nullptr);
    sqlite3_prepare_v2(db, "SELECT k, v FROM kv ORDER BY k", -1,
                       &scan_stmt, nullptr);

    // ── Unique-key pool ───────────────────────────────────────────────────────
    // Prevents duplicate-key semantic divergence: SQLite's INTEGER PRIMARY KEY
    // rejects duplicates with SQLITE_CONSTRAINT; AdaptTree::insert returns false.
    // Both behaviours are correct, but comparing them across engines is fragile.
    // Solution: skip both engines when a duplicate key is encountered.
    std::unordered_set<uint64_t> inserted_keys;

    // ── Operation decode loop ─────────────────────────────────────────────────
    // Trailing bytes (offset + kOpSize > size) are silently ignored — no abort.
    size_t offset = 0;
    while (offset + kOpSize <= size) {
        uint8_t  op    = data[offset];
        uint64_t key   = 0;
        uint64_t value = 0;
        // Use memcpy to avoid UB on unaligned uint64_t reads (T-04-01 mitigation).
        memcpy(&key,   data + offset + 1, 8);
        memcpy(&value, data + offset + 9, 8);
        offset += kOpSize;

        if (op == kOpInsert) {
            // ── Unique-key guard ───────────────────────────────────────────────
            if (inserted_keys.count(key)) {
                // Duplicate: silently skip both engines — not a divergence.
                continue;
            }

            // ── AdaptTree insert ───────────────────────────────────────────────
            bool adapt_ok = tree.insert(key, value);

            // ── SQLite insert ──────────────────────────────────────────────────
            sqlite3_bind_int64(insert_stmt, 1, static_cast<int64_t>(key));
            sqlite3_bind_int64(insert_stmt, 2, static_cast<int64_t>(value));
            int rc = sqlite3_step(insert_stmt);
            sqlite3_reset(insert_stmt);
            bool sqlite_ok = (rc == SQLITE_DONE);

            // ── Divergence check ───────────────────────────────────────────────
            if (adapt_ok != sqlite_ok) {
                fprintf(stderr,
                    "[DIVERGENCE] op=INSERT key=%llu: AdaptTree=%s SQLite=%s\n",
                    (unsigned long long)key,
                    adapt_ok  ? "inserted" : "rejected",
                    sqlite_ok ? "inserted" : "rejected");
                abort();
            }

            if (adapt_ok) {
                inserted_keys.insert(key);
            }

        } else if (op == kOpGet) {
            // ── AdaptTree get ──────────────────────────────────────────────────
            std::optional<uint64_t> adapt_result = tree.get(key);

            // ── SQLite get ─────────────────────────────────────────────────────
            sqlite3_bind_int64(get_stmt, 1, static_cast<int64_t>(key));
            int rc = sqlite3_step(get_stmt);
            bool sqlite_found = (rc == SQLITE_ROW);
            int64_t sqlite_value = sqlite_found ? sqlite3_column_int64(get_stmt, 0) : 0;
            sqlite3_reset(get_stmt);

            // ── Divergence check ───────────────────────────────────────────────
            bool adapt_found = adapt_result.has_value();
            if (adapt_found != sqlite_found) {
                fprintf(stderr,
                    "[DIVERGENCE] op=GET key=%llu: AdaptTree=%s SQLite=%s\n",
                    (unsigned long long)key,
                    adapt_found  ? "found" : "not_found",
                    sqlite_found ? "found" : "not_found");
                abort();
            }
            if (adapt_found && sqlite_found &&
                adapt_result.value() != static_cast<uint64_t>(sqlite_value)) {
                fprintf(stderr,
                    "[DIVERGENCE] op=GET key=%llu: AdaptTree value=%llu SQLite value=%llu\n",
                    (unsigned long long)key,
                    (unsigned long long)adapt_result.value(),
                    (unsigned long long)sqlite_value);
                abort();
            }

        } else if (op == kOpFullScan) {
            // ── Phase 4 FULL_SCAN uses point-query enumeration over inserted_keys ──
            // Since range-scan iterator is not built until Phase 5, we enumerate
            // all inserted keys via point queries and collect results into a map.
            // Phase 5 will replace this with scan(UINT64_MIN, UINT64_MAX).
            std::map<uint64_t, uint64_t> adapt_map;
            for (uint64_t k : inserted_keys) {
                auto v = tree.get(k);
                if (v.has_value()) {
                    adapt_map[k] = v.value();
                }
            }

            // ── SQLite FULL_SCAN ───────────────────────────────────────────────
            std::map<uint64_t, uint64_t> sqlite_map;
            sqlite3_reset(scan_stmt);
            while (sqlite3_step(scan_stmt) == SQLITE_ROW) {
                uint64_t k = static_cast<uint64_t>(sqlite3_column_int64(scan_stmt, 0));
                uint64_t v = static_cast<uint64_t>(sqlite3_column_int64(scan_stmt, 1));
                sqlite_map[k] = v;
            }
            sqlite3_reset(scan_stmt);

            // ── Divergence check ───────────────────────────────────────────────
            if (adapt_map != sqlite_map) {
                fprintf(stderr,
                    "FULL_SCAN divergence: AdaptTree has %zu entries, "
                    "SQLite has %zu entries\n",
                    adapt_map.size(), sqlite_map.size());
                // Print first diverging key
                for (auto& [k, v] : sqlite_map) {
                    auto it = adapt_map.find(k);
                    if (it == adapt_map.end()) {
                        fprintf(stderr, "  missing in AdaptTree: key=%llu\n",
                                (unsigned long long)k);
                        break;
                    } else if (it->second != v) {
                        fprintf(stderr,
                            "  value mismatch at key=%llu: AdaptTree=%llu SQLite=%llu\n",
                            (unsigned long long)k,
                            (unsigned long long)it->second,
                            (unsigned long long)v);
                        break;
                    }
                }
                abort();
            }
        }
        // Unknown op types are silently ignored — forward compatibility.
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    sqlite3_finalize(insert_stmt);
    sqlite3_finalize(get_stmt);
    sqlite3_finalize(scan_stmt);
    sqlite3_close(db);
    // AdaptTree destructors (tree, pool, dm) run automatically via RAII.
    std::filesystem::remove(tmppath);

    return 0;
}
