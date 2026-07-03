// Explicit template instantiation of BPlusTree<Wal> for wal_crash_recovery_tests.
#include "adapttree/wal.hpp"
#include "adapttree/bplus_tree.hpp"
#include "../src/bplus_tree.cpp"  // NOLINT: deliberate TU-scoped include for instantiation

namespace adapttree {
template class BPlusTree<::Wal>;  // BPlusTree<Wal, TrivialMvcc> via default
}
