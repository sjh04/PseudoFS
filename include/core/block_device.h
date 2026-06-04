#pragma once

#include <cstdint>

namespace pfs {

// 块设备层:整块虚拟磁盘就是一段内存缓冲区,对外只提供"按块读、按块写"。
// 上层(超级块、inode、目录)全部通过它访问磁盘,持久化时再把整块缓冲区
// 存成宿主机文件 pfs.img,下次启动读回来即可恢复。
class BlockDevice {
   public:
    BlockDevice(uint32_t total_blocks, uint32_t block_size = 512);
    ~BlockDevice();

    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;

    int read_block(uint32_t block_no, void* buf) const;
    int write_block(uint32_t block_no, const void* buf);

    int save_to_file(const char* path) const;
    int load_from_file(const char* path);

    uint32_t total_blocks() const;
    uint32_t block_size() const;
    uint32_t disk_size() const;

    void zero_all();

   private:
    uint8_t* disk_;          // 整块虚拟磁盘的内存缓冲区
    uint32_t total_blocks_;  // 总块数
    uint32_t block_size_;    // 块大小(字节)
};

}  // namespace pfs
