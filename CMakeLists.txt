cmake_minimum_required(VERSION 3.20)
project(AdaptTree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Library: adapttree (page + disk_manager)
add_library(adapttree STATIC
  src/page.cpp
  src/disk_manager.cpp
)
target_include_directories(adapttree PUBLIC include)
target_compile_features(adapttree PUBLIC cxx_std_20)

add_subdirectory(tests)

option(ADAPTTREE_FUZZING "Build differential fuzzer (requires Clang + libFuzzer)" OFF)
if(ADAPTTREE_FUZZING)
  add_subdirectory(fuzz)
endif()
