#pragma once

#include <cstdint>

#include "core/block_device.h"
#include "core/constants.h"

namespace pfs {

// On-disk superblock layout, stored at block SUPER_BLK (block 1).
// The free block stack uses the group-linked method from the PPT:
//   s_free[0..s_nfree-1] holds block numbers.
//   When s_nfree==1, s_free[0] is the link to the next group (0 = end of chain).
//   Allocation pops from top (s_nfree-1); freeing pushes to top.
struct DiskSuperBlock {
    uint16_t s_isize;               // inode block count
    uint16_t s_fsize;               // data block count
    uint16_t s_nfree;               // entries in free block stack
    uint16_t s_free[NICFREE];       // free block stack
    uint16_t s_ninode;              // entries in free inode stack
    uint16_t s_inode[NICINOD];      // free inode stack
    uint16_t s_rinode;              // remembered inode for linear scan
    uint8_t s_fmod;                 // modification flag
};

class SuperBlock {
public:
    explicit SuperBlock(BlockDevice& dev);

    // Initialize free block chain and free inode stack on a fresh disk.
    // Call this during format. Assumes data blocks 0..DATA_BLK_NUM-1 are all free
    // except those pre-allocated by the caller (root dir, etc).
    void format(uint16_t reserved_blocks, uint16_t reserved_inodes);

    // Read superblock from disk into memory.
    void load();

    // Write superblock back to disk.
    void flush();

    // Allocate one free data block. Returns block number relative to data area
    // (0-based), or INVALID_BLK if disk is full.
    uint16_t balloc();

    // Free a data block (block number relative to data area).
    void bfree(uint16_t block_no);

    // Allocate a free inode number. Returns inode number (0-based),
    // or INVALID_BLK if no free inodes.
    uint16_t ialloc();

    // Free an inode number.
    void ifree(uint16_t inode_no);

    // Accessors
    uint16_t free_block_count() const;
    uint16_t free_inode_count() const;
    uint16_t total_blocks() const;
    uint16_t total_inodes() const;

private:
    BlockDevice& dev_;
    DiskSuperBlock sb_;
};

}  // namespace pfs
