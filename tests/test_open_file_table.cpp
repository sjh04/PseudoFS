#include <gtest/gtest.h>

#include "core/constants.h"
#include "core/open_file_table.h"
#include "core/vfs.h"

using namespace pfs;

class OpenFileTableTest : public ::testing::Test {
   protected:
    OpenFileTable oft;
};

TEST_F(OpenFileTableTest, AllocAndQuery) {
    int fd = oft.alloc_fd(0, 42, O_READ);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(oft.is_valid(0, fd));
    EXPECT_EQ(oft.get_inode(0, fd), 42);
    EXPECT_EQ(oft.get_flags(0, fd), O_READ);
    EXPECT_EQ(oft.get_offset(0, fd), 0);
}

TEST_F(OpenFileTableTest, OffsetReadWrite) {
    int fd = oft.alloc_fd(0, 10, O_READ | O_WRITE);
    EXPECT_EQ(oft.set_offset(0, fd, 1024), 0);
    EXPECT_EQ(oft.get_offset(0, fd), 1024);
}

TEST_F(OpenFileTableTest, FreeFd) {
    int fd = oft.alloc_fd(0, 10, O_READ);
    EXPECT_EQ(oft.free_fd(0, fd), 0);
    EXPECT_FALSE(oft.is_valid(0, fd));
    EXPECT_EQ(oft.get_inode(0, fd), INVALID_BLK);
}

TEST_F(OpenFileTableTest, FreeInvalidFails) {
    EXPECT_EQ(oft.free_fd(0, 0), -1);
    EXPECT_EQ(oft.free_fd(0, -1), -1);
    EXPECT_EQ(oft.free_fd(99, 0), -1);
}

TEST_F(OpenFileTableTest, MultipleUsers) {
    int fd_u0 = oft.alloc_fd(0, 100, O_READ);
    int fd_u1 = oft.alloc_fd(1, 200, O_WRITE);

    EXPECT_EQ(oft.get_inode(0, fd_u0), 100);
    EXPECT_EQ(oft.get_inode(1, fd_u1), 200);

    // fd values are per-user (both can be 0), but they point to different inodes
    EXPECT_NE(oft.get_inode(0, fd_u0), oft.get_inode(1, fd_u1));

    oft.free_fd(0, fd_u0);
    oft.free_fd(1, fd_u1);
}

TEST_F(OpenFileTableTest, UserTableFull) {
    for (uint32_t i = 0; i < MAX_OPEN_FILE; i++) {
        EXPECT_GE(oft.alloc_fd(0, i, O_READ), 0);
    }
    EXPECT_EQ(oft.alloc_fd(0, 999, O_READ), -1);
}

TEST_F(OpenFileTableTest, SysTableFull) {
    // Fill the system table across multiple users
    int count = 0;
    for (uint32_t u = 0; u < MAX_USER && count < static_cast<int>(SYS_OPEN_FILE); u++) {
        for (uint32_t f = 0; f < MAX_OPEN_FILE && count < static_cast<int>(SYS_OPEN_FILE); f++) {
            int fd = oft.alloc_fd(u, count, O_READ);
            ASSERT_GE(fd, 0) << "failed at count=" << count;
            count++;
        }
    }
    // Next alloc should fail (system table full)
    EXPECT_EQ(oft.alloc_fd(0, 999, O_READ), -1);
}

TEST_F(OpenFileTableTest, CloseAll) {
    oft.alloc_fd(0, 10, O_READ);
    oft.alloc_fd(0, 20, O_WRITE);
    oft.alloc_fd(0, 30, O_READ);

    oft.close_all(0);

    for (uint32_t i = 0; i < MAX_OPEN_FILE; i++) {
        EXPECT_FALSE(oft.is_valid(0, static_cast<int>(i)));
    }
}

TEST_F(OpenFileTableTest, ReuseAfterFree) {
    int fd1 = oft.alloc_fd(0, 10, O_READ);
    oft.free_fd(0, fd1);

    int fd2 = oft.alloc_fd(0, 20, O_WRITE);
    EXPECT_EQ(fd2, fd1);
    EXPECT_EQ(oft.get_inode(0, fd2), 20);
}

TEST_F(OpenFileTableTest, IndependentOffsets) {
    // Two users open the same inode — separate offsets
    int fd0 = oft.alloc_fd(0, 42, O_READ);
    int fd1 = oft.alloc_fd(1, 42, O_READ);

    oft.set_offset(0, fd0, 100);
    oft.set_offset(1, fd1, 200);

    EXPECT_EQ(oft.get_offset(0, fd0), 100);
    EXPECT_EQ(oft.get_offset(1, fd1), 200);

    oft.free_fd(0, fd0);
    oft.free_fd(1, fd1);
}
