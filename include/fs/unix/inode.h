#pragma once

#include <cstdint>
#include <cstring>

#include "core/block_device.h"
#include "core/constants.h"

namespace pfs {

class SuperBlock;

// 磁盘 inode,64 字节,存放在 inode 区(2..33 号块)。
// 每块装 INODES_PER_BLK(8)个 inode。
struct DiskINode {
    uint16_t di_mode;         // 文件类型 + rwx 权限
    uint16_t di_nlink;        // 硬链接计数
    uint16_t di_uid;          // 所有者用户号
    uint16_t di_gid;          // 所有者组号
    uint32_t di_size;         // 文件大小(字节)
    uint32_t di_atime;        // 最后访问时间
    uint32_t di_mtime;        // 最后修改时间
    uint32_t di_ctime;        // 创建时间
    uint16_t di_addr[NADDR];  // 块地址数组(混合索引):0..5 直接,6..7 一次间址,8 二次间址,9 预留
    uint8_t _pad[20];         // 对齐到 64 字节
};

static_assert(sizeof(DiskINode) == INODE_SIZE, "DiskINode must be 64 bytes");

// 内存 inode,运行时使用。
struct MemINode {
    DiskINode di;      // 磁盘 inode 副本
    uint16_t i_ino;    // inode 编号
    uint16_t i_count;  // 引用计数:当前有多少人正用着这个内存 inode(get 加、put 减,与磁盘上的 di_nlink 含义不同)
    bool i_dirty;      // 自上次写回后是否被修改
};

// inode 管理器:磁盘 inode 的读写、内存 inode 缓存、混合索引寻址都在这层。
class InodeManager {
   public:
    InodeManager(BlockDevice& dev, SuperBlock& sb);

    // 把一个磁盘 inode 读进内存 inode。调用方最终必须调用 put()。
    MemINode* get(uint16_t ino);

    // 引用计数减一。归零且脏则写回磁盘。
    // 若 di_nlink==0,释放全部数据块和 inode 自身。
    void put(MemINode* ip);

    // 立即把一个内存 inode 写回磁盘。
    void write_back(MemINode* ip);

    // 分配一个新 inode(调用 sb.ialloc()),初始化后返回。
    // 无空闲 inode 时返回 nullptr。
    MemINode* alloc(uint16_t mode, uint16_t uid, uint16_t gid);

    // --- 混合索引:把文件内的逻辑块偏移映射到物理块 ---

    // 给定一个 inode 和文件内的逻辑块号,
    // 返回物理数据块号(相对数据区)。
    // 未映射(文件空洞)时返回 INVALID_BLK。
    uint16_t bmap(MemINode* ip, uint32_t logical_blk);

    // 与 bmap 类似,但尚未映射时会分配块(含间址块)。
    // 写操作使用。失败时返回 INVALID_BLK。
    uint16_t bmap_alloc(MemINode* ip, uint32_t logical_blk);

    // 释放这个 inode 占用的全部数据块(以及间址块)。
    void truncate(MemINode* ip);

   private:
    BlockDevice& dev_;  // 底层块设备,读写 inode 区和间址块
    SuperBlock& sb_;    // 超级块,块/inode 的分配回收(balloc/bfree/ialloc/ifree)找它

    // 缓存:简单的线性数组。工业级系统用哈希表,
    // 但对 256 个 inode 来说这样够用了。
    static constexpr int CACHE_SIZE = 64;  // 缓存槽数
    MemINode cache_[CACHE_SIZE];           // 内存 inode 槽位
    bool cache_used_[CACHE_SIZE];          // 槽位占用标记

    // 磁盘 I/O 辅助
    void read_dinode(uint16_t ino, DiskINode& di);
    void write_dinode(uint16_t ino, const DiskINode& di);

    // 在缓存里找 inode 号为 ino 的槽,或找一个空槽。
    MemINode* cache_lookup(uint16_t ino);
    MemINode* cache_alloc();

    // 间址块辅助
    uint16_t read_indirect(uint16_t block_no, uint16_t index);
    void write_indirect(uint16_t block_no, uint16_t index, uint16_t value);
};

}  // namespace pfs
