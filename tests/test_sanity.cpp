#include <gtest/gtest.h>

TEST(Sanity, BasicAssertions) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}
