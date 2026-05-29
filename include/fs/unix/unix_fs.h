#pragma once

#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/open_file_table.h"
#include "core/vfs.h"
#include "fs/unix/directory.h"
#include "fs/unix/inode.h"
#include "fs/unix/super_block.h"

namespace pfs {

class UnixFs : public IFileSystem {
   public:
    explicit UnixFs(BlockDevice& dev);

    int fs_format() override;
    int fs_mount() override;
    int fs_unmount() override;

    int fs_create(const char* path, uint16_t mode) override;
    int fs_open(const char* path, int flags) override;
    int fs_close(int fd) override;
    ssize_t fs_read(int fd, void* buf, size_t len) override;
    ssize_t fs_write(int fd, const void* buf, size_t len) override;
    int fs_delete(const char* path) override;
    int fs_delete_recursive(const char* path) override;

    int fs_mkdir(const char* path) override;
    int fs_rmdir(const char* path) override;
    int fs_chdir(const char* path) override;
    int fs_ls(const char* path, std::vector<DirEntry>& out) override;
    std::string fs_pwd() override;

    int fs_stat(const char* path, FileStat& out) override;
    int fs_chmod(const char* path, uint16_t mode) override;
    int fs_link(const char* src, const char* dst) override;

    std::string fs_type_name() const override;
    DiskUsage fs_disk_usage() const override;

    // uid/gid: actual UNIX ids used for permission checks
    void set_user(uint16_t uid, uint16_t gid) override;

    // Set the disk image path for auto-sync. When set, every write
    // operation (create/write/delete/mkdir/rmdir/chmod/link) flushes
    // the superblock and saves the disk image to this file.
    // Prevents data loss on crash.
    void set_disk_path(const std::string& path);

   private:
    void sync();
    BlockDevice& dev_;
    SuperBlock sb_;
    InodeManager imng_;
    DirectoryManager dmng_;
    OpenFileTable oft_;

    uint16_t root_ino_;
    uint16_t cwd_ino_;
    std::string cwd_path_;
    uint16_t cur_uid_;
    uint16_t cur_gid_;
    bool mounted_;
    std::string disk_path_;
    uint16_t user_slot_;

    bool check_access(MemINode* ip, uint8_t required);
};

}  // namespace pfs
