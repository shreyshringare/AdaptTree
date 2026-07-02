#include "fuzzer_common.hpp"

// Differential fuzzer — base configuration (use_learned_index_ = false).
// Validates AdaptTree correctness against the SQLite oracle with the learned
// index disabled. Any divergence is a base tree bug.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzerState s(/*use_learned_index=*/false);
    DecodeAndExecute(s, data, size);
    return 0;
}
