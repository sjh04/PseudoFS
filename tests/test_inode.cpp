#include <gtest/gtest.h>

#include <cstring>

#include "core/block_device.h"
#include "core/constants.h"
#include "fs/unix/inode.h"
#include "fs/unix/super_block.h"

using namespace pfs;

class InodeTest : public ::testing::Test {
protected:
    BlockDevice dev{TOTAL_BLK_NUM, BLOCK_SIZE};
    SuperBlock sb{dev};
    InodeManager mgr{dev, sb};

    void SetUp() override {
        sb.format(3, 0);
    }
};

TEST_F(InodeTest, AllocAndGet) {
    MemINode* ip = mgr.alloc(MODE_FILE | DEFAULT_MODE, 1000, 100);
    ASSERT_NE(ip, nullptr);
    EXPECT_EQ(ip->di.di_mode, MODE_FILE | DEFAULT_MODE);
    EXPECT_EQ(ip->di.di_uid, 1000);
    EXPECT_EQ(ip->di.di_gid, 100);
    EXPECT_EQ(ip->di.di_nlink, 1);
    EXPECT_EQ(ip->di.di_size, 0);
    EXPECT_EQ(ip->i_count, 1);

    uint16_t ino = ip->i_ino;
    mgr.write_back(ip);
    mgr.put(ip);

    // Re-read from disk
    MemINode* ip2 = mgr.get(ino);
    ASSERT_NE(ip2, nullptr);
    EXPECT_EQ(ip2->di.di_mode, MODE_FILE | DEFAULT_MODE);
    EXPECT_EQ(ip2->di.di_uid, 1000);
    mgr.put(ip2);
}

TEST_F(InodeTest, RefCounting) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);
    uint16_t ino = ip->i_ino;

    MemINode* ip2 = mgr.get(ino);
    EXPECT_EQ(ip->i_count, 2);
    EXPECT_EQ(ip2, ip);

    mgr.put(ip2);
    EXPECT_EQ(ip->i_count, 1);
    mgr.put(ip);
}

TEST_F(InodeTest, DirectBlocks) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);

    for (uint32_t i = 0; i < DIRECT_ADDR; i++) {
        uint16_t blk = mgr.bmap_alloc(ip, i);
        ASSERT_NE(blk, INVALID_BLK);
        EXPECT_NE(blk, 0);
    }

    // Verify bmap returns the same blocks
    for (uint32_t i = 0; i < DIRECT_ADDR; i++) {
        uint16_t blk = mgr.bmap(ip, i);
        EXPECT_EQ(blk, ip->di.di_addr[i]);
    }

    mgr.put(ip);
}

TEST_F(InodeTest, SingleIndirect) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);

    // Map a block in the single-indirect range (addr[6])
    uint32_t logical = DIRECT_ADDR + 5;
    uint16_t blk = mgr.bmap_alloc(ip, logical);
    ASSERT_NE(blk, INVALID_BLK);

    // The indirect block itself should be allocated
    EXPECT_NE(ip->di.di_addr[DIRECT_ADDR], 0);

    // bmap should return the same block
    EXPECT_EQ(mgr.bmap(ip, logical), blk);

    mgr.put(ip);
}

TEST_F(InodeTest, SecondIndirectRange) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);

    // Map a block in the second single-indirect range (addr[7])
    uint32_t logical = DIRECT_ADDR + PTRS_PER_BLK + 10;
    uint16_t blk = mgr.bmap_alloc(ip, logical);
    ASSERT_NE(blk, INVALID_BLK);

    EXPECT_NE(ip->di.di_addr[DIRECT_ADDR + 1], 0);
    EXPECT_EQ(mgr.bmap(ip, logical), blk);

    mgr.put(ip);
}

TEST_F(InodeTest, DoubleIndirect) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);

    // Map a block in the double-indirect range (addr[8])
    uint32_t logical = DIRECT_ADDR + 2 * PTRS_PER_BLK + 5;
    uint16_t blk = mgr.bmap_alloc(ip, logical);
    ASSERT_NE(blk, INVALID_BLK);

    EXPECT_NE(ip->di.di_addr[8], 0);
    EXPECT_EQ(mgr.bmap(ip, logical), blk);

    mgr.put(ip);
}

TEST_F(InodeTest, Truncate) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);
    uint16_t free_before = sb.free_block_count();

    // Allocate some direct + indirect blocks
    for (uint32_t i = 0; i < DIRECT_ADDR + 3; i++) {
        mgr.bmap_alloc(ip, i);
    }

    uint16_t free_after_alloc = sb.free_block_count();
    // Should have allocated DIRECT_ADDR+3 data blocks + 1 indirect block
    EXPECT_EQ(free_before - free_after_alloc, DIRECT_ADDR + 3 + 1);

    mgr.truncate(ip);

    // All blocks should be freed
    EXPECT_EQ(sb.free_block_count(), free_before);
    EXPECT_EQ(ip->di.di_size, 0);
    for (int i = 0; i < NADDR; i++) {
        EXPECT_EQ(ip->di.di_addr[i], 0);
    }

    mgr.put(ip);
}

TEST_F(InodeTest, UnlinkFreesBlocks) {
    uint16_t free_before = sb.free_block_count();

    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);
    mgr.bmap_alloc(ip, 0);
    mgr.bmap_alloc(ip, 1);

    uint16_t free_after_alloc = sb.free_block_count();
    uint16_t inode_free_before = sb.free_inode_count();

    // Simulate unlink: set nlink to 0, then put
    ip->di.di_nlink = 0;
    mgr.put(ip);

    // Data blocks should be freed back
    EXPECT_EQ(sb.free_block_count(), free_before);
    // Inode should be freed too
    EXPECT_EQ(sb.free_inode_count(), inode_free_before + 1);
}

TEST_F(InodeTest, WriteAndReadBlock) {
    MemINode* ip = mgr.alloc(MODE_FILE, 0, 0);
    uint16_t phys = mgr.bmap_alloc(ip, 0);
    ASSERT_NE(phys, INVALID_BLK);

    // Write data to the physical block
    uint8_t wbuf[BLOCK_SIZE];
    std::memset(wbuf, 0x42, BLOCK_SIZE);
    dev.write_block(DATA_START_BLK + phys, wbuf);

    // Read back via bmap
    uint16_t phys2 = mgr.bmap(ip, 0);
    EXPECT_EQ(phys, phys2);

    uint8_t rbuf[BLOCK_SIZE];
    dev.read_block(DATA_START_BLK + phys2, rbuf);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, BLOCK_SIZE), 0);

    mgr.put(ip);
}
