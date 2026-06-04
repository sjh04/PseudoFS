#include "core/block_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pfs {

// 整块"磁盘"就是一段 calloc 出来的连续内存,读/写一个块 = 按
// block_no * block_size 偏移做一次 memcpy,上层感知不到这是模拟的。
// calloc 顺带保证新盘全零,相当于一块出厂"裸盘"。
BlockDevice::BlockDevice(uint32_t total_blocks, uint32_t block_size)
    : total_blocks_(total_blocks), block_size_(block_size) {
    disk_ = static_cast<uint8_t*>(std::calloc(total_blocks, block_size));
}

// 释放模拟磁盘占用的内存
BlockDevice::~BlockDevice() {
    std::free(disk_);
}

// 读一个块:从磁盘数组第 block_no 块起拷 block_size 字节到调用者缓冲区。
// 块号越界/空指针返回 -1,这是块设备层唯一的合法性检查。
int BlockDevice::read_block(uint32_t block_no, void* buf) const {
    if (block_no >= total_blocks_ || buf == nullptr) {
        return -1;
    }
    std::memcpy(buf, disk_ + static_cast<size_t>(block_no) * block_size_, block_size_);
    return 0;
}

// 写一个块:方向相反,把调用者缓冲区整块拷进磁盘数组
int BlockDevice::write_block(uint32_t block_no, const void* buf) {
    if (block_no >= total_blocks_ || buf == nullptr) {
        return -1;
    }
    std::memcpy(disk_ + static_cast<size_t>(block_no) * block_size_, buf, block_size_);
    return 0;
}

// 持久化:把整个内存数组 dump 成宿主机文件(pfs.img)。
// 内存数组随进程退出就没了,下次启动靠这个文件恢复。
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

// 启动时从宿主机文件整盘读回,与 save_to_file 配对,实现"重启数据还在"
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

// 总块数(UNIX 引擎:1 引导 + 1 超级块 + 32 inode 区 + 512 数据区 = 546)
uint32_t BlockDevice::total_blocks() const {
    return total_blocks_;
}

// 块大小(512 字节)
uint32_t BlockDevice::block_size() const {
    return block_size_;
}

// 磁盘总容量 = 总块数 × 块大小
uint32_t BlockDevice::disk_size() const {
    return total_blocks_ * block_size_;
}

// 整盘清零,format 前调用,回到"裸盘"状态
void BlockDevice::zero_all() {
    std::memset(disk_, 0, static_cast<size_t>(total_blocks_) * block_size_);
}

}  // namespace pfs
