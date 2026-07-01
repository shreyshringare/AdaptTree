#!/bin/bash
PROJ="/mnt/d/Projects/SDE Projects/AdaptTree"
cd "$PROJ/build"
echo "=== page_tests ==="
tests/page_tests --gtest_filter="${1:-*}" 2>&1
echo "=== disk_manager_tests ==="
tests/disk_manager_tests --gtest_filter="${1:-*}" 2>&1
echo "=== eintr_tests ==="
tests/eintr_tests --gtest_filter="${1:-*}" 2>&1
echo "=== buffer_pool_tests ==="
tests/buffer_pool_tests --gtest_filter="${1:-*}" 2>&1
