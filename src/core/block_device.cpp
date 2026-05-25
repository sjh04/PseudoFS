#include "core/block_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pfs {

BlockDevice::BlockDevice(uint32_t total_blocks, uint32_t block_size)
    : total_blocks_(total_blocks), block_size_(block_size) {
    disk_ = static_cast<uint8_t*>(std::calloc(total_blocks, block_size));
}

BlockDevice::~BlockDevice() {
    std::free(disk_);
}

int BlockDevice::read_block(uint32_t block_no, void* buf) const {
    if (block_no >= total_blocks_ || buf == nullptr) {
        return -1;
    }
    std::memcpy(buf, disk_ + static_cast<size_t>(block_no) * block_size_, block_size_);
    return 0;
}

int BlockDevice::write_block(uint32_t block_no, const void* buf) {
    if (block_no >= total_blocks_ || buf == nullptr) {
        return -1;
    }
    std::memcpy(disk_ + static_cast<size_t>(block_no) * block_size_, buf, block_size_);
    return 0;
}

int BlockDevice::save_to_file(const char* path) const {
    FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        return -1;
    }
    size_t total = static_cast<size_t>(total_blocks_) * block_size_;
    size_t written = std::fwrite(disk_, 1, total, fp);
    std::fclose(fp);
    return (written == total) ? 0 : -1;
}

int BlockDevice::load_from_file(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return -1;
    }
    size_t total = static_cast<size_t>(total_blocks_) * block_size_;
    size_t read_bytes = std::fread(disk_, 1, total, fp);
    std::fclose(fp);
    return (read_bytes == total) ? 0 : -1;
}

uint32_t BlockDevice::total_blocks() const {
    return total_blocks_;
}

uint32_t BlockDevice::block_size() const {
    return block_size_;
}

uint32_t BlockDevice::disk_size() const {
    return total_blocks_ * block_size_;
}

void BlockDevice::zero_all() {
    std::memset(disk_, 0, static_cast<size_t>(total_blocks_) * block_size_);
}

}  // namespace pfs
