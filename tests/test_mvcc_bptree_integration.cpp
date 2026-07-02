#include "adapttree/bplus_tree.hpp"
#include "adapttree/mvcc.hpp"
#include "adapttree/buffer_pool.hpp"
#include <gtest/gtest.h>
#include <filesystem>

using namespace adapttree;

// Test that BPlusTree<NullWAL, MVCC> can be constructed and used
TEST(MvccBptreeIntegration, MVCC_BTREE_01_ConstructWithRealMvcc) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_btree_test.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        MVCC mvcc;
        BPlusTree<NullWAL, MVCC> tree(pool, wal, mvcc);
        EXPECT_TRUE(tree.insert(1, 10));
        auto v = tree.get(1);
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, 10u);
    }
    std::filesystem::remove(tmp);
}

// Test that get() goes through MVCC read_version (TrivialMvcc is transparent)
TEST(MvccBptreeIntegration, MVCC_BTREE_02_GetUsesReadVersion) {
    auto tmp = std::filesystem::temp_directory_path() / "mvcc_btree_test2.db";
    {
        NullWAL wal;
        DiskManager dm(tmp.string());
        BufferPool<NullWAL> pool(&dm, &wal, 64);
        BPlusTree<NullWAL> tree(pool, wal);  // default TrivialMvcc
        tree.insert(42, 420);
        auto v = tree.get(42);
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, 420u);
        EXPECT_FALSE(tree.get(99).has_value());
    }
    std::filesystem::remove(tmp);
}
