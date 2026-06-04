#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/open_file_table.h"
#include "core/vfs.h"

namespace pfs {

#pragma pack(push, 1)
// --- FAT16 磁盘结构(紧凑排布,无对齐填充) ---

// FAT 表项的特殊值
constexpr uint16_t FAT16_END_OF_CHAIN = 0xFFFF;  // 簇链结束标记
constexpr uint16_t FAT16_FREE_CLUSTER = 0x0000;  // 空闲簇
constexpr uint16_t FAT16_BAD_CLUSTER = 0xFFF7;   // 坏簇(本模拟器不产生,仅保留语义)
// 目录项 attr 字段的属性位
constexpr uint8_t FAT16_ATTR_READ_ONLY = 0x01;  // 只读
constexpr uint8_t FAT16_ATTR_DIRECTORY = 0x10;  // 目录
constexpr uint8_t FAT16_ATTR_ARCHIVE = 0x20;    // 普通文件(归档位)

// 精简版引导扇区,存在 0 号块,记录卷的几何参数(块大小、FAT 大小等)
struct FAT16BootSector {
    char volume_label[11];        // 卷标
    uint16_t bytes_per_sector;    // 512
    uint8_t sectors_per_cluster;  // 1
    uint16_t reserved_sectors;    // 1
    uint8_t num_fats;             // 2
    uint16_t root_entry_count;    // 16
    uint16_t total_sectors;       // 546
    uint16_t fat_size_sectors;    // 4
    uint8_t _pad[489];            // 补齐到 512 字节
};

static_assert(sizeof(FAT16BootSector) == 512, "BootSector must be 512 bytes");

// 目录项(32 字节,兼容 DOS):8.3 文件名 + 起始簇号 + 大小
struct FAT16DirEntry {
    char name[8];  // 文件名,不足 8 字符用空格补齐
    char ext[3];   // 扩展名
    uint8_t attr;  // 属性位(目录/文件/只读…)
    uint8_t reserved[10];  // 保留(DOS 兼容占位)
    uint16_t time;           // 修改时间
    uint16_t date;           // 修改日期
    uint16_t first_cluster;  // 起始簇号(文件 = 一条簇链)
    uint32_t file_size;      // 文件大小(字节)
};

static_assert(sizeof(FAT16DirEntry) == 32, "DirEntry must be 32 bytes");
#pragma pack(pop)

// FAT16 引擎:引导扇区 + FAT 表×2 + 根目录区 + 数据区,文件用簇链组织。
// 实现 IFileSystem,和 UNIX 引擎并列挂在 VFS 下,可在 TUI 中切换。
class Fat16Fs : public IFileSystem {
   public:
    explicit Fat16Fs(BlockDevice& dev);

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

    void set_user(uint16_t uid, uint16_t gid) override;
    void set_disk_path(const std::string& path);

   private:
    void sync();
    // --- FAT16 磁盘布局常量 ---
    // 块号: [0]引导 / [1..4]FAT1 / [5..8]FAT2 / [9]根目录区 / [10..]数据区
    static constexpr uint32_t kBootBlk = 0;     // 引导扇区
    static constexpr uint32_t kFat1Start = 1;   // FAT1 起始块
    static constexpr uint32_t kFatSize = 4;     // 每张 FAT 占的块数
    static constexpr uint32_t kFat2Start = 5;   // FAT2(备份)起始块
    static constexpr uint32_t kRootDirBlk = 9;  // 根目录区
    static constexpr uint32_t kDataStart = 10;  // 数据区起始块
    static constexpr uint32_t kEntriesPerFatBlk = BLOCK_SIZE / 2;              // 256
    static constexpr uint32_t kDirEntriesPerBlk = BLOCK_SIZE / 32;             // 16
    static constexpr uint32_t kClustersPerFat = kFatSize * kEntriesPerFatBlk;  // 1024
    static constexpr uint32_t kDataBlkCount = TOTAL_BLK_NUM - kDataStart;      // 536
    static constexpr uint16_t kRootCluster = 0;                                // 根目录的哨兵簇号

    BlockDevice& dev_;   // 底层块设备
    OpenFileTable oft_;  // 打开文件表(与 UNIX 引擎共用同一套实现)
    // 每个打开 fd 所属的父目录簇号,在 open 时记下来,这样查/改文件大小
    // 时能直接定位到对应的目录项,而不必扫遍所有簇(否则文件数据可能被
    // 误当成目录项)。按用户 fd 下标索引,只在 fd 打开期间有效。
    uint16_t open_parent_[MAX_OPEN_FILE] = {};
    FAT16BootSector boot_;       // 引导扇区的内存副本
    std::vector<uint16_t> fat_;  // 内存中的 FAT 表副本
    std::string cwd_path_;       // 当前工作目录路径,形如 "/home/alice"
    uint16_t cwd_cluster_;       // 当前工作目录的簇号,0 = 根目录
    uint16_t cur_uid_;           // 当前用户号(FAT16 无权限位,仅保持接口一致)
    uint16_t cur_gid_;           // 当前组号
    // 当前会话使用的打开文件表槽位。FAT16 没有按用户的权限,且本模拟器
    // 同时只有一个活动用户,所以(和 UNIX 引擎一样)所有会话共用 0 号槽。
    // 注意这里不能用 cur_uid_:OpenFileTable 会拒绝 user_id >= MAX_USER,
    // 那样 uid >= 8 的用户就打不开文件了。见测试 HighUidCanOpenAndReadWrite。
    uint16_t user_slot_ = 0;
    bool mounted_;           // 是否已挂载
    std::string disk_path_;  // 磁盘镜像路径,非空则每次写操作后自动落盘

    // --- 内部辅助函数 ---
    uint32_t cluster_to_block(uint16_t cluster) const;
    int read_fat_entry(uint16_t cluster, uint16_t& out) const;
    int write_fat_entry(uint16_t cluster, uint16_t value);
    uint16_t alloc_cluster();
    int free_cluster_chain(uint16_t start_cluster);
    int read_cluster_chain(uint16_t start_cluster, std::vector<uint8_t>& data,
                           size_t max_bytes = ~0u) const;
    int write_cluster_chain(uint16_t start_cluster, const void* data, size_t len,
                            uint16_t& out_first_cluster);
    int resolve_path(const char* path, uint16_t& out_cluster, uint16_t& out_parent,
                     std::string& out_name) const;
    int read_dir_entries(uint16_t cluster, std::vector<FAT16DirEntry>& out) const;
    int write_dir_entry(uint16_t dir_cluster, const FAT16DirEntry& entry);
    int remove_dir_entry(uint16_t dir_cluster, const char* name);
    int append_dir_entry(uint16_t dir_cluster, const FAT16DirEntry& entry);
    uint16_t find_free_cluster() const;
    void init_fat_from_disk();
    void flush_fat();
    void set_dir_entry_name(FAT16DirEntry& e, const char* name);
    void fat16_to_vfs_entry(const FAT16DirEntry& fe, DirEntry& ve) const;
    void timestamp(uint16_t& t, uint16_t& d) const;
    uint32_t count_clusters(uint16_t start) const;
    // 从目录项读取/更新文件大小,只在文件自己的父目录(dir_cluster)里查找。
    // dir_cluster 等于 kRootCluster 表示根目录。
    uint32_t get_file_size(uint16_t dir_cluster, uint16_t first_cluster) const;
    void update_file_size(uint16_t dir_cluster, uint16_t first_cluster, uint32_t new_size);
};

constexpr uint16_t Fat16Fs::kRootCluster;

}  // namespace pfs
