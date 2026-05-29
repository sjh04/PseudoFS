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
// --- FAT16 on-disk structures (packed, no padding) ---

constexpr uint16_t FAT16_END_OF_CHAIN = 0xFFFF;
constexpr uint16_t FAT16_FREE_CLUSTER = 0x0000;
constexpr uint16_t FAT16_BAD_CLUSTER = 0xFFF7;
constexpr uint8_t FAT16_ATTR_READ_ONLY = 0x01;
constexpr uint8_t FAT16_ATTR_DIRECTORY = 0x10;
constexpr uint8_t FAT16_ATTR_ARCHIVE = 0x20;

// Minimal boot sector stored in block 0
struct FAT16BootSector {
    char volume_label[11];
    uint16_t bytes_per_sector;    // 512
    uint8_t sectors_per_cluster;  // 1
    uint16_t reserved_sectors;    // 1
    uint8_t num_fats;             // 2
    uint16_t root_entry_count;    // 16
    uint16_t total_sectors;       // 546
    uint16_t fat_size_sectors;    // 4
    uint8_t _pad[489];            // pad to 512 bytes
};

static_assert(sizeof(FAT16BootSector) == 512, "BootSector must be 512 bytes");

// Directory entry (32 bytes, DOS-compatible)
struct FAT16DirEntry {
    char name[8];  // filename, space-padded
    char ext[3];   // extension
    uint8_t attr;  // attributes
    uint8_t reserved[10];
    uint16_t time;           // modified time
    uint16_t date;           // modified date
    uint16_t first_cluster;  // starting cluster
    uint32_t file_size;      // file size in bytes
};

static_assert(sizeof(FAT16DirEntry) == 32, "DirEntry must be 32 bytes");
#pragma pack(pop)

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

    std::string fs_type_name() const override;
    DiskUsage fs_disk_usage() const override;
    void fs_block_map(std::vector<uint8_t>& out) override;

    void set_user(uint16_t uid, uint16_t gid) override;
    void set_disk_path(const std::string& path);

   private:
    void sync();
    // --- FAT16 disk layout constants ---
    static constexpr uint32_t kBootBlk = 0;
    static constexpr uint32_t kFat1Start = 1;
    static constexpr uint32_t kFatSize = 4;  // blocks per FAT
    static constexpr uint32_t kFat2Start = 5;
    static constexpr uint32_t kRootDirBlk = 9;
    static constexpr uint32_t kDataStart = 10;
    static constexpr uint32_t kEntriesPerFatBlk = BLOCK_SIZE / 2;              // 256
    static constexpr uint32_t kDirEntriesPerBlk = BLOCK_SIZE / 32;             // 16
    static constexpr uint32_t kClustersPerFat = kFatSize * kEntriesPerFatBlk;  // 1024
    static constexpr uint32_t kDataBlkCount = TOTAL_BLK_NUM - kDataStart;      // 536
    static constexpr uint16_t kRootCluster = 0;                                // Sentinel for root

    BlockDevice& dev_;
    OpenFileTable oft_;
    FAT16BootSector boot_;
    std::vector<uint16_t> fat_;  // in-memory FAT table copy
    std::string cwd_path_;       // e.g. "/home/alice"
    uint16_t cwd_cluster_;       // 0 = root
    uint16_t cur_uid_;
    uint16_t cur_gid_;
    bool mounted_;
    std::string disk_path_;

    // --- Internal helpers ---
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
    uint32_t get_file_size(uint16_t first_cluster) const;
    void update_file_size(uint16_t first_cluster, uint32_t new_size);
};

constexpr uint16_t Fat16Fs::kRootCluster;

}  // namespace pfs
