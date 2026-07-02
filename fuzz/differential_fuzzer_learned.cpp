#include "fuzzer_common.hpp"

// Differential fuzzer — learned index configuration (use_learned_index_ = true).
// Runs the identical operation decode loop as differential_fuzzer but with the
// learned index enabled. Any divergence from the SQLite oracle is a
// learned-index-specific bug (the base fuzzer already cleared the base tree).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzerState s(/*use_learned_index=*/true);
    DecodeAndExecute(s, data, size);
    return 0;
}
