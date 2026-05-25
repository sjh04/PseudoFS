#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include "core/block_device.h"

using namespace pfs;

class BlockDeviceTest : public ::testing::Test {
protected:
    static constexpr uint32_t kBlocks = 64;
    static constexpr uint32_t kBlockSize = 512;
    BlockDevice dev{kBlocks, kBlockSize};
};

TEST_F(BlockDeviceTest, Geometry) {
    EXPECT_EQ(dev.total_blocks(), kBlocks);
    EXPECT_EQ(dev.block_size(), kBlockSize);
    EXPECT_EQ(dev.disk_size(), kBlocks * kBlockSize);
}

TEST_F(BlockDeviceTest, WriteAndRead) {
    uint8_t wbuf[kBlockSize];
    uint8_t rbuf[kBlockSize];
    std::memset(wbuf, 0xAB, kBlockSize);

    EXPECT_EQ(dev.write_block(0, wbuf), 0);
    EXPECT_EQ(dev.read_block(0, rbuf), 0);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, kBlockSize), 0);
}

TEST_F(BlockDeviceTest, DifferentBlocks) {
    uint8_t buf_a[kBlockSize], buf_b[kBlockSize];
    std::memset(buf_a, 0x11, kBlockSize);
    std::memset(buf_b, 0x22, kBlockSize);

    dev.write_block(0, buf_a);
    dev.write_block(1, buf_b);

    uint8_t rbuf[kBlockSize];
    dev.read_block(0, rbuf);
    EXPECT_EQ(rbuf[0], 0x11);

    dev.read_block(1, rbuf);
    EXPECT_EQ(rbuf[0], 0x22);
}

TEST_F(BlockDeviceTest, OutOfBounds) {
    uint8_t buf[kBlockSize];
    EXPECT_EQ(dev.read_block(kBlocks, buf), -1);
    EXPECT_EQ(dev.write_block(kBlocks, buf), -1);
    EXPECT_EQ(dev.read_block(0, nullptr), -1);
}

TEST_F(BlockDeviceTest, ZeroAll) {
    uint8_t wbuf[kBlockSize];
    std::memset(wbuf, 0xFF, kBlockSize);
    dev.write_block(0, wbuf);

    dev.zero_all();

    uint8_t rbuf[kBlockSize];
    dev.read_block(0, rbuf);
    for (uint32_t i = 0; i < kBlockSize; i++) {
        EXPECT_EQ(rbuf[i], 0);
    }
}

TEST_F(BlockDeviceTest, SaveAndLoad) {
    const char* path = "/tmp/pfs_test_disk.img";

    uint8_t wbuf[kBlockSize];
    std::memset(wbuf, 0xCD, kBlockSize);
    dev.write_block(5, wbuf);

    EXPECT_EQ(dev.save_to_file(path), 0);

    BlockDevice dev2(kBlocks, kBlockSize);
    EXPECT_EQ(dev2.load_from_file(path), 0);

    uint8_t rbuf[kBlockSize];
    dev2.read_block(5, rbuf);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, kBlockSize), 0);

    std::remove(path);
}
