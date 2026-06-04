#include "fs/unix/inode.h"

#include <cstring>
#include <ctime>

#include "fs/unix/super_block.h"

namespace pfs {

// inode 管理器:磁盘 inode 的读写、内存 inode 缓存、混合索引寻址都在这层
InodeManager::InodeManager(BlockDevice& dev, SuperBlock& sb) : dev_(dev), sb_(sb) {
    std::memset(cache_, 0, sizeof(cache_));
    std::memset(cache_used_, 0, sizeof(cache_used_));
}

// ---- 磁盘 inode 读写 ----

// 按 inode 号定位磁盘位置:每块装 8 个 inode(512/64),
// 块号 = inode 区起始 + ino/8,块内偏移 = (ino%8)*64
void InodeManager::read_dinode(uint16_t ino, DiskINode& di) {
    uint32_t block = INODE_START_BLK + ino / INODES_PER_BLK;
    uint32_t offset = (ino % INODES_PER_BLK) * INODE_SIZE;

    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(block, buf);
    std::memcpy(&di, buf + offset, sizeof(DiskINode));
}

// 写回一个磁盘 inode。磁盘只支持按块读写,所以改 64 字节
// 也得"读整块 → 改其中一段 → 写整块"
void InodeManager::write_dinode(uint16_t ino, const DiskINode& di) {
    uint32_t block = INODE_START_BLK + ino / INODES_PER_BLK;
    uint32_t offset = (ino % INODES_PER_BLK) * INODE_SIZE;

    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(block, buf);
    std::memcpy(buf + offset, &di, sizeof(DiskINode));
    dev_.write_block(block, buf);
}

// ---- 内存 inode 缓存 ----

// 在缓存里线性查找指定 inode 号,未命中返回 nullptr
MemINode* InodeManager::cache_lookup(uint16_t ino) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache_used_[i] && cache_[i].i_ino == ino) {
            return &cache_[i];
        }
    }
    return nullptr;
}

// 占一个空闲缓存槽并清零。全占满返回 nullptr(已知局限:没有 LRU 淘汰)
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

// ---- 对外接口 ----

// 两套计数,两个含义:i_count = 内存引用数(当前有多少人正用着这个 MemINode),
// di_nlink = 磁盘上的硬链接数。get/put 管 i_count,link/unlink 管 di_nlink。
//
// get(即 UNIX 的 iget):按 inode 号取内存 inode。
// 缓存命中只把 i_count 加一;未命中则占一个槽从磁盘读入,i_count = 1。
// 同一文件无论被打开多少次,内存里始终只有一份 MemINode。
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

// put(即 UNIX 的 iput):归还一个引用,i_count 减一,
// 清零后视情况写回或销毁,最后释放缓存槽
void InodeManager::put(MemINode* ip) {
    if (ip == nullptr) {
        return;
    }

    ip->i_count--;
    if (ip->i_count > 0) {
        return;  // 还有人在用(比如另一个打开的 fd),先不回收
    }

    // 最后一个引用也放掉了。若此时 nlink 也是 0,说明文件是在打开状态下被
    // unlink 的——直到此刻才真正销毁:释放全部数据块、归还 inode。
    // 这就是 rm 一个还 open 着的文件不会出错的原因。
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

    // 释放缓存槽
    int idx = static_cast<int>(ip - cache_);
    if (idx >= 0 && idx < CACHE_SIZE) {
        cache_used_[idx] = false;
    }
}

// 把内存 inode 的修改写回磁盘 inode 区,并清脏标志
void InodeManager::write_back(MemINode* ip) {
    if (ip == nullptr) {
        return;
    }
    write_dinode(ip->i_ino, ip->di);
    ip->i_dirty = false;
}

// 创建新文件/目录时分配全新 inode:先从超级块拿空闲 inode 号,
// 再占缓存槽,初始化元数据(mode/uid/gid/时间戳),nlink = 1,标脏。
// 注意失败回滚:拿到了 ino 但缓存满,要把 ino 还回去。
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

// ---- 间址块辅助 ----

// 读间址块的第 index 项。间址块就是一个装了 256 个 uint16 块号的数据块
uint16_t InodeManager::read_indirect(uint16_t block_no, uint16_t index) {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(DATA_START_BLK + block_no, buf);
    auto* entries = reinterpret_cast<uint16_t*>(buf);
    return entries[index];
}

// 改间址块的第 index 项:读整块、改 2 字节、写回
void InodeManager::write_indirect(uint16_t block_no, uint16_t index, uint16_t value) {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(DATA_START_BLK + block_no, buf);
    auto* entries = reinterpret_cast<uint16_t*>(buf);
    entries[index] = value;
    dev_.write_block(DATA_START_BLK + block_no, buf);
}

// ---- 混合索引 ----
//
// addr[0..5] : 直接索引            → 6 × 512 = 3KB
// addr[6..7] : 一次间址,每项指向一个装 256 个 uint16 块号的间址块
//              → 2 × 256 × 512 = 256KB
// addr[8]    : 二次间址            → 256 × 256 × 512 = 32MB
// addr[9]    : 预留(三次间址,未实现)
//
// 这张表就是必答题 Q4「最大文件长度」的答案:3KB + 256KB + 32MB。

// bmap:逻辑块号 → 物理块号(读路径,必画图 #3)。
// 逐级判断:先减掉前面区间覆盖的块数,再在本级查表。
// 任何一级查到 0 都说明文件这里是"洞",返回 0,上层读到全零。
uint16_t InodeManager::bmap(MemINode* ip, uint32_t logical_blk) {
    if (logical_blk < DIRECT_ADDR) {
        // 直接索引:inode 里直接存着块号
        return ip->di.di_addr[logical_blk];
    }

    logical_blk -= DIRECT_ADDR;

    // 一次间址:addr[6] 和 addr[7],多读一次盘
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

    // 二次间址:addr[8],两层查表,多读两次盘
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

// bmap 的写路径版本:沿途哪一级缺块就 balloc 哪一级。
// 新分配的间址块必须清零——0 表示"未分配",不能留脏数据。
uint16_t InodeManager::bmap_alloc(MemINode* ip, uint32_t logical_blk) {
    if (logical_blk < DIRECT_ADDR) {
        // 直接索引:缺块就分配,块号记进 addr[]
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

    // 一次间址:间址块本身缺了也要先分配
    for (int i = 0; i < 2; i++) {
        if (logical_blk < PTRS_PER_BLK) {
            uint16_t addr_idx = DIRECT_ADDR + i;
            if (ip->di.di_addr[addr_idx] == 0) {
                uint16_t blk = sb_.balloc();
                if (blk == INVALID_BLK) {
                    return INVALID_BLK;
                }
                ip->di.di_addr[addr_idx] = blk;
                // 新间址块清零,保证未写过的项都是 0(未分配)
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

    // 二次间址:最多沿途补三个块(二级间址块、一级间址块、数据块)
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

// 释放文件占用的全部块。必须逐级下钻:先放叶子数据块,再放间址块本身——
// 只清 addr[] 不下钻的话,间址块和它指向的所有块都会泄漏。
void InodeManager::truncate(MemINode* ip) {
    // 直接块:挨个放掉
    for (int i = 0; i < DIRECT_ADDR; i++) {
        if (ip->di.di_addr[i] != 0) {
            sb_.bfree(ip->di.di_addr[i]);
            ip->di.di_addr[i] = 0;
        }
    }

    // 一次间址:先放它指向的数据块,最后放间址块自己
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

    // 二次间址:两层下钻,数据块 → 一级间址块 → 二级间址块,由内向外放
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
