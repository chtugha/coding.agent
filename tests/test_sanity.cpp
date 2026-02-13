#include <gtest/gtest.h>

TEST(SanityTest, BasicAssertions) {
    EXPECT_EQ(7 * 6, 42);
    EXPECT_TRUE(true);
}

TEST(SanityTest, StringComparison) {
    std::string hello = "Hello";
    EXPECT_EQ(hello, "Hello");
}
