#pragma once

#include <cstdint>

namespace pfs {

constexpr uint32_t BLOCK_SIZE = 512;  // 块大小(字节),磁盘 I/O 的最小单位

// 磁盘布局(块号)
constexpr uint32_t BOOT_BLK = 0;         // 引导块(预留)
constexpr uint32_t SUPER_BLK = 1;        // 超级块
constexpr uint32_t INODE_START_BLK = 2;  // inode 区起始块号
constexpr uint32_t INODE_BLK_NUM = 32;   // inode 区块数
constexpr uint32_t DATA_START_BLK = INODE_START_BLK + INODE_BLK_NUM;  // 数据区起始块号 = 34
constexpr uint32_t DATA_BLK_NUM = 2048;                               // 数据区块数
constexpr uint32_t TOTAL_BLK_NUM = DATA_START_BLK + DATA_BLK_NUM;     // 全盘总块数 = 2082

// inode
constexpr uint32_t INODE_SIZE = 64;                               // 每个磁盘 inode 占的字节数
constexpr uint32_t INODES_PER_BLK = BLOCK_SIZE / INODE_SIZE;      // 每块装的 inode 数 = 8
constexpr uint32_t INODE_TOTAL = INODE_BLK_NUM * INODES_PER_BLK;  // inode 总数 = 256
constexpr uint32_t NADDR = 10;                                    // 块地址数组 di_addr[] 的长度
constexpr uint32_t DIRECT_ADDR = 6;                               // 直接索引项数(addr[0..5])
constexpr uint32_t PTRS_PER_BLK = BLOCK_SIZE / sizeof(uint16_t);  // 每个间址块装的块号数 = 256

// 成组链接法的空闲块链
constexpr uint32_t NICFREE = 50;  // 每组块数 = 超级块空闲块栈容量
constexpr uint32_t NICINOD = 50;  // 超级块空闲 inode 栈容量

// 目录
constexpr uint32_t MAX_FILENAME = 14;                                  // 文件名最大长度
constexpr uint32_t DIR_ENTRY_SIZE = MAX_FILENAME + 2;                  // 目录项大小:名字 + inode 号 = 16
constexpr uint32_t DIR_ENTRIES_PER_BLK = BLOCK_SIZE / DIR_ENTRY_SIZE;  // 每块装的目录项数 = 32
constexpr uint32_t DIRNUM = 128;                                       // 单个目录最多容纳的目录项数

// 打开文件表
constexpr uint32_t MAX_OPEN_FILE = 20;  // 每用户最多同时打开的文件数
constexpr uint32_t SYS_OPEN_FILE = 40;  // 系统打开文件表容量
constexpr uint32_t MAX_USER = 8;        // 最大用户数

// 文件模式位(文件类型存放在 di_mode 的高位)
constexpr uint16_t MODE_FILE = 0x0400;     // 普通文件
constexpr uint16_t MODE_DIR = 0x0800;      // 目录
constexpr uint16_t MODE_SYMLINK = 0x1000;  // 符号链接;数据块里存放目标路径

// 标准 UNIX 权限位(八进制布局)
constexpr uint16_t PERM_OX = 0001;  // other 执行
constexpr uint16_t PERM_OW = 0002;  // other 写
constexpr uint16_t PERM_OR = 0004;  // other 读
constexpr uint16_t PERM_GX = 0010;  // group 执行
constexpr uint16_t PERM_GW = 0020;  // group 写
constexpr uint16_t PERM_GR = 0040;  // group 读
constexpr uint16_t PERM_UX = 0100;  // owner 执行
constexpr uint16_t PERM_UW = 0200;  // owner 写
constexpr uint16_t PERM_UR = 0400;  // owner 读
constexpr uint16_t DEFAULT_MODE = 0777;  // rwxrwxrwx

// 哨兵值
constexpr uint16_t INVALID_BLK = 0xFFFF;  // 非法块号/inode 号,表示分配失败或未找到
constexpr uint16_t END_OF_CHAIN = 0;      // 成组链的链尾标记

}  // namespace pfs
