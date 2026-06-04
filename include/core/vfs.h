#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pfs {

// 打开模式标志(可按位组合)
constexpr int O_READ = 1;    // 读
constexpr int O_WRITE = 2;   // 写
constexpr int O_APPEND = 4;  // 追加写,偏移定位到文件尾
constexpr int O_TRUNC = 8;   // 打开时丢弃原有内容(例如 `open f w`)

// DirEntry/FileStat 的 type 字段取值
constexpr uint8_t TYPE_FILE = 0;     // 普通文件
constexpr uint8_t TYPE_DIR = 1;      // 目录
constexpr uint8_t TYPE_SYMLINK = 2;  // 符号链接

// fs_block_map() 给磁盘视图返回的每块状态分类。
constexpr uint8_t BLK_FREE = 0;  // 可分配,当前空闲
constexpr uint8_t BLK_USED = 1;  // 存放文件/目录/间址数据
constexpr uint8_t BLK_META = 2;  // 保留的元数据(超级块 / inode 区 / FAT / ...)

// 上层通用目录项:ls 等命令返回给 Shell/TUI 的视图,与两种引擎各自的
// 磁盘目录项(UNIX 的 DiskDirEntry、FAT16 的目录项)解耦。
struct DirEntry {
    char name[14];      // 文件名
    uint16_t inode_no;  // inode 编号(FAT16 引擎可借用此字段表达定位信息)
    uint8_t type;       // 类型:文件/目录/符号链接
    uint32_t size;      // 文件大小(字节)
};

// 上层通用文件状态:stat 命令返回的元信息,与磁盘 inode 结构解耦。
struct FileStat {
    uint16_t mode;   // 文件类型 + rwx 权限
    uint16_t uid;    // 所有者用户号
    uint16_t gid;    // 所有者组号
    uint32_t size;   // 文件大小(字节)
    uint32_t atime;  // 最后访问时间
    uint32_t mtime;  // 最后修改时间
    uint32_t ctime;  // 创建时间
    uint16_t nlink;  // 硬链接计数
    uint8_t type;    // 类型:文件/目录/符号链接
};

// 磁盘使用情况:TUI 磁盘状态面板用来画 inode/块占用进度条。
struct DiskUsage {
    uint32_t total_blocks;   // 数据块总数
    uint32_t used_blocks;    // 已用数据块数
    uint32_t total_inodes;   // inode 总数
    uint32_t used_inodes;    // 已用 inode 数
};

// VFS 抽象文件系统接口:全套文件操作的统一抽象基类(纯虚)。
// UNIX inode 引擎(UnixFs)和 FAT16 引擎都实现这同一套接口;上层的
// Shell/TUI 只持有一个 IFileSystem* 指针,完全不知道底层是 inode 还是 FAT。
// F2 切换引擎,做的就是把这个指针从一个实现换成另一个——这正是依赖倒置:
// 上层依赖抽象接口,而不依赖具体引擎。
class IFileSystem {
   public:
    virtual ~IFileSystem() = default;

    virtual int fs_format() = 0;
    virtual int fs_mount() = 0;
    virtual int fs_unmount() = 0;

    // 设置当前用户上下文用于权限检查(由 login/su 驱动)。
    virtual void set_user(uint16_t uid, uint16_t gid) = 0;

    virtual int fs_create(const char* path, uint16_t mode) = 0;
    virtual int fs_open(const char* path, int flags) = 0;
    virtual int fs_close(int fd) = 0;
    virtual ssize_t fs_read(int fd, void* buf, size_t len) = 0;
    virtual ssize_t fs_write(int fd, const void* buf, size_t len) = 0;
    virtual int fs_delete(const char* path) = 0;
    virtual int fs_delete_recursive(const char* path) = 0;

    virtual int fs_mkdir(const char* path) = 0;
    virtual int fs_rmdir(const char* path) = 0;
    virtual int fs_chdir(const char* path) = 0;
    virtual int fs_ls(const char* path, std::vector<DirEntry>& out) = 0;
    virtual std::string fs_pwd() = 0;

    virtual int fs_stat(const char* path, FileStat& out) = 0;
    virtual int fs_chmod(const char* path, uint16_t mode) = 0;
    virtual int fs_link(const char* src, const char* dst) = 0;

    // 符号链接。fs_symlink 创建 `linkpath`,指向 `target`
    //(任意字符串,惰性解析)。fs_readlink 返回符号链接存放的目标。
    // 不支持符号链接的引擎返回 -1。
    virtual int fs_symlink(const char* target, const char* linkpath) = 0;
    virtual int fs_readlink(const char* path, std::string& out) = 0;

    virtual std::string fs_type_name() const = 0;
    virtual DiskUsage fs_disk_usage() const = 0;

    // 给 `out` 填入每个磁盘块一字节的状态(BLK_FREE/BLK_USED/BLK_META);
    // out.size() 即为总块数。驱动 TUI 的磁盘视图。
    virtual void fs_block_map(std::vector<uint8_t>& out) = 0;
};

}  // namespace pfs
