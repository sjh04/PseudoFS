#include "fs/unix/directory.h"

#include <cstring>

#include "fs/unix/super_block.h"

namespace pfs {

// 目录管理器:目录项的增删查 + 路径解析。
// 目录在 UNIX 里就是一种内容为目录项数组的普通文件,
// 所以读写目录内容同样要经过 inode 层的 bmap 寻址。
DirectoryManager::DirectoryManager(BlockDevice& dev, InodeManager& imng) : dev_(dev), imng_(imng) {
}

// ---- 路径辅助 ----

// 把路径按 '/' 切段,空段(连续斜杠、首尾斜杠)自动跳过。
// "/a/b/c" 和 "a/b/c" 切出来相同;绝对/相对的区别由调用方看 path[0] 判断。
std::vector<std::string> DirectoryManager::split_path(const char* path) {
    std::vector<std::string> parts;
    std::string s(path);
    size_t start = 0;
    while (start < s.size()) {
        if (s[start] == '/') {
            start++;
            continue;
        }
        size_t end = s.find('/', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// ---- 目录数据读写 ----

// 把目录文件的全部目录项读进内存:按逻辑块顺序 bmap 出物理块,
// 逐块读出,按 16 字节一条切成目录项数组
int DirectoryManager::read_dir_data(MemINode* dir_ip, std::vector<DiskDirEntry>& entries) {
    uint32_t size = dir_ip->di.di_size;
    uint32_t num_entries = size / DIR_ENTRY_SIZE;
    entries.resize(num_entries);

    uint8_t buf[BLOCK_SIZE];
    uint32_t entry_idx = 0;

    for (uint32_t blk_off = 0; blk_off * BLOCK_SIZE < size; blk_off++) {
        uint16_t phys = imng_.bmap(dir_ip, blk_off);
        if (phys == 0 || phys == INVALID_BLK) {
            return -1;
        }
        dev_.read_block(DATA_START_BLK + phys, buf);

        uint32_t entries_in_block = DIR_ENTRIES_PER_BLK;
        if (entry_idx + entries_in_block > num_entries) {
            entries_in_block = num_entries - entry_idx;
        }
        std::memcpy(&entries[entry_idx], buf, entries_in_block * DIR_ENTRY_SIZE);
        entry_idx += entries_in_block;
    }
    return 0;
}

// 把整张目录项表写回目录文件,不够的块用 bmap_alloc 现场分配,
// 最后更新目录文件大小并标脏
int DirectoryManager::write_dir_data(MemINode* dir_ip, const std::vector<DiskDirEntry>& entries) {
    uint32_t total_size = entries.size() * DIR_ENTRY_SIZE;
    uint32_t blocks_needed = (total_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    uint8_t buf[BLOCK_SIZE];
    uint32_t entry_idx = 0;

    for (uint32_t blk_off = 0; blk_off < blocks_needed; blk_off++) {
        uint16_t phys = imng_.bmap_alloc(dir_ip, blk_off);
        if (phys == INVALID_BLK) {
            return -1;
        }

        std::memset(buf, 0, BLOCK_SIZE);
        uint32_t entries_in_block = DIR_ENTRIES_PER_BLK;
        if (entry_idx + entries_in_block > entries.size()) {
            entries_in_block = entries.size() - entry_idx;
        }
        std::memcpy(buf, &entries[entry_idx], entries_in_block * DIR_ENTRY_SIZE);
        dev_.write_block(DATA_START_BLK + phys, buf);
        entry_idx += entries_in_block;
    }

    dir_ip->di.di_size = total_size;
    dir_ip->i_dirty = true;
    return 0;
}

// ---- 对外接口 ----

// 在一个目录里按名字查 inode 号。d_ino == 0 的槽是已删除的空项,要跳过
uint16_t DirectoryManager::lookup(MemINode* dir_ip, const char* name) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return INVALID_BLK;
    }

    for (auto& e : entries) {
        if (e.d_ino != 0 && std::strncmp(e.d_name, name, MAX_FILENAME) == 0) {
            return e.d_ino;
        }
    }
    return INVALID_BLK;
}

// 往目录里挂一条 name → ino 的目录项,创建文件和硬链接(ln)走的是同一个函数
// ——"链接"的本质就是多一条目录项指向同一个 inode
int DirectoryManager::link(MemINode* dir_ip, const char* name, uint16_t ino) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return -1;
    }

    // 查重名
    for (auto& e : entries) {
        if (e.d_ino != 0 && std::strncmp(e.d_name, name, MAX_FILENAME) == 0) {
            return -1;
        }
    }

    // 优先复用空槽(d_ino == 0,删除留下的洞)
    for (auto& e : entries) {
        if (e.d_ino == 0) {
            std::memset(e.d_name, 0, MAX_FILENAME);
            std::strncpy(e.d_name, name, MAX_FILENAME);
            e.d_ino = ino;
            return write_dir_data(dir_ip, entries);
        }
    }

    // 没有空槽,在表尾追加一条
    DiskDirEntry ne{};
    std::strncpy(ne.d_name, name, MAX_FILENAME);
    ne.d_ino = ino;
    entries.push_back(ne);
    return write_dir_data(dir_ip, entries);
}

// 从目录里摘掉一条目录项(清成空槽)。注意这里只动目录:
// inode 的 nlink 减一、以及是否真正回收,由上层 UnixFS 决定
int DirectoryManager::unlink(MemINode* dir_ip, const char* name) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return -1;
    }

    for (auto& e : entries) {
        if (e.d_ino != 0 && std::strncmp(e.d_name, name, MAX_FILENAME) == 0) {
            e.d_ino = 0;
            std::memset(e.d_name, 0, MAX_FILENAME);
            return write_dir_data(dir_ip, entries);
        }
    }
    return -1;
}

// 给 ls/tree 用的只读接口,原样返回全部目录项
int DirectoryManager::read_entries(MemINode* dir_ip, std::vector<DiskDirEntry>& out) {
    return read_dir_data(dir_ip, out);
}

// 新目录"开张":写入 . 和 .. 两条初始目录项
// (. 指自己,.. 指父目录,根目录的 .. 指向它自己)
int DirectoryManager::init_dir(MemINode* dir_ip, uint16_t self_ino, uint16_t parent_ino) {
    std::vector<DiskDirEntry> entries(2);

    std::memset(&entries[0], 0, DIR_ENTRY_SIZE);
    std::strncpy(entries[0].d_name, ".", MAX_FILENAME);
    entries[0].d_ino = self_ino;

    std::memset(&entries[1], 0, DIR_ENTRY_SIZE);
    std::strncpy(entries[1].d_name, "..", MAX_FILENAME);
    entries[1].d_ino = parent_ino;

    return write_dir_data(dir_ip, entries);
}

// 判断目录是否为空(除 . 和 .. 外没有有效项),rmdir 前的必查项
bool DirectoryManager::is_empty(MemINode* dir_ip) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return false;
    }
    for (auto& e : entries) {
        if (e.d_ino != 0 && std::strncmp(e.d_name, ".", MAX_FILENAME) != 0 &&
            std::strncmp(e.d_name, "..", MAX_FILENAME) != 0) {
            return false;
        }
    }
    return true;
}

// namei:路径字符串 → inode 号,整个文件系统"按名字找文件"的总入口
uint16_t DirectoryManager::namei(const char* path, uint16_t cwd_ino, uint16_t root_ino) {
    auto parts = split_path(path);
    if (parts.empty()) {
        // 路径只有斜杠("/" 或 "//"):绝对路径就是根,相对路径就是当前目录
        return (path[0] == '/') ? root_ino : cwd_ino;
    }

    // 绝对路径从根出发,相对路径从当前目录出发
    uint16_t cur_ino = (path[0] == '/') ? root_ino : cwd_ino;

    // 逐段解析:get 当前 inode → 确认是目录 → lookup 下一段 → put 归还引用
    // 再前进。get/put 严格配对,引用计数才不会泄漏。
    for (auto& name : parts) {
        MemINode* ip = imng_.get(cur_ino);
        if (ip == nullptr || !(ip->di.di_mode & MODE_DIR)) {
            if (ip) imng_.put(ip);
            return INVALID_BLK;
        }
        uint16_t next = lookup(ip, name.c_str());
        imng_.put(ip);
        if (next == INVALID_BLK) {
            return INVALID_BLK;
        }
        cur_ino = next;
    }
    return cur_ino;
}

// 和 namei 走法相同,但停在倒数第二级,最后一段名字经 basename 带回。
// create/delete/link 改的是父目录的目录项,所以要的是父目录而不是目标本身
// (目标此时可能还不存在)。
uint16_t DirectoryManager::namei_parent(const char* path, uint16_t cwd_ino, uint16_t root_ino,
                                        std::string& basename) {
    auto parts = split_path(path);
    if (parts.empty()) {
        return INVALID_BLK;
    }

    basename = parts.back();
    parts.pop_back();

    if (parts.empty()) {
        // 路径只剩文件名一段:父目录就是当前目录(绝对路径则是根)
        return (path[0] == '/') ? root_ino : cwd_ino;
    }

    // 逐段解析到父目录为止,走法和 namei 完全一致
    uint16_t cur_ino = (path[0] == '/') ? root_ino : cwd_ino;
    for (auto& name : parts) {
        MemINode* ip = imng_.get(cur_ino);
        if (ip == nullptr || !(ip->di.di_mode & MODE_DIR)) {
            if (ip) imng_.put(ip);
            return INVALID_BLK;
        }
        uint16_t next = lookup(ip, name.c_str());
        imng_.put(ip);
        if (next == INVALID_BLK) {
            return INVALID_BLK;
        }
        cur_ino = next;
    }
    return cur_ino;
}

}  // namespace pfs
