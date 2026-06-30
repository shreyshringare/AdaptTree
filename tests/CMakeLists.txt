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
