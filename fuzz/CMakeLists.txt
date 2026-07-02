cmake_minimum_required(VERSION 3.20)

# Guard: libFuzzer is Clang-only
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Differential fuzzer requires Clang (libFuzzer). "
                      "Configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

# ---------- SQLite3 static lib (NO -fsanitize=fuzzer) ----------
# sqlite3.c must be placed in fuzz/third_party/sqlite3.c
# Download from https://sqlite.org/amalgamation.html (sqlite-amalgamation-*.zip)
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/)

# Remove fuzzer instrumentation from SQLite; keep ASAN + UBSAN.
# Using generator expression to strip only the fuzzer flag.
target_compile_options(sqlite3 PRIVATE
  -g -O1
  -fsanitize=address,undefined
  -fno-omit-frame-pointer
  -fno-sanitize-recover=undefined
  # Explicitly NO -fsanitize=fuzzer — SQLite must not be instrumented
)

# ---------- Fuzzer harness (full sanitizer set) ----------
add_executable(differential_fuzzer differential_fuzzer.cpp)

# Add AdaptTree sources directly (same pattern as tests/CMakeLists.txt)
target_sources(differential_fuzzer PRIVATE
  ${CMAKE_SOURCE_DIR}/src/bplus_tree.cpp
  ${CMAKE_SOURCE_DIR}/src/buffer_pool.cpp
)

target_compile_options(differential_fuzzer PRIVATE
  -g
  -O1
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize=vptr
  -fno-omit-frame-pointer
  -fno-sanitize-recover=undefined
  -std=c++20
)

target_link_options(differential_fuzzer PRIVATE
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize=vptr
)

target_include_directories(differential_fuzzer PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  third_party/
)

target_link_libraries(differential_fuzzer PRIVATE
  sqlite3
  adapttree      # The AdaptTree static lib target (page + disk_manager)
)

# ---------- Differential fuzzer — learned index ON (FUZZ-06, FUZZ-07) ----------
add_executable(differential_fuzzer_learned differential_fuzzer_learned.cpp)

target_sources(differential_fuzzer_learned PRIVATE
  ${CMAKE_SOURCE_DIR}/src/bplus_tree.cpp
  ${CMAKE_SOURCE_DIR}/src/buffer_pool.cpp
  ${CMAKE_SOURCE_DIR}/src/pgm_builder.cpp
)

target_compile_options(differential_fuzzer_learned PRIVATE
  -g
  -O1
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize=vptr
  -fno-omit-frame-pointer
  -fno-sanitize-recover=undefined
  -std=c++20
)

target_link_options(differential_fuzzer_learned PRIVATE
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize=vptr
)

target_include_directories(differential_fuzzer_learned PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/fuzz
  third_party/
)

target_link_libraries(differential_fuzzer_learned PRIVATE
  sqlite3
  adapttree
)

# ---------- Base fuzzer: add pgm_builder source (Phase 9 integration) ----------
target_sources(differential_fuzzer PRIVATE
  ${CMAKE_SOURCE_DIR}/src/pgm_builder.cpp
)
target_include_directories(differential_fuzzer PRIVATE
  ${CMAKE_SOURCE_DIR}/fuzz
)

# ---------- Corpus directory ----------
# fuzz/corpus/ must exist; libFuzzer reads seeds from it.
# Seeds are committed binary files, not generated at build time.
