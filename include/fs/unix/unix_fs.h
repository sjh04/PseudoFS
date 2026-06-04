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

// UNIX 引擎:把 IFileSystem 接口落到 inode 体系上。
// 自己不直接碰盘,而是组合超级块、inode、目录三个下层模块,
// 再加一张打开文件表,凑齐 open/read/write/create/delete/... 全套命令。
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
    int fs_symlink(const char* target, const char* linkpath) override;
    int fs_readlink(const char* path, std::string& out) override;

    std::string fs_type_name() const override;
    DiskUsage fs_disk_usage() const override;
    void fs_block_map(std::vector<uint8_t>& out) override;

    // uid/gid:权限检查实际用到的 UNIX 用户号/组号
    void set_user(uint16_t uid, uint16_t gid) override;

    // 设置磁盘镜像路径以开启自动落盘。一旦设置,每次写操作
    //(create/write/delete/mkdir/rmdir/chmod/link)都会 flush 超级块
    // 并把磁盘镜像存到这个文件,防止崩溃丢数据。
    void set_disk_path(const std::string& path);

   private:
    void sync();
    BlockDevice& dev_;       // 底层块设备
    SuperBlock sb_;          // 超级块:空闲块(成组链)+ 空闲 inode 栈
    InodeManager imng_;      // inode 管理:iget/iput/混合索引 bmap
    DirectoryManager dmng_;  // 目录管理:namei/目录项增删查
    OpenFileTable oft_;      // 打开文件表:fd → inode + 偏移

    uint16_t root_ino_;      // 根目录 inode 号,绝对路径解析的起点
    uint16_t cwd_ino_;       // 当前工作目录的 inode 号,相对路径解析的起点
    std::string cwd_path_;   // 当前工作目录的路径字符串,pwd 直接返回它
    uint16_t cur_uid_;       // 当前用户号,权限检查用
    uint16_t cur_gid_;       // 当前组号,权限检查用
    bool mounted_;           // 是否已挂载
    std::string disk_path_;  // 磁盘镜像路径,非空则每次写操作后自动落盘
    uint16_t user_slot_;     // 打开文件表槽位(单活动用户,固定用 0 号槽)

    bool check_access(MemINode* ip, uint8_t required);

    // 把路径解析成 inode,并跟随末端的软链接(限制跳数,这样成环/断链
    // 会失败而不是死循环)。给那些应当作用于链接目标的操作用(open、chdir)。
    uint16_t namei_follow(const char* path);

    // 从软链接 inode 的第一个数据块里读出它存的目标路径字符串。
    std::string read_link_target(MemINode* ip);
};

}  // namespace pfs
