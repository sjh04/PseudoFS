#pragma once

#include <cstdint>
#include <cstring>

#include "core/block_device.h"
#include "core/constants.h"

namespace pfs {

class SuperBlock;

// On-disk inode, 64 bytes, stored in the inode area (blocks 2..33).
// Each block holds INODES_PER_BLK (8) inodes.
struct DiskINode {
    uint16_t di_mode;         // file type + rwx permissions
    uint16_t di_nlink;        // hard link count
    uint16_t di_uid;          // owner user id
    uint16_t di_gid;          // owner group id
    uint32_t di_size;         // file size in bytes
    uint32_t di_atime;        // last access time
    uint32_t di_mtime;        // last modification time
    uint32_t di_ctime;        // creation time
    uint16_t di_addr[NADDR];  // block addresses (mixed indexing)
    uint8_t _pad[20];         // align to 64 bytes
};

static_assert(sizeof(DiskINode) == INODE_SIZE, "DiskINode must be 64 bytes");

// In-memory inode, used at runtime.
struct MemINode {
    DiskINode di;      // copy of disk inode
    uint16_t i_ino;    // inode number
    uint16_t i_count;  // reference count
    bool i_dirty;      // modified since last write-back
};

// Manages inode read/write on disk + in-memory cache + mixed indexing.
class InodeManager {
   public:
    InodeManager(BlockDevice& dev, SuperBlock& sb);

    // Read a disk inode into a MemINode. Caller must eventually call put().
    MemINode* get(uint16_t ino);

    // Decrement reference count. If zero and dirty, write back to disk.
    // If di_nlink==0, free all data blocks and the inode itself.
    void put(MemINode* ip);

    // Write a MemINode back to disk immediately.
    void write_back(MemINode* ip);

    // Allocate a new inode (calls sb.ialloc()), initialize it, return it.
    // Returns nullptr if no free inodes.
    MemINode* alloc(uint16_t mode, uint16_t uid, uint16_t gid);

    // --- Mixed indexing: map logical block offset to physical block ---

    // Given an inode and a logical block index within the file,
    // return the physical data block number (relative to data area).
    // Returns INVALID_BLK if not mapped.
    uint16_t bmap(MemINode* ip, uint32_t logical_blk);

    // Like bmap, but allocates blocks (including indirect blocks) if not
    // yet mapped. Used by write operations. Returns INVALID_BLK on failure.
    uint16_t bmap_alloc(MemINode* ip, uint32_t logical_blk);

    // Free all data blocks (and indirect blocks) owned by this inode.
    void truncate(MemINode* ip);

   private:
    BlockDevice& dev_;
    SuperBlock& sb_;

    // Cache: simple linear array. Production systems use a hash table,
    // but for 256 inodes this is sufficient.
    static constexpr int CACHE_SIZE = 64;
    MemINode cache_[CACHE_SIZE];
    bool cache_used_[CACHE_SIZE];

    // Disk I/O helpers
    void read_dinode(uint16_t ino, DiskINode& di);
    void write_dinode(uint16_t ino, const DiskINode& di);

    // Find a cache slot for inode ino, or an empty slot.
    MemINode* cache_lookup(uint16_t ino);
    MemINode* cache_alloc();

    // Indirect block helpers
    uint16_t read_indirect(uint16_t block_no, uint16_t index);
    void write_indirect(uint16_t block_no, uint16_t index, uint16_t value);
};

}  // namespace pfs
