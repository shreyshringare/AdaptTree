// Explicit template instantiation of BPlusTree<NullWAL, MVCC> for mvcc integration tests.
// Mirrors the pattern used in bplus_tree_spy_inst.cpp for SpyWal.
#include "adapttree/mvcc.hpp"
#include "adapttree/bplus_tree.hpp"
#include "../src/bplus_tree.cpp"  // NOLINT: deliberate TU-scoped include for instantiation

namespace adapttree {
template class BPlusTree<NullWAL, MVCC>;
}
