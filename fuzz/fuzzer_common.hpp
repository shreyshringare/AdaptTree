#ifndef ADAPTTREE_FUZZER_COMMON_HPP
#define ADAPTTREE_FUZZER_COMMON_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <map>
#include <string>
#include <cstdio>
#include <filesystem>

#include "sqlite3.h"
#include "adapttree/wal.hpp"
#include "adapttree/bplus_tree.hpp"

// ── Wire format ───────────────────────────────────────────────────────────────
// Each operation is exactly 17 bytes: op_type(1) | key(8 LE) | value(8 LE)
// For kOpDelete and kOpGet, value bytes are ignored.
// For kOpFullScan, key and value bytes are ignored.
static constexpr size_t  kOpSize     = 17;
static constexpr uint8_t kOpInsert   = 0x01;
static constexpr uint8_t kOpGet      = 0x02;
static constexpr uint8_t kOpFullScan = 0x03;
static constexpr uint8_t kOpDelete   = 0x04;

// ── FuzzerState ───────────────────────────────────────────────────────────────
// Owns all per-invocation resources. Constructed once per LLVMFuzzerTestOneInput
// call. Destroyed at end of call, releasing temp file and SQLite connection.
struct FuzzerState {
private:
    static std::string make_tmppath() {
        char buf[] = "/tmp/fuzz_adapttree_XXXXXX";
        int fd = ::mkstemp(buf);
        if (fd >= 0) ::close(fd);
        return std::string(buf);
    }

    // Members declared in init order — do not reorder.
    std::string                        tmppath_;
    NullWAL                            wal_;
    adapttree::DiskManager             dm_;
    adapttree::BufferPool<NullWAL>     pool_;
    adapttree::BPlusTree<NullWAL>      tree_;
    sqlite3*                           db_          = nullptr;
    sqlite3_stmt*                      insert_stmt_ = nullptr;
    sqlite3_stmt*                      get_stmt_    = nullptr;
    sqlite3_stmt*                      delete_stmt_ = nullptr;
    sqlite3_stmt*                      scan_stmt_   = nullptr;

public:
    std::unordered_set<uint64_t> inserted_keys;

    explicit FuzzerState(bool use_learned_index)
        : tmppath_(make_tmppath())
        , dm_(tmppath_)
        , pool_(&dm_, &wal_, 64)
        , tree_(pool_, wal_)
    {
        tree_.use_learned_index_ = use_learned_index;

        if (sqlite3_open(":memory:", &db_) != SQLITE_OK) return;
        sqlite3_exec(db_,
            "CREATE TABLE kv(k INTEGER PRIMARY KEY, v INTEGER)",
            nullptr, nullptr, nullptr);
        sqlite3_prepare_v2(db_, "INSERT INTO kv(k, v) VALUES(?, ?)", -1,
                           &insert_stmt_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT v FROM kv WHERE k = ?", -1,
                           &get_stmt_, nullptr);
        sqlite3_prepare_v2(db_, "DELETE FROM kv WHERE k = ?", -1,
                           &delete_stmt_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT k, v FROM kv ORDER BY k", -1,
                           &scan_stmt_, nullptr);
    }

    ~FuzzerState() {
        sqlite3_finalize(insert_stmt_);
        sqlite3_finalize(get_stmt_);
        sqlite3_finalize(delete_stmt_);
        sqlite3_finalize(scan_stmt_);
        sqlite3_close(db_);
        std::filesystem::remove(tmppath_);
    }

    // Non-copyable, non-movable — owns unique temp file and DB connection.
    FuzzerState(const FuzzerState&)            = delete;
    FuzzerState& operator=(const FuzzerState&) = delete;

    adapttree::BPlusTree<NullWAL>& tree()        { return tree_; }
    sqlite3*                        db()          { return db_; }
    sqlite3_stmt*                   insert_stmt() { return insert_stmt_; }
    sqlite3_stmt*                   get_stmt()    { return get_stmt_; }
    sqlite3_stmt*                   delete_stmt() { return delete_stmt_; }
    sqlite3_stmt*                   scan_stmt()   { return scan_stmt_; }
};

// ── DecodeAndExecute ──────────────────────────────────────────────────────────
// Decode fuzzer bytes and execute each operation on both AdaptTree and the
// SQLite oracle. Aborts on any behavioral divergence.
inline void DecodeAndExecute(FuzzerState& s, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset + kOpSize <= size) {
        uint8_t  op    = data[offset];
        uint64_t key   = 0;
        uint64_t value = 0;
        // memcpy avoids UB on unaligned uint64_t reads (T-04-01 mitigation).
        std::memcpy(&key,   data + offset + 1, 8);
        std::memcpy(&value, data + offset + 9, 8);
        offset += kOpSize;

        if (op == kOpInsert) {
            if (s.inserted_keys.count(key)) continue;  // duplicate guard

            bool adapt_ok = s.tree().insert(key, value);

            sqlite3_bind_int64(s.insert_stmt(), 1, static_cast<int64_t>(key));
            sqlite3_bind_int64(s.insert_stmt(), 2, static_cast<int64_t>(value));
            int rc = sqlite3_step(s.insert_stmt());
            sqlite3_reset(s.insert_stmt());
            bool sqlite_ok = (rc == SQLITE_DONE);

            if (adapt_ok != sqlite_ok) {
                fprintf(stderr,
                    "[DIVERGENCE] op=INSERT key=%llu: AdaptTree=%s SQLite=%s\n",
                    (unsigned long long)key,
                    adapt_ok  ? "inserted" : "rejected",
                    sqlite_ok ? "inserted" : "rejected");
                abort();
            }
            if (adapt_ok) s.inserted_keys.insert(key);

        } else if (op == kOpGet) {
            auto adapt_result = s.tree().get(key);

            sqlite3_bind_int64(s.get_stmt(), 1, static_cast<int64_t>(key));
            int rc = sqlite3_step(s.get_stmt());
            bool    sqlite_found = (rc == SQLITE_ROW);
            int64_t sqlite_value = sqlite_found
                                   ? sqlite3_column_int64(s.get_stmt(), 0) : 0;
            sqlite3_reset(s.get_stmt());

            bool adapt_found = adapt_result.has_value();
            if (adapt_found != sqlite_found) {
                fprintf(stderr,
                    "[DIVERGENCE] op=GET key=%llu: AdaptTree=%s SQLite=%s\n",
                    (unsigned long long)key,
                    adapt_found  ? "found" : "not_found",
                    sqlite_found ? "found" : "not_found");
                abort();
            }
            if (adapt_found &&
                adapt_result.value() != static_cast<uint64_t>(sqlite_value)) {
                fprintf(stderr,
                    "[DIVERGENCE] op=GET key=%llu: AdaptTree=%llu SQLite=%llu\n",
                    (unsigned long long)key,
                    (unsigned long long)adapt_result.value(),
                    (unsigned long long)sqlite_value);
                abort();
            }

        } else if (op == kOpDelete) {
            if (!s.inserted_keys.count(key)) continue;  // unique-key guard

            bool adapt_ok = s.tree().remove(key);

            sqlite3_bind_int64(s.delete_stmt(), 1, static_cast<int64_t>(key));
            int rc           = sqlite3_step(s.delete_stmt());
            sqlite3_reset(s.delete_stmt());
            bool sqlite_ok   = (rc == SQLITE_DONE);
            int  sqlite_chg  = sqlite3_changes(s.db());

            if (!adapt_ok) {
                fprintf(stderr,
                    "[DIVERGENCE] op=DELETE key=%llu: AdaptTree returned false "
                    "but key was in inserted_keys (SQLite changes=%d)\n",
                    (unsigned long long)key, sqlite_chg);
                abort();
            }
            if (sqlite_chg == 0) {
                fprintf(stderr,
                    "[DIVERGENCE] op=DELETE key=%llu: SQLite deleted 0 rows "
                    "but key was in inserted_keys\n",
                    (unsigned long long)key);
                abort();
            }
            s.inserted_keys.erase(key);

        } else if (op == kOpFullScan) {
            std::map<uint64_t, uint64_t> adapt_map;
            {
                auto it = s.tree().scan(0, UINT64_MAX);
                while (it.valid()) {
                    adapt_map[it.key()] = it.value();
                    it.next();
                }
            }

            std::map<uint64_t, uint64_t> sqlite_map;
            sqlite3_reset(s.scan_stmt());
            while (sqlite3_step(s.scan_stmt()) == SQLITE_ROW) {
                uint64_t k = static_cast<uint64_t>(
                    sqlite3_column_int64(s.scan_stmt(), 0));
                uint64_t v = static_cast<uint64_t>(
                    sqlite3_column_int64(s.scan_stmt(), 1));
                sqlite_map[k] = v;
            }
            sqlite3_reset(s.scan_stmt());

            if (adapt_map != sqlite_map) {
                fprintf(stderr,
                    "FULL_SCAN divergence: AdaptTree has %zu entries, "
                    "SQLite has %zu entries\n",
                    adapt_map.size(), sqlite_map.size());
                for (auto& [k, v] : sqlite_map) {
                    auto it = adapt_map.find(k);
                    if (it == adapt_map.end()) {
                        fprintf(stderr, "  missing in AdaptTree: key=%llu\n",
                                (unsigned long long)k);
                        break;
                    } else if (it->second != v) {
                        fprintf(stderr,
                            "  value mismatch at key=%llu: "
                            "AdaptTree=%llu SQLite=%llu\n",
                            (unsigned long long)k,
                            (unsigned long long)it->second,
                            (unsigned long long)v);
                        break;
                    }
                }
                abort();
            }
        }
        // Unknown op types silently ignored — forward compatibility.
    }
}

#endif // ADAPTTREE_FUZZER_COMMON_HPP
