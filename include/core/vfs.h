#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pfs {

constexpr int O_READ = 1;
constexpr int O_WRITE = 2;
constexpr int O_APPEND = 4;

constexpr uint8_t TYPE_FILE = 0;
constexpr uint8_t TYPE_DIR = 1;

// Per-block status categories returned by fs_block_map() for the disk view.
constexpr uint8_t BLK_FREE = 0;  // allocatable, currently unused
constexpr uint8_t BLK_USED = 1;  // holds file/dir/indirect data
constexpr uint8_t BLK_META = 2;  // reserved metadata (superblock / inode area / FAT / ...)

struct DirEntry {
    char name[14];
    uint16_t inode_no;
    uint8_t type;
    uint32_t size;
};

struct FileStat {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint16_t nlink;
    uint8_t type;
};

struct DiskUsage {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t total_inodes;
    uint32_t used_inodes;
};

class IFileSystem {
   public:
    virtual ~IFileSystem() = default;

    virtual int fs_format() = 0;
    virtual int fs_mount() = 0;
    virtual int fs_unmount() = 0;

    // Set the current user context for permission checks (driven by login/su).
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

    virtual std::string fs_type_name() const = 0;
    virtual DiskUsage fs_disk_usage() const = 0;

    // Fill `out` with one status byte (BLK_FREE/BLK_USED/BLK_META) per disk
    // block; out.size() becomes the total block count. Drives the TUI disk view.
    virtual void fs_block_map(std::vector<uint8_t>& out) = 0;
};

}  // namespace pfs
