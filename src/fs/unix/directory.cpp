#include "fs/unix/directory.h"

#include <cstring>

#include "fs/unix/super_block.h"

namespace pfs {

DirectoryManager::DirectoryManager(BlockDevice& dev, InodeManager& imng)
    : dev_(dev), imng_(imng) {}

// ---- Path helpers ----

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

// ---- Raw directory data I/O ----

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

// ---- Public API ----

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

int DirectoryManager::link(MemINode* dir_ip, const char* name, uint16_t ino) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return -1;
    }

    // Check for duplicate name
    for (auto& e : entries) {
        if (e.d_ino != 0 && std::strncmp(e.d_name, name, MAX_FILENAME) == 0) {
            return -1;
        }
    }

    // Find an empty slot (d_ino == 0)
    for (auto& e : entries) {
        if (e.d_ino == 0) {
            std::memset(e.d_name, 0, MAX_FILENAME);
            std::strncpy(e.d_name, name, MAX_FILENAME);
            e.d_ino = ino;
            return write_dir_data(dir_ip, entries);
        }
    }

    // No empty slot — append a new entry
    DiskDirEntry ne{};
    std::strncpy(ne.d_name, name, MAX_FILENAME);
    ne.d_ino = ino;
    entries.push_back(ne);
    return write_dir_data(dir_ip, entries);
}

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

int DirectoryManager::read_entries(MemINode* dir_ip, std::vector<DiskDirEntry>& out) {
    return read_dir_data(dir_ip, out);
}

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

bool DirectoryManager::is_empty(MemINode* dir_ip) {
    std::vector<DiskDirEntry> entries;
    if (read_dir_data(dir_ip, entries) != 0) {
        return false;
    }
    for (auto& e : entries) {
        if (e.d_ino != 0 &&
            std::strncmp(e.d_name, ".", MAX_FILENAME) != 0 &&
            std::strncmp(e.d_name, "..", MAX_FILENAME) != 0) {
            return false;
        }
    }
    return true;
}

uint16_t DirectoryManager::namei(const char* path, uint16_t cwd_ino, uint16_t root_ino) {
    auto parts = split_path(path);
    if (parts.empty()) {
        return (path[0] == '/') ? root_ino : cwd_ino;
    }

    // Determine starting point: absolute path starts from root
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

uint16_t DirectoryManager::namei_parent(const char* path, uint16_t cwd_ino, uint16_t root_ino,
                                         std::string& basename) {
    auto parts = split_path(path);
    if (parts.empty()) {
        return INVALID_BLK;
    }

    basename = parts.back();
    parts.pop_back();

    if (parts.empty()) {
        // Path is just a filename — parent is cwd (or root if absolute)
        return (path[0] == '/') ? root_ino : cwd_ino;
    }

    // Resolve the parent path
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
