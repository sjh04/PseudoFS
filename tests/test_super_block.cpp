#include <gtest/gtest.h>

#include <set>

#include "core/block_device.h"
#include "core/constants.h"
#include "fs/unix/super_block.h"

using namespace pfs;

class SuperBlockTest : public ::testing::Test {
   protected:
    BlockDevice dev{TOTAL_BLK_NUM, BLOCK_SIZE};
    SuperBlock sb{dev};

    void SetUp() override {
        sb.format(3, 4);  // reserve 3 data blocks, 4 inodes (root dir, etc)
    }
};

TEST_F(SuperBlockTest, FormatInitialization) {
    EXPECT_EQ(sb.total_blocks(), DATA_BLK_NUM);
    EXPECT_EQ(sb.total_inodes(), INODE_TOTAL);
    EXPECT_EQ(sb.free_block_count(), DATA_BLK_NUM - 3);
    EXPECT_EQ(sb.free_inode_count(), INODE_TOTAL - 4);
}

TEST_F(SuperBlockTest, BallocReturnsValidBlocks) {
    uint16_t blk = sb.balloc();
    EXPECT_NE(blk, INVALID_BLK);
    EXPECT_LT(blk, DATA_BLK_NUM);
}

TEST_F(SuperBlockTest, BallocReturnsUniqueBlocks) {
    std::set<uint16_t> allocated;
    uint16_t total_free = sb.free_block_count();
    for (uint16_t i = 0; i < total_free; i++) {
        uint16_t blk = sb.balloc();
        ASSERT_NE(blk, INVALID_BLK) << "failed at allocation " << i;
        ASSERT_TRUE(allocated.insert(blk).second)
            << "duplicate block " << blk << " at allocation " << i;
    }
    // Now disk should be full
    EXPECT_EQ(sb.balloc(), INVALID_BLK);
    EXPECT_EQ(sb.free_block_count(), 0);
}

TEST_F(SuperBlockTest, BfreeAndRealloc) {
    uint16_t blk = sb.balloc();
    uint16_t free_before = sb.free_block_count();

    sb.bfree(blk);
    EXPECT_EQ(sb.free_block_count(), free_before + 1);

    uint16_t blk2 = sb.balloc();
    EXPECT_EQ(blk2, blk);
}

TEST_F(SuperBlockTest, GroupChainCrossing) {
    // Allocate more than NICFREE blocks to force group chain traversal
    std::set<uint16_t> allocated;
    for (uint32_t i = 0; i < NICFREE + 10; i++) {
        uint16_t blk = sb.balloc();
        ASSERT_NE(blk, INVALID_BLK);
        allocated.insert(blk);
    }
    EXPECT_EQ(allocated.size(), NICFREE + 10);
}

TEST_F(SuperBlockTest, BfreeOverflow) {
    // Allocate many blocks, then free them all to trigger stack overflow handling
    std::vector<uint16_t> blocks;
    for (uint32_t i = 0; i < NICFREE + 10; i++) {
        blocks.push_back(sb.balloc());
    }
    uint16_t free_before = sb.free_block_count();

    for (auto blk : blocks) {
        sb.bfree(blk);
    }
    EXPECT_EQ(sb.free_block_count(), free_before + blocks.size());
}

TEST_F(SuperBlockTest, IallocReturnsValidInodes) {
    uint16_t ino = sb.ialloc();
    EXPECT_NE(ino, INVALID_BLK);
    EXPECT_LT(ino, INODE_TOTAL);
}

TEST_F(SuperBlockTest, IallocReturnsUniqueInodes) {
    // The inode stack caches up to NICINOD entries at a time.
    // Disk-based refill is handled by the inode module (not yet implemented),
    // so here we only test the cached batch.
    std::set<uint16_t> allocated;
    for (uint16_t i = 0; i < NICINOD; i++) {
        uint16_t ino = sb.ialloc();
        ASSERT_NE(ino, INVALID_BLK);
        ASSERT_TRUE(allocated.insert(ino).second);
    }
    // Stack exhausted (no disk refill yet)
    EXPECT_EQ(sb.ialloc(), INVALID_BLK);
}

TEST_F(SuperBlockTest, IfreeAndRealloc) {
    uint16_t ino = sb.ialloc();
    sb.ifree(ino);
    uint16_t ino2 = sb.ialloc();
    EXPECT_EQ(ino2, ino);
}

TEST_F(SuperBlockTest, PersistenceAcrossLoadFlush) {
    uint16_t blk1 = sb.balloc();
    uint16_t blk2 = sb.balloc();
    uint16_t ino1 = sb.ialloc();
    sb.flush();

    SuperBlock sb2(dev);
    sb2.load();
    EXPECT_EQ(sb2.free_block_count(), sb.free_block_count());
    EXPECT_EQ(sb2.free_inode_count(), sb.free_inode_count());

    // Next allocation should not repeat already-allocated values
    uint16_t blk3 = sb2.balloc();
    EXPECT_NE(blk3, blk1);
    EXPECT_NE(blk3, blk2);
}
