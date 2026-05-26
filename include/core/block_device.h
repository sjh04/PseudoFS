#pragma once

#include <cstdint>

namespace pfs {

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
    uint8_t* disk_;
    uint32_t total_blocks_;
    uint32_t block_size_;
};

}  // namespace pfs
