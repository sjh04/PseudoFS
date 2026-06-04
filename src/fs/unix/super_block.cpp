#include "fs/unix/super_block.h"

#include <cstring>

namespace pfs {

// 超级块管理器:空闲数据块(成组链接法)和空闲 inode(栈)都归它管。
// 构造时只清零内存副本,内容靠 format()(新盘)或 load()(旧盘)填充。
SuperBlock::SuperBlock(BlockDevice& dev) : dev_(dev) {
    std::memset(&sb_, 0, sizeof(sb_));
}

// 格式化超级块这一层:建空闲块链(成组链接法)+ 建空闲 inode 栈。
// 根目录由上层 UnixFS::format 创建,不在这里。
void SuperBlock::format(uint16_t reserved_blocks, uint16_t reserved_inodes) {
    std::memset(&sb_, 0, sizeof(sb_));
    sb_.s_isize = INODE_BLK_NUM;
    sb_.s_fsize = DATA_BLK_NUM;
    sb_.s_fmod = 1;

    // 用成组链接法组织空闲块(对应课件 PPT 17-26 页)。
    //
    // 整条链是一串"组",每组的存储格式:
    //   [count, entry[0], entry[1], ..., entry[count-1]]
    // entry[0] 是指向前一组的链接(0 = 链尾),
    // entry[1..count-1] 是空闲块号。
    //
    // 磁盘上的每一组写在一个"存储块"里,而存储块本身也是空闲块:
    // balloc() 顺着链接把下一组读进内存后,这个存储块自己就作为
    // 分配结果返回,一点不浪费。
    //
    // 所以磁盘上每组实际记账 NICFREE 块:
    //   1 个存储块 + (NICFREE-1) 个表项 = NICFREE
    // 超级块里的内存组记账 s_nfree 块(它不占存储块)。
    //
    // 从低块号往高块号建,链是自底向上串起来的。

    // [0, reserved_blocks) 这几块永远不进空闲链,balloc() 不可能发出 0 号块,
    // 所以 inode 地址数组里 addr[i] == 0 可以安全地表示"未分配"。
    uint16_t total_free = DATA_BLK_NUM - reserved_blocks;
    uint16_t cursor = reserved_blocks;
    uint16_t prev_link = END_OF_CHAIN;

    uint8_t blk_buf[BLOCK_SIZE];

    // 建磁盘组:每组消耗 NICFREE 块(1 存储块 + NICFREE-1 个表项),
    // 只要还剩至少一整组就继续建。余下的 0..NICFREE-1 块进下面的
    // 超级块内存组——内存组要装 s_free[0] 链接 + 至多 NICFREE-1 个
    // 空闲块,s_nfree 才不会超过 NICFREE。
    // (若 total_free 恰为 NICFREE 整数倍时不这样处理,这里会剩
    // NICFREE 块,s_nfree = NICFREE+1,直接撑爆 s_free[]。
    // 见测试 FormatFreeListIntactAtGroupBoundary。)
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

    // 剩余的块进超级块的内存栈。
    // entry[0] = 指向最后一个磁盘组的链接(一个磁盘组都没有则为 END_OF_CHAIN)。
    uint16_t remaining = total_free - (cursor - reserved_blocks);
    sb_.s_nfree = remaining + 1;
    sb_.s_free[0] = prev_link;
    for (uint16_t i = 0; i < remaining; i++) {
        sb_.s_free[1 + i] = cursor++;
    }

    sb_.s_free_total = total_free;

    // 建空闲 inode 栈:跳过保留 inode,按号顺序装,至多 NICINOD 个
    sb_.s_ninode = 0;
    for (uint16_t i = reserved_inodes; i < INODE_TOTAL && sb_.s_ninode < NICINOD; i++) {
        sb_.s_inode[sb_.s_ninode++] = i;
    }
    sb_.s_rinode = reserved_inodes + sb_.s_ninode;
    sb_.s_inode_total = INODE_TOTAL - reserved_inodes;

    flush();
}

// 挂载已有磁盘:从 SUPER_BLK 块读回超级块,空闲链/inode 栈随之恢复
void SuperBlock::load() {
    uint8_t buf[BLOCK_SIZE];
    dev_.read_block(SUPER_BLK, buf);
    std::memcpy(&sb_, buf, sizeof(sb_));
}

// 把内存中的超级块写回磁盘并清除修改标志,sync 时落盘的就是它
void SuperBlock::flush() {
    uint8_t buf[BLOCK_SIZE];
    std::memset(buf, 0, BLOCK_SIZE);
    std::memcpy(buf, &sb_, sizeof(sb_));
    dev_.write_block(SUPER_BLK, buf);
    sb_.s_fmod = 0;
}

// 分配一个空闲块(成组链接法的核心,必画图 #2)。
// 常规情况:栈顶弹一个,纯内存操作 O(1)。
// 边界情况:栈里只剩链接项时,把下一组从磁盘读进栈,
// 然后把存放该组的链块本身作为分配结果返回——一次磁盘读补充约 50 项。
uint16_t SuperBlock::balloc() {
    if (sb_.s_nfree == 0) {
        return INVALID_BLK;
    }

    if (sb_.s_nfree == 1) {
        // 只剩链接项了。s_free[0] 指向磁盘上的下一组
        // (若为 END_OF_CHAIN,说明这已是最后一组,盘真满了)。
        uint16_t link_block = sb_.s_free[0];
        if (link_block == END_OF_CHAIN) {
            sb_.s_nfree = 0;
            return INVALID_BLK;
        }
        // 把下一组读进内存栈,然后返回链块本身
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

// 回收一个块,balloc 的镜像操作。
// 常规情况:压回栈顶,O(1)。
// 边界情况:栈满(NICFREE 项)时,把整栈写进这个刚释放的块,
// 让它成为新的链头存储块,内存栈重置为只含它一项。
void SuperBlock::bfree(uint16_t block_no) {
    if (sb_.s_nfree == NICFREE) {
        // 栈满:当前整栈写进被释放的块,栈重置为只含这个块
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

// 分配一个空闲 inode:从超级块的空闲 inode 栈直接弹出
uint16_t SuperBlock::ialloc() {
    if (sb_.s_ninode == 0) {
        // 栈空了。按 UNIX 的设计应从 s_rinode 开始回扫磁盘 inode 区,
        // 把 di_mode==0 的空闲 inode 补进栈;当前没实现回扫,直接返回
        // 失败,调用方必须检查返回值。(已知局限,答辩可主动提)
        return INVALID_BLK;
    }
    sb_.s_ninode--;
    uint16_t ino = sb_.s_inode[sb_.s_ninode];
    sb_.s_inode_total--;
    sb_.s_fmod = 1;
    return ino;
}

// 回收一个 inode:栈没满就压栈;栈满则只更新 s_rinode
void SuperBlock::ifree(uint16_t inode_no) {
    sb_.s_inode_total++;
    if (sb_.s_ninode < NICINOD) {
        sb_.s_inode[sb_.s_ninode] = inode_no;
        sb_.s_ninode++;
    } else {
        // 栈装不下了。记住更小的空闲 inode 号给将来的回扫当起点,
        // inode 本身不会丢(磁盘上它的 di_mode 已清零,回扫能找到)。
        if (inode_no < sb_.s_rinode) {
            sb_.s_rinode = inode_no;
        }
    }
    sb_.s_fmod = 1;
}

// 遍历整条空闲链收集所有空闲块号,给 TUI 的 F3 磁盘位图用。
// 也正好暴露成组链的局限:想知道某块是否空闲,只能 O(n) 走完整条链。
void SuperBlock::collect_free_blocks(std::vector<uint16_t>& out) {
    out.clear();
    // 内存组:s_free[1..s_nfree-1] 是空闲块,
    // s_free[0] 是指向磁盘下一组的链接(或 END_OF_CHAIN)
    for (uint16_t i = 1; i < sb_.s_nfree; i++) {
        out.push_back(sb_.s_free[i]);
    }

    uint16_t link = (sb_.s_nfree >= 1) ? sb_.s_free[0] : END_OF_CHAIN;
    uint8_t buf[BLOCK_SIZE];
    // 用数据区块数做步数上限,防止链被写坏成环时死循环
    for (uint32_t guard = 0; link != END_OF_CHAIN && guard < DATA_BLK_NUM; ++guard) {
        out.push_back(link);  // 存储块本身也是空闲块
        dev_.read_block(DATA_START_BLK + link, buf);
        auto* grp = reinterpret_cast<uint16_t*>(buf);
        uint16_t count = grp[0];
        uint16_t next = grp[1];                    // entry[0] = 指向下一组的链接
        if (count == 0 || count > NICFREE) break;  // 组数据损坏
        for (uint16_t i = 2; i <= count; i++) {    // entry[1..count-1] 是空闲块号
            out.push_back(grp[i]);
        }
        link = next;
    }
}

// 当前空闲数据块总数(s_free_total 全程精确维护,查询 O(1))
uint16_t SuperBlock::free_block_count() const {
    return sb_.s_free_total;
}

// 当前空闲 inode 总数
uint16_t SuperBlock::free_inode_count() const {
    return sb_.s_inode_total;
}

// 数据区总块数
uint16_t SuperBlock::total_blocks() const {
    return sb_.s_fsize;
}

// inode 总数(256)
uint16_t SuperBlock::total_inodes() const {
    return INODE_TOTAL;
}

// 根目录的 inode 号,挂载后一切路径解析的起点
uint16_t SuperBlock::root_inode() const {
    return sb_.s_root_ino;
}

// 记录根目录 inode 号,UnixFS::format 建完根目录后回填
void SuperBlock::set_root_inode(uint16_t ino) {
    sb_.s_root_ino = ino;
    sb_.s_fmod = 1;
}

}  // namespace pfs
