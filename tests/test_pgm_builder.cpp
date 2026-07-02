#include "adapttree/pgm_builder.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <stdexcept>

using namespace adapttree;

TEST(PGMBuilderTest, EmptyInput) {
    PGMBuilder builder(4);
    std::vector<KeyPos> pts;
    auto segs = builder.fit(pts);
    EXPECT_EQ(segs.size(), 0u);
}

TEST(PGMBuilderTest, SinglePoint) {
    PGMBuilder builder(4);
    std::vector<KeyPos> pts = {{42u, 7u}};
    auto segs = builder.fit(pts);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_DOUBLE_EQ(segs[0].slope, 0.0);
    EXPECT_EQ(segs[0].predict(42, 100), 7u);
}

TEST(PGMBuilderTest, SequentialDense_SingleSegment) {
    std::vector<KeyPos> pts;
    for (uint32_t i = 0; i < 200; ++i) pts.push_back({i, i});
    PGMBuilder builder(4);
    auto segs = builder.fit(pts);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_NO_THROW(builder.validate(pts, segs));
}

TEST(PGMBuilderTest, NonLinear_EpsilonZero_ValidateThrows) {
    std::vector<KeyPos> pts = {{0u,0u},{1u,100u},{2u,1u},{3u,200u}};
    PGMBuilder builder(0);
    auto segs = builder.fit(pts);
    EXPECT_THROW(builder.validate(pts, segs), std::logic_error);
}

TEST(PGMBuilderTest, PredictClamp) {
    PGMBuilder builder(4);
    std::vector<KeyPos> pts = {{10u, 5u}};
    auto segs = builder.fit(pts);
    ASSERT_FALSE(segs.empty());
    uint32_t result = segs[0].predict(9999, 10);
    EXPECT_LT(result, 10u);
    EXPECT_EQ(result, 9u);
}

TEST(PGMBuilderTest, MemoryFitsPageHeader) {
    static_assert(sizeof(LearnedSegment) <= 32,
        "LearnedSegment must fit in 32-byte page header reserved area");
    std::vector<KeyPos> pts;
    for (uint32_t i = 0; i < 200; ++i) pts.push_back({i, i});
    PGMBuilder builder(4);
    auto segs = builder.fit(pts);
    EXPECT_LE(segs.size() * sizeof(LearnedSegment), 32u);
}
