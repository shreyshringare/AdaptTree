// Explicit template instantiation of BPlusTree<SpyWal> for wal_tests.
// This TU is compiled as part of wal_tests so that test_wal.cpp can use
// BPlusTree<SpyWal> without pulling the full bplus_tree.cpp into test_wal.cpp.
#include "spy_wal.hpp"
#include "adapttree/bplus_tree.hpp"
#include "../src/bplus_tree.cpp"  // NOLINT: deliberate TU-scoped include for instantiation

namespace adapttree {
template class BPlusTree<SpyWal>;
}
