cmake_minimum_required(VERSION 3.20)

add_executable(learned_index_bench
    learned_index_bench.cpp
    ${CMAKE_SOURCE_DIR}/src/bplus_tree.cpp
    ${CMAKE_SOURCE_DIR}/src/buffer_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/pgm_builder.cpp
)

target_include_directories(learned_index_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(learned_index_bench PRIVATE
    adapttree
    pgm_builder
    benchmark::benchmark
)

target_compile_options(learned_index_bench PRIVATE
    -O2 -Wall -Wextra -Wno-unused-parameter
)

target_compile_features(learned_index_bench PUBLIC cxx_std_20)
