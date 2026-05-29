#pragma once

#include <cstdint>

namespace pfs {

constexpr uint32_t BLOCK_SIZE = 512;

// Disk layout (block numbers)
constexpr uint32_t BOOT_BLK = 0;
constexpr uint32_t SUPER_BLK = 1;
constexpr uint32_t INODE_START_BLK = 2;
constexpr uint32_t INODE_BLK_NUM = 32;
constexpr uint32_t DATA_START_BLK = INODE_START_BLK + INODE_BLK_NUM;  // 34
constexpr uint32_t DATA_BLK_NUM = 512;
constexpr uint32_t TOTAL_BLK_NUM = DATA_START_BLK + DATA_BLK_NUM;  // 546

// INode
constexpr uint32_t INODE_SIZE = 64;
constexpr uint32_t INODES_PER_BLK = BLOCK_SIZE / INODE_SIZE;      // 8
constexpr uint32_t INODE_TOTAL = INODE_BLK_NUM * INODES_PER_BLK;  // 256
constexpr uint32_t NADDR = 10;
constexpr uint32_t DIRECT_ADDR = 6;
constexpr uint32_t PTRS_PER_BLK = BLOCK_SIZE / sizeof(uint16_t);  // 256

// Group-linked free list
constexpr uint32_t NICFREE = 50;
constexpr uint32_t NICINOD = 50;

// Directory
constexpr uint32_t MAX_FILENAME = 14;
constexpr uint32_t DIR_ENTRY_SIZE = MAX_FILENAME + 2;                  // 16
constexpr uint32_t DIR_ENTRIES_PER_BLK = BLOCK_SIZE / DIR_ENTRY_SIZE;  // 32
constexpr uint32_t DIRNUM = 128;

// Open file table
constexpr uint32_t MAX_OPEN_FILE = 20;
constexpr uint32_t SYS_OPEN_FILE = 40;
constexpr uint32_t MAX_USER = 8;

// File mode bits (type stored in the high bits of di_mode)
constexpr uint16_t MODE_FILE = 0x0400;
constexpr uint16_t MODE_DIR = 0x0800;
constexpr uint16_t MODE_SYMLINK = 0x1000;  // symbolic link; data block holds target path

// Standard UNIX permission bits (octal layout)
constexpr uint16_t PERM_OX = 0001;
constexpr uint16_t PERM_OW = 0002;
constexpr uint16_t PERM_OR = 0004;
constexpr uint16_t PERM_GX = 0010;
constexpr uint16_t PERM_GW = 0020;
constexpr uint16_t PERM_GR = 0040;
constexpr uint16_t PERM_UX = 0100;
constexpr uint16_t PERM_UW = 0200;
constexpr uint16_t PERM_UR = 0400;
constexpr uint16_t DEFAULT_MODE = 0777;  // rwxrwxrwx

// Sentinel values
constexpr uint16_t INVALID_BLK = 0xFFFF;
constexpr uint16_t END_OF_CHAIN = 0;

}  // namespace pfs
