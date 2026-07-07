#!/bin/bash
BASE='/mnt/d/Projects/SDE Projects/AdaptTree'
ALL_PASS=1
for bin in page_tests disk_manager_tests eintr_tests buffer_pool_tests bplus_tree_tests wal_tests test_mvcc pgm_builder_tests test_learned_integration test_learned_integration_gaps test_mvcc_bptree_integration test_bplustree_counters wal_crash_recovery_tests; do
  echo "=== $bin ==="
  timeout 60 "$BASE/build/tests/$bin" --gtest_brief=1 2>&1 | tail -3
  if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "FAILED: $bin"
    ALL_PASS=0
  fi
done
if [ $ALL_PASS -eq 1 ]; then
  echo "ALL TESTS PASSED"
else
  echo "SOME TESTS FAILED"
  exit 1
fi
