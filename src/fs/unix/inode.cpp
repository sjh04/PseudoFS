#include "fs/unix/inode.h"

#include <cstring>
#include <ctime>

#include "fs/unix/super_block.h"

namespace pfs {

InodeManager::InodeManager(BlockDevice& dev, SuperBlock& sb) : dev_(dev), sb_(sb) {
    std::memset(cache_, 0, sizeof(cache_));
    std::memset(cache_used_, 0, sizeof(cache_used_));
}

// ---- Disk I/O ----

void InodeManager::read_dinode(uint16_t ino, DiskINode& di) {
    uint32_t block = INODE_START_BLK + ino / INODES_PER_BLK;
    uint32_t offset = (ino % INODES_PER_BLK) * INODE_SIZE;

    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(block, buf);
    std::memcpy(&di, buf + offset, sizeof(DiskINode));
}

void InodeManager::write_dinode(uint16_t ino, const DiskINode& di) {
    uint32_t block = INODE_START_BLK + ino / INODES_PER_BLK;
    uint32_t offset = (ino % INODES_PER_BLK) * INODE_SIZE;

    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(block, buf);
    std::memcpy(buf + offset, &di, sizeof(DiskINode));
    dev_.write_block(block, buf);
}

// ---- Cache ----

MemINode* InodeManager::cache_lookup(uint16_t ino) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache_used_[i] && cache_[i].i_ino == ino) {
            return &cache_[i];
        }
    }
    return nullptr;
}

MemINode* InodeManager::cache_alloc() {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!cache_used_[i]) {
            cache_used_[i] = true;
            std::memset(&cache_[i], 0, sizeof(MemINode));
            return &cache_[i];
        }
    }
    return nullptr;
}

// ---- Public API ----

MemINode* InodeManager::get(uint16_t ino) {
    if (ino >= INODE_TOTAL) {
        return nullptr;
    }

    MemINode* ip = cache_lookup(ino);
    if (ip != nullptr) {
        ip->i_count++;
        return ip;
    }

    ip = cache_alloc();
    if (ip == nullptr) {
        return nullptr;
    }

    read_dinode(ino, ip->di);
    ip->i_ino = ino;
    ip->i_count = 1;
    ip->i_dirty = false;
    return ip;
}

void InodeManager::put(MemINode* ip) {
    if (ip == nullptr) {
        return;
    }

    ip->i_count--;
    if (ip->i_count > 0) {
        return;
    }

    if (ip->di.di_nlink == 0) {
        truncate(ip);
        ip->di.di_mode = 0;
        ip->di.di_size = 0;
        ip->i_dirty = true;
        write_back(ip);
        sb_.ifree(ip->i_ino);
    } else if (ip->i_dirty) {
        write_back(ip);
    }

    // Release cache slot
    int idx = static_cast<int>(ip - cache_);
    if (idx >= 0 && idx < CACHE_SIZE) {
        cache_used_[idx] = false;
    }
}

void InodeManager::write_back(MemINode* ip) {
    if (ip == nullptr) {
        return;
    }
    write_dinode(ip->i_ino, ip->di);
    ip->i_dirty = false;
}

MemINode* InodeManager::alloc(uint16_t mode, uint16_t uid, uint16_t gid) {
    uint16_t ino = sb_.ialloc();
    if (ino == INVALID_BLK) {
        return nullptr;
    }

    MemINode* ip = cache_alloc();
    if (ip == nullptr) {
        sb_.ifree(ino);
        return nullptr;
    }

    std::memset(&ip->di, 0, sizeof(DiskINode));
    ip->di.di_mode = mode;
    ip->di.di_nlink = 1;
    ip->di.di_uid = uid;
    ip->di.di_gid = gid;
    ip->di.di_size = 0;
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    ip->di.di_atime = now;
    ip->di.di_mtime = now;
    ip->di.di_ctime = now;

    ip->i_ino = ino;
    ip->i_count = 1;
    ip->i_dirty = true;
    return ip;
}

// ---- Indirect block helpers ----

uint16_t InodeManager::read_indirect(uint16_t block_no, uint16_t index) {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(DATA_START_BLK + block_no, buf);
    auto* entries = reinterpret_cast<uint16_t*>(buf);
    return entries[index];
}

void InodeManager::write_indirect(uint16_t block_no, uint16_t index, uint16_t value) {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(DATA_START_BLK + block_no, buf);
    auto* entries = reinterpret_cast<uint16_t*>(buf);
    entries[index] = value;
    dev_.write_block(DATA_START_BLK + block_no, buf);
}

// ---- Mixed indexing ----
//
// addr[0..5]  : direct blocks             → up to 6 * 512 = 3KB
// addr[6..7]  : single-indirect blocks    → each points to a block of 256 uint16_t
//               → 2 * 256 * 512 = 256KB
// addr[8]     : double-indirect block     → points to a block of 256 single-indirect
//               pointers → 256 * 256 * 512 = 32MB
// addr[9]     : reserved (triple-indirect, not implemented)

uint16_t InodeManager::bmap(MemINode* ip, uint32_t logical_blk) {
    if (logical_blk < DIRECT_ADDR) {
        // Direct block
        return ip->di.di_addr[logical_blk];
    }

    logical_blk -= DIRECT_ADDR;

    // Single-indirect: addr[6] and addr[7]
    for (int i = 0; i < 2; i++) {
        if (logical_blk < PTRS_PER_BLK) {
            uint16_t indirect_blk = ip->di.di_addr[DIRECT_ADDR + i];
            if (indirect_blk == 0) {
                return 0;
            }
            return read_indirect(indirect_blk, logical_blk);
        }
        logical_blk -= PTRS_PER_BLK;
    }

    // Double-indirect: addr[8]
    if (logical_blk < PTRS_PER_BLK * PTRS_PER_BLK) {
        uint16_t dbl_blk = ip->di.di_addr[8];
        if (dbl_blk == 0) {
            return 0;
        }
        uint16_t idx1 = logical_blk / PTRS_PER_BLK;
        uint16_t idx2 = logical_blk % PTRS_PER_BLK;
        uint16_t sgl_blk = read_indirect(dbl_blk, idx1);
        if (sgl_blk == 0) {
            return 0;
        }
        return read_indirect(sgl_blk, idx2);
    }

    return 0;
}

uint16_t InodeManager::bmap_alloc(MemINode* ip, uint32_t logical_blk) {
    if (logical_blk < DIRECT_ADDR) {
        if (ip->di.di_addr[logical_blk] == 0) {
            uint16_t blk = sb_.balloc();
            if (blk == INVALID_BLK) {
                return INVALID_BLK;
            }
            ip->di.di_addr[logical_blk] = blk;
            ip->i_dirty = true;
        }
        return ip->di.di_addr[logical_blk];
    }

    logical_blk -= DIRECT_ADDR;

    // Single-indirect
    for (int i = 0; i < 2; i++) {
        if (logical_blk < PTRS_PER_BLK) {
            uint16_t addr_idx = DIRECT_ADDR + i;
            if (ip->di.di_addr[addr_idx] == 0) {
                uint16_t blk = sb_.balloc();
                if (blk == INVALID_BLK) {
                    return INVALID_BLK;
                }
                ip->di.di_addr[addr_idx] = blk;
                // Zero out the new indirect block
                uint8_t zero[BLOCK_SIZE] = {};
                dev_.write_block(DATA_START_BLK + blk, zero);
                ip->i_dirty = true;
            }

            uint16_t indirect_blk = ip->di.di_addr[addr_idx];
            uint16_t data_blk = read_indirect(indirect_blk, logical_blk);
            if (data_blk == 0) {
                data_blk = sb_.balloc();
                if (data_blk == INVALID_BLK) {
                    return INVALID_BLK;
                }
                write_indirect(indirect_blk, logical_blk, data_blk);
            }
            return data_blk;
        }
        logical_blk -= PTRS_PER_BLK;
    }

    // Double-indirect
    if (logical_blk < PTRS_PER_BLK * PTRS_PER_BLK) {
        if (ip->di.di_addr[8] == 0) {
            uint16_t blk = sb_.balloc();
            if (blk == INVALID_BLK) {
                return INVALID_BLK;
            }
            ip->di.di_addr[8] = blk;
            uint8_t zero[BLOCK_SIZE] = {};
            dev_.write_block(DATA_START_BLK + blk, zero);
            ip->i_dirty = true;
        }

        uint16_t dbl_blk = ip->di.di_addr[8];
        uint16_t idx1 = logical_blk / PTRS_PER_BLK;
        uint16_t idx2 = logical_blk % PTRS_PER_BLK;

        uint16_t sgl_blk = read_indirect(dbl_blk, idx1);
        if (sgl_blk == 0) {
            sgl_blk = sb_.balloc();
            if (sgl_blk == INVALID_BLK) {
                return INVALID_BLK;
            }
            write_indirect(dbl_blk, idx1, sgl_blk);
            uint8_t zero[BLOCK_SIZE] = {};
            dev_.write_block(DATA_START_BLK + sgl_blk, zero);
        }

        uint16_t data_blk = read_indirect(sgl_blk, idx2);
        if (data_blk == 0) {
            data_blk = sb_.balloc();
            if (data_blk == INVALID_BLK) {
                return INVALID_BLK;
            }
            write_indirect(sgl_blk, idx2, data_blk);
        }
        return data_blk;
    }

    return INVALID_BLK;
}

void InodeManager::truncate(MemINode* ip) {
    // Free direct blocks
    for (int i = 0; i < DIRECT_ADDR; i++) {
        if (ip->di.di_addr[i] != 0) {
            sb_.bfree(ip->di.di_addr[i]);
            ip->di.di_addr[i] = 0;
        }
    }

    // Free single-indirect blocks
    for (int i = 0; i < 2; i++) {
        uint16_t indirect_blk = ip->di.di_addr[DIRECT_ADDR + i];
        if (indirect_blk == 0) {
            continue;
        }
        uint8_t buf[BLOCK_SIZE];
        dev_.read_block(DATA_START_BLK + indirect_blk, buf);
        auto* entries = reinterpret_cast<uint16_t*>(buf);
        for (uint32_t j = 0; j < PTRS_PER_BLK; j++) {
            if (entries[j] != 0) {
                sb_.bfree(entries[j]);
            }
        }
        sb_.bfree(indirect_blk);
        ip->di.di_addr[DIRECT_ADDR + i] = 0;
    }

    // Free double-indirect block
    uint16_t dbl_blk = ip->di.di_addr[8];
    if (dbl_blk != 0) {
        uint8_t buf1[BLOCK_SIZE];
        dev_.read_block(DATA_START_BLK + dbl_blk, buf1);
        auto* lvl1 = reinterpret_cast<uint16_t*>(buf1);
        for (uint32_t i = 0; i < PTRS_PER_BLK; i++) {
            if (lvl1[i] == 0) {
                continue;
            }
            uint8_t buf2[BLOCK_SIZE];
            dev_.read_block(DATA_START_BLK + lvl1[i], buf2);
            auto* lvl2 = reinterpret_cast<uint16_t*>(buf2);
            for (uint32_t j = 0; j < PTRS_PER_BLK; j++) {
                if (lvl2[j] != 0) {
                    sb_.bfree(lvl2[j]);
                }
            }
            sb_.bfree(lvl1[i]);
        }
        sb_.bfree(dbl_blk);
        ip->di.di_addr[8] = 0;
    }

    ip->di.di_size = 0;
    ip->i_dirty = true;
}

}  // namespace pfs
