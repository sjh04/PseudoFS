#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "fs/unix/inode.h"

namespace pfs {

// 磁盘目录项,16 字节。
// 一个目录文件就是这种目录项的数组。
// d_ino == 0 表示该槽为空(已删除或未使用)。
struct DiskDirEntry {
    char d_name[MAX_FILENAME];  // 文件名,14 字节,不足补 '\0'
    uint16_t d_ino;             // inode 编号,0 = 空
};

static_assert(sizeof(DiskDirEntry) == DIR_ENTRY_SIZE, "DiskDirEntry must be 16 bytes");

// 目录管理器:把目录当作目录项数组来读写,负责路径解析(namei)和目录项的增删查。
class DirectoryManager {
   public:
    DirectoryManager(BlockDevice& dev, InodeManager& imng);

    // 把一条路径(绝对路径或相对当前目录)解析成 inode 号。
    // 未找到返回 INVALID_BLK。
    // cwd_ino:当前工作目录的 inode 号。
    // root_ino:根目录的 inode 号。
    uint16_t namei(const char* path, uint16_t cwd_ino, uint16_t root_ino);

    // 与 namei 类似,但返回的是父目录的 inode 号,
    // 并把路径最后一段写入 'basename'。
    // 供 create/mkdir/delete 使用——它们需要父目录来插入/删除目录项。
    uint16_t namei_parent(const char* path, uint16_t cwd_ino, uint16_t root_ino,
                          std::string& basename);

    // 在一个目录 inode 里查名字。返回 inode 号,
    // 未找到返回 INVALID_BLK。
    uint16_t lookup(MemINode* dir_ip, const char* name);

    // 往目录里加一条新目录项(name → ino)。
    // 成功返回 0,失败返回 -1(目录满或同名已存在)。
    int link(MemINode* dir_ip, const char* name, uint16_t ino);

    // 按名字从目录里删除一条目录项。
    // 成功返回 0,未找到返回 -1。
    int unlink(MemINode* dir_ip, const char* name);

    // 把目录里的全部目录项读进 vector。
    int read_entries(MemINode* dir_ip, std::vector<DiskDirEntry>& out);

    // 初始化一个新目录:写入 "." 和 ".." 两条目录项。
    // self_ino:本目录的 inode,parent_ino:父目录的 inode。
    int init_dir(MemINode* dir_ip, uint16_t self_ino, uint16_t parent_ino);

    // 判断目录是否为空(只剩 "." 和 "..")。
    bool is_empty(MemINode* dir_ip);

   private:
    BlockDevice& dev_;    // 底层块设备,读写目录数据块用
    InodeManager& imng_;  // inode 管理器,经 bmap 定位目录的数据块

    // 把路径切成各段。"/home/alice" → ["home", "alice"]
    std::vector<std::string> split_path(const char* path);

    // 从一个 inode 读写原始目录数据(DiskDirEntry 数组)。
    int read_dir_data(MemINode* dir_ip, std::vector<DiskDirEntry>& entries);
    int write_dir_data(MemINode* dir_ip, const std::vector<DiskDirEntry>& entries);
};

}  // namespace pfs
