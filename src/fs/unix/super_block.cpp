#include "fs/unix/super_block.h"

#include <cstring>

namespace pfs {

SuperBlock::SuperBlock(BlockDevice& dev) : dev_(dev) {
    std::memset(&sb_, 0, sizeof(sb_));
}

void SuperBlock::format(uint16_t reserved_blocks, uint16_t reserved_inodes) {
    std::memset(&sb_, 0, sizeof(sb_));
    sb_.s_isize = INODE_BLK_NUM;
    sb_.s_fsize = DATA_BLK_NUM;
    sb_.s_fmod = 1;

    // Build the free block chain using group-linked method (PPT slides 17-26).
    //
    // The chain is a linked list of "groups". Each group is stored as:
    //   [count, entry[0], entry[1], ..., entry[count-1]]
    // where entry[0] is the link to the previous group (0 = end of chain),
    // and entry[1..count-1] are free block numbers.
    //
    // Each disk group is written to a "storage block" which is itself a free
    // block. When balloc() follows a link, it reads the storage block to load
    // the group, then returns the storage block as a free block.
    //
    // So each disk group accounts for NICFREE blocks:
    //   1 storage block + (NICFREE-1) entries = NICFREE
    // The superblock group accounts for s_nfree blocks (no storage block).
    //
    // We build from low block numbers to high, bottom-up in the chain.

    uint16_t total_free = DATA_BLK_NUM - reserved_blocks;
    uint16_t cursor = reserved_blocks;
    uint16_t prev_link = END_OF_CHAIN;

    uint8_t blk_buf[BLOCK_SIZE];

    // Build disk groups: each consumes NICFREE blocks from the free list
    // (1 storage block + (NICFREE-1) free-block entries). Keep building while at
    // least a full group remains. Whatever is left (0..NICFREE-1 blocks) goes
    // into the superblock's in-memory group below, which holds the link in
    // s_free[0] plus up to NICFREE-1 free blocks — so s_nfree stays <= NICFREE.
    // (Stopping one group early when total_free is a multiple of NICFREE would
    // leave NICFREE blocks here, making s_nfree = NICFREE+1 and overflowing
    // s_free[]. See test FormatFreeListIntactAtGroupBoundary.)
    while (total_free - (cursor - reserved_blocks) >= NICFREE) {
        uint16_t storage_block = cursor++;

        std::memset(blk_buf, 0, BLOCK_SIZE);
        auto* grp = reinterpret_cast<uint16_t*>(blk_buf);
        grp[0] = NICFREE;
        grp[1] = prev_link;
        for (uint16_t i = 1; i < NICFREE; i++) {
            grp[1 + i] = cursor++;
        }

        dev_.write_block(DATA_START_BLK + storage_block, blk_buf);
        prev_link = storage_block;
    }

    // Remaining blocks go into the superblock's in-memory stack.
    // entry[0] = link to the last disk group (or END_OF_CHAIN if none).
    uint16_t remaining = total_free - (cursor - reserved_blocks);
    sb_.s_nfree = remaining + 1;
    sb_.s_free[0] = prev_link;
    for (uint16_t i = 0; i < remaining; i++) {
        sb_.s_free[1 + i] = cursor++;
    }

    sb_.s_free_total = total_free;

    // Build free inode stack: fill with inode numbers starting after reserved
    sb_.s_ninode = 0;
    for (uint16_t i = reserved_inodes; i < INODE_TOTAL && sb_.s_ninode < NICINOD; i++) {
        sb_.s_inode[sb_.s_ninode++] = i;
    }
    sb_.s_rinode = reserved_inodes + sb_.s_ninode;
    sb_.s_inode_total = INODE_TOTAL - reserved_inodes;

    flush();
}

void SuperBlock::load() {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(SUPER_BLK, buf);
    std::memcpy(&sb_, buf, sizeof(sb_));
}

void SuperBlock::flush() {
    uint8_t buf[BLOCK_SIZE];
    std::memset(buf, 0, BLOCK_SIZE);
    std::memcpy(buf, &sb_, sizeof(sb_));
    dev_.write_block(SUPER_BLK, buf);
    sb_.s_fmod = 0;
}

uint16_t SuperBlock::balloc() {
    if (sb_.s_nfree == 0) {
        return INVALID_BLK;
    }

    if (sb_.s_nfree == 1) {
        // Only the link entry remains. s_free[0] points to the next group
        // on disk (or END_OF_CHAIN if this is the last group).
        uint16_t link_block = sb_.s_free[0];
        if (link_block == END_OF_CHAIN) {
            sb_.s_nfree = 0;
            return INVALID_BLK;
        }
        // Load the next group from disk, then return the link block itself.
        uint8_t buf[BLOCK_SIZE];
        dev_.read_block(DATA_START_BLK + link_block, buf);
        auto* grp = reinterpret_cast<uint16_t*>(buf);
        sb_.s_nfree = grp[0];
        for (uint16_t i = 0; i < sb_.s_nfree; i++) {
            sb_.s_free[i] = grp[1 + i];
        }
        sb_.s_free_total--;
        sb_.s_fmod = 1;
        return link_block;
    }

    sb_.s_nfree--;
    uint16_t block_no = sb_.s_free[sb_.s_nfree];
    sb_.s_free_total--;
    sb_.s_fmod = 1;
    return block_no;
}

void SuperBlock::bfree(uint16_t block_no) {
    if (sb_.s_nfree == NICFREE) {
        // Stack full: write current stack into the freed block,
        // then reset the stack to contain only this block.
        uint8_t buf[BLOCK_SIZE];
        std::memset(buf, 0, BLOCK_SIZE);
        auto* grp = reinterpret_cast<uint16_t*>(buf);
        grp[0] = sb_.s_nfree;
        for (uint16_t i = 0; i < sb_.s_nfree; i++) {
            grp[1 + i] = sb_.s_free[i];
        }
        dev_.write_block(DATA_START_BLK + block_no, buf);

        sb_.s_nfree = 1;
        sb_.s_free[0] = block_no;
    } else {
        sb_.s_free[sb_.s_nfree] = block_no;
        sb_.s_nfree++;
    }
    sb_.s_free_total++;
    sb_.s_fmod = 1;
}

uint16_t SuperBlock::ialloc() {
    if (sb_.s_ninode == 0) {
        // Refill the free inode stack by scanning the inode area on disk.
        // Start from s_rinode (the remembered position).
        // In a real implementation we'd read disk inodes and check di_mode==0.
        // For now, this situation means all cached inodes are used.
        // The caller should check the return value.
        return INVALID_BLK;
    }
    sb_.s_ninode--;
    uint16_t ino = sb_.s_inode[sb_.s_ninode];
    sb_.s_inode_total--;
    sb_.s_fmod = 1;
    return ino;
}

void SuperBlock::ifree(uint16_t inode_no) {
    sb_.s_inode_total++;
    if (sb_.s_ninode < NICINOD) {
        sb_.s_inode[sb_.s_ninode] = inode_no;
        sb_.s_ninode++;
    } else {
        // Stack full. If this inode number is smaller than s_rinode,
        // update s_rinode so the next scan will find it.
        if (inode_no < sb_.s_rinode) {
            sb_.s_rinode = inode_no;
        }
    }
    sb_.s_fmod = 1;
}

void SuperBlock::collect_free_blocks(std::vector<uint16_t>& out) {
    out.clear();
    // In-memory group: s_free[1..s_nfree-1] are free blocks; s_free[0] is the
    // link to the next on-disk group (or END_OF_CHAIN).
    for (uint16_t i = 1; i < sb_.s_nfree; i++) {
        out.push_back(sb_.s_free[i]);
    }

    uint16_t link = (sb_.s_nfree >= 1) ? sb_.s_free[0] : END_OF_CHAIN;
    uint8_t buf[BLOCK_SIZE];
    // Bound the walk by the data-block count to survive a corrupt/cyclic chain.
    for (uint32_t guard = 0; link != END_OF_CHAIN && guard < DATA_BLK_NUM;
         ++guard) {
        out.push_back(link);  // the storage block itself is a free block
        dev_.read_block(DATA_START_BLK + link, buf);
        auto* grp = reinterpret_cast<uint16_t*>(buf);
        uint16_t count = grp[0];
        uint16_t next = grp[1];  // entry[0] = link to next group
        if (count == 0 || count > NICFREE) break;  // corrupt group
        for (uint16_t i = 2; i <= count; i++) {     // entry[1..count-1]
            out.push_back(grp[i]);
        }
        link = next;
    }
}

uint16_t SuperBlock::free_block_count() const {
    return sb_.s_free_total;
}

uint16_t SuperBlock::free_inode_count() const {
    return sb_.s_inode_total;
}

uint16_t SuperBlock::total_blocks() const {
    return sb_.s_fsize;
}

uint16_t SuperBlock::total_inodes() const {
    return INODE_TOTAL;
}

uint16_t SuperBlock::root_inode() const {
    return sb_.s_root_ino;
}

void SuperBlock::set_root_inode(uint16_t ino) {
    sb_.s_root_ino = ino;
    sb_.s_fmod = 1;
}

}  // namespace pfs
