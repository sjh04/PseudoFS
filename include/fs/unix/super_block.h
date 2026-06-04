#pragma once

#include <cstdint>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"

namespace pfs {

// 磁盘超级块布局,存放在 SUPER_BLK 块(1 号块)。
// 空闲块栈采用课件里的成组链接法:
//   s_free[0..s_nfree-1] 存放块号。
//   当 s_nfree==1 时,s_free[0] 是指向下一组的链接(0 = 链尾)。
//   分配从栈顶弹出(s_nfree-1),回收压回栈顶。
struct DiskSuperBlock {
    uint16_t s_isize;           // inode 区块数
    uint16_t s_fsize;           // 数据块总数
    uint16_t s_nfree;           // 空闲块栈中的项数
    uint16_t s_free[NICFREE];   // 空闲块栈(成组链接法的内存组)
    uint16_t s_ninode;          // 空闲 inode 栈中的项数
    uint16_t s_inode[NICINOD];  // 空闲 inode 栈
    uint16_t s_rinode;          // 供线性回扫用的记忆 inode 号
    uint16_t s_free_total;      // 空闲数据块总数
    uint16_t s_inode_total;     // 空闲 inode 总数
    uint16_t s_root_ino;        // 根目录 inode 号
    uint8_t s_fmod;             // 修改标志
};

// 超级块管理器:空闲数据块(成组链接法)和空闲 inode(栈)都归它管。
// 内存里持有一份超级块副本 sb_,format()/load() 填充它,flush() 把它落盘。
class SuperBlock {
   public:
    explicit SuperBlock(BlockDevice& dev);

    // 在新盘上建立空闲块链和空闲 inode 栈,在 format 时调用。
    // 假定数据块 0..DATA_BLK_NUM-1 全部空闲,
    // 除了调用方预先占用的那些(根目录等)。
    void format(uint16_t reserved_blocks, uint16_t reserved_inodes);

    // 从磁盘把超级块读进内存。
    void load();

    // 把超级块写回磁盘。
    void flush();

    // 分配一个空闲数据块。返回相对数据区的块号(从 0 起),
    // 盘满时返回 INVALID_BLK。
    uint16_t balloc();

    // 回收一个数据块(块号相对数据区)。
    void bfree(uint16_t block_no);

    // 分配一个空闲 inode 号。返回 inode 号(从 0 起),
    // 无空闲 inode 时返回 INVALID_BLK。
    uint16_t ialloc();

    // 回收一个 inode 号。
    void ifree(uint16_t inode_no);

    // 只读地遍历成组链,枚举出所有空闲数据块(从 0 起)。
    // 供诊断 / 磁盘可视化用。一致的磁盘上结果数量等于 free_block_count()。
    void collect_free_blocks(std::vector<uint16_t>& out);

    // 访问器
    uint16_t free_block_count() const;
    uint16_t free_inode_count() const;
    uint16_t total_blocks() const;
    uint16_t total_inodes() const;
    uint16_t root_inode() const;
    void set_root_inode(uint16_t ino);

   private:
    BlockDevice& dev_;
    DiskSuperBlock sb_;
};

}  // namespace pfs
