include(GoogleTest)

add_executable(page_tests test_page.cpp)
target_link_libraries(page_tests PRIVATE adapttree GTest::gtest_main)
gtest_discover_tests(page_tests)

add_executable(disk_manager_tests test_disk_manager.cpp)
target_link_libraries(disk_manager_tests PRIVATE adapttree GTest::gtest_main)
gtest_discover_tests(disk_manager_tests)

add_executable(eintr_tests test_eintr.cpp)
target_link_libraries(eintr_tests PRIVATE adapttree GTest::gtest_main)
gtest_discover_tests(eintr_tests)

add_executable(buffer_pool_tests test_buffer_pool.cpp ../src/buffer_pool.cpp)
target_include_directories(buffer_pool_tests PRIVATE ../include)
target_link_libraries(buffer_pool_tests PRIVATE GTest::gtest_main)
target_compile_features(buffer_pool_tests PRIVATE cxx_std_20)
gtest_discover_tests(buffer_pool_tests)

add_executable(bplus_tree_tests test_bplus_tree.cpp ../src/bplus_tree.cpp ../src/buffer_pool.cpp)
target_include_directories(bplus_tree_tests PRIVATE ../include)
target_link_libraries(bplus_tree_tests PRIVATE pgm_builder GTest::gtest_main)
target_compile_features(bplus_tree_tests PRIVATE cxx_std_20)
gtest_discover_tests(bplus_tree_tests)

add_executable(wal_tests
  test_wal.cpp
  bplus_tree_spy_inst.cpp
  ../src/buffer_pool.cpp
)
target_include_directories(wal_tests PRIVATE ../include .)
target_link_libraries(wal_tests PRIVATE adapttree pgm_builder GTest::gtest_main)
target_compile_features(wal_tests PRIVATE cxx_std_20)
gtest_discover_tests(wal_tests)

add_executable(test_mvcc test_mvcc.cpp)
target_link_libraries(test_mvcc PRIVATE mvcc GTest::gtest_main)
gtest_discover_tests(test_mvcc)

add_executable(pgm_builder_tests test_pgm_builder.cpp)
target_link_libraries(pgm_builder_tests PRIVATE pgm_builder GTest::gtest_main)
gtest_discover_tests(pgm_builder_tests)

add_executable(test_learned_integration
  test_learned_integration.cpp
  ../src/bplus_tree.cpp
  ../src/buffer_pool.cpp
)
target_include_directories(test_learned_integration PRIVATE ../include)
target_link_libraries(test_learned_integration PRIVATE pgm_builder GTest::gtest_main)
target_compile_features(test_learned_integration PRIVATE cxx_std_20)
gtest_discover_tests(test_learned_integration)

add_executable(test_learned_integration_gaps
  test_learned_integration_gaps.cpp
  ../src/bplus_tree.cpp
  ../src/buffer_pool.cpp
)
target_include_directories(test_learned_integration_gaps PRIVATE ../include)
target_link_libraries(test_learned_integration_gaps PRIVATE pgm_builder GTest::gtest_main)
target_compile_features(test_learned_integration_gaps PRIVATE cxx_std_20)
gtest_discover_tests(test_learned_integration_gaps)

add_executable(test_mvcc_bptree_integration
  test_mvcc_bptree_integration.cpp
  bplus_tree_mvcc_inst.cpp
  ../src/buffer_pool.cpp
  ../src/mvcc.cpp
)
target_include_directories(test_mvcc_bptree_integration PRIVATE ../include)
target_link_libraries(test_mvcc_bptree_integration PRIVATE pgm_builder GTest::gtest_main)
target_compile_features(test_mvcc_bptree_integration PRIVATE cxx_std_20)
gtest_discover_tests(test_mvcc_bptree_integration)

add_executable(test_bplustree_counters
  test_bplustree_counters.cpp
  ../src/bplus_tree.cpp
  ../src/buffer_pool.cpp
)
target_include_directories(test_bplustree_counters PRIVATE ../include)
target_link_libraries(test_bplustree_counters PRIVATE pgm_builder GTest::gtest_main)
target_compile_features(test_bplustree_counters PRIVATE cxx_std_20)
gtest_discover_tests(test_bplustree_counters)

add_executable(wal_crash_recovery_tests
  test_wal_crash_recovery.cpp
  bplus_tree_wal_inst.cpp
  ../src/buffer_pool.cpp
)
target_include_directories(wal_crash_recovery_tests PRIVATE ../include .)
target_link_libraries(wal_crash_recovery_tests PRIVATE adapttree pgm_builder GTest::gtest_main)
target_compile_features(wal_crash_recovery_tests PRIVATE cxx_std_20)
gtest_discover_tests(wal_crash_recovery_tests)
