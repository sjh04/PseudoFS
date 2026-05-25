#include "fs/fat16/fat16_fs.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace pfs {

// --- Forward-declared helpers ---
static std::string to_upper(const std::string& s);
static std::string entry_name(const FAT16DirEntry& e);
static size_t data_size(const std::vector<uint8_t>& data);

Fat16Fs::Fat16Fs(BlockDevice& dev)
    : dev_(dev)
    , cwd_path_("/")
    , cwd_cluster_(kRootCluster)
    , cur_uid_(0)
    , cur_gid_(0)
    , mounted_(false) {}

void Fat16Fs::set_user(uint16_t uid, uint16_t gid) {
    cur_uid_ = uid;
    cur_gid_ = gid;
}

// ======================== Lifecycle ========================

int Fat16Fs::fs_format() {
    std::memset(&boot_, 0, sizeof(boot_));
    std::strncpy(boot_.volume_label, "PFS-FAT16  ", 11);
    boot_.bytes_per_sector = 512;
    boot_.sectors_per_cluster = 1;
    boot_.reserved_sectors = 1;
    boot_.num_fats = 2;
    boot_.root_entry_count = 16;
    boot_.total_sectors = TOTAL_BLK_NUM;
    boot_.fat_size_sectors = kFatSize;
    dev_.write_block(kBootBlk, &boot_);

    std::vector<uint8_t> fat_blk(BLOCK_SIZE, 0);
    for (uint32_t i = 0; i < kFatSize; ++i) {
        dev_.write_block(kFat1Start + i, fat_blk.data());
        dev_.write_block(kFat2Start + i, fat_blk.data());
    }

    fat_.assign(kClustersPerFat, 0);
    fat_[0] = 0xFFF8;
    fat_[1] = 0xFFFF;
    flush_fat();

    fat_blk.assign(BLOCK_SIZE, 0);
    dev_.write_block(kRootDirBlk, fat_blk.data());

    cwd_path_ = "/";
    cwd_cluster_ = kRootCluster;
    mounted_ = true;
    return 0;
}

int Fat16Fs::fs_mount() {
    int ret = dev_.read_block(kBootBlk, &boot_);
    if (ret != 0) return -1;
    if (boot_.bytes_per_sector != 512) return -1;
    if (boot_.total_sectors != TOTAL_BLK_NUM) return -1;

    init_fat_from_disk();
    cwd_path_ = "/";
    cwd_cluster_ = kRootCluster;
    mounted_ = true;
    return 0;
}

int Fat16Fs::fs_unmount() {
    if (!mounted_) return -1;
    flush_fat();
    dev_.write_block(kBootBlk, &boot_);
    mounted_ = false;
    return 0;
}

// ======================== File Operations ========================

int Fat16Fs::fs_create(const char* path, uint16_t /*mode*/) {
    if (!mounted_) return -1;

    uint16_t parent_cluster, unused;
    std::string name;
    int ret = resolve_path(path, unused, parent_cluster, name);
    if (ret == 0) return -1;  // already exists
    if (parent_cluster == kRootCluster && ret != -2) return -1;

    uint16_t first_cluster = alloc_cluster();
    if (first_cluster == FAT16_END_OF_CHAIN) return -1;
    fat_[first_cluster] = FAT16_END_OF_CHAIN;
    flush_fat();

    // Zero the new cluster
    std::vector<uint8_t> zero_blk(BLOCK_SIZE, 0);
    dev_.write_block(cluster_to_block(first_cluster), zero_blk.data());

    FAT16DirEntry entry;
    std::memset(&entry, 0, sizeof(entry));
    set_dir_entry_name(entry, name.c_str());
    entry.attr = FAT16_ATTR_ARCHIVE;
    entry.first_cluster = first_cluster;
    entry.file_size = 0;
    timestamp(entry.time, entry.date);

    return append_dir_entry(parent_cluster, entry);
}

int Fat16Fs::fs_open(const char* path, int flags) {
    if (!mounted_) return -1;

    uint16_t cluster;
    uint16_t parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e)) {
            int fd = oft_.alloc_fd(cur_uid_, e.first_cluster,
                                   static_cast<uint8_t>(flags));
            return fd;
        }
    }
    return -1;
}

int Fat16Fs::fs_close(int fd) {
    return oft_.free_fd(cur_uid_, fd);
}

ssize_t Fat16Fs::fs_read(int fd, void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(cur_uid_, fd)) return -1;

    uint16_t first_cluster = oft_.get_inode(cur_uid_, fd);
    uint32_t offset = oft_.get_offset(cur_uid_, fd);

    std::vector<uint8_t> data;
    if (read_cluster_chain(first_cluster, data) != 0) return -1;

    size_t real_size = data_size(data);
    if (offset >= real_size) return 0;
    size_t to_read = std::min(len, real_size - offset);
    std::memcpy(buf, data.data() + offset, to_read);
    oft_.set_offset(cur_uid_, fd, offset + to_read);

    return static_cast<ssize_t>(to_read);
}

ssize_t Fat16Fs::fs_write(int fd, const void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(cur_uid_, fd)) return -1;

    uint8_t flags = oft_.get_flags(cur_uid_, fd);
    if (!(flags & O_WRITE) && !(flags & O_APPEND)) return -1;

    uint16_t first_cluster = oft_.get_inode(cur_uid_, fd);
    uint32_t offset = oft_.get_offset(cur_uid_, fd);

    bool append = (flags & O_APPEND) != 0;
    if (append) {
        std::vector<uint8_t> existing;
        read_cluster_chain(first_cluster, existing);
        offset = existing.size();
    }

    std::vector<uint8_t> existing;
    read_cluster_chain(first_cluster, existing);
    std::vector<uint8_t> combined(offset + len);
    size_t copy_existing = std::min(existing.size(), static_cast<size_t>(offset));
    std::memcpy(combined.data(), existing.data(), copy_existing);
    std::memcpy(combined.data() + offset, buf, len);

    // Free old chain, write new
    free_cluster_chain(first_cluster);
    uint16_t new_first;
    int ret = write_cluster_chain(FAT16_END_OF_CHAIN, combined.data(),
                                  combined.size(), new_first);
    if (ret != 0) return -1;

    // Update open file table entry to point to new first cluster.
    // We close and reopen the fd internally to keep the table consistent.
    oft_.free_fd(cur_uid_, fd);
    int new_fd = oft_.alloc_fd(cur_uid_, new_first, flags);
    if (new_fd < 0) return -1;
    oft_.set_offset(cur_uid_, new_fd, offset + len);

    return static_cast<ssize_t>(len);
}

int Fat16Fs::fs_delete(const char* path) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e)) {
            if (e.attr & FAT16_ATTR_DIRECTORY) return -1;
            free_cluster_chain(e.first_cluster);
            break;
        }
    }

    return remove_dir_entry(parent, name.c_str());
}

// ======================== Directory Operations ========================

int Fat16Fs::fs_mkdir(const char* path) {
    if (!mounted_) return -1;

    uint16_t unused, parent_cluster;
    std::string name;
    int ret = resolve_path(path, unused, parent_cluster, name);
    if (ret == 0) return -1;

    uint16_t dir_cluster = alloc_cluster();
    if (dir_cluster == FAT16_END_OF_CHAIN) return -1;
    fat_[dir_cluster] = FAT16_END_OF_CHAIN;
    flush_fat();

    std::vector<uint8_t> dir_block(BLOCK_SIZE, 0);
    auto* dot = reinterpret_cast<FAT16DirEntry*>(dir_block.data());
    std::memset(dot, 0, sizeof(FAT16DirEntry));
    std::memcpy(dot->name, ".       ", 8);
    dot->attr = FAT16_ATTR_DIRECTORY;
    dot->first_cluster = dir_cluster;
    timestamp(dot->time, dot->date);

    auto* dotdot = reinterpret_cast<FAT16DirEntry*>(dir_block.data() + 32);
    std::memset(dotdot, 0, sizeof(FAT16DirEntry));
    std::memcpy(dotdot->name, "..      ", 8);
    dotdot->attr = FAT16_ATTR_DIRECTORY;
    dotdot->first_cluster = parent_cluster;
    timestamp(dotdot->time, dotdot->date);

    dev_.write_block(cluster_to_block(dir_cluster), dir_block.data());

    FAT16DirEntry entry;
    std::memset(&entry, 0, sizeof(entry));
    set_dir_entry_name(entry, name.c_str());
    entry.attr = FAT16_ATTR_DIRECTORY;
    entry.first_cluster = dir_cluster;
    entry.file_size = 0;
    timestamp(entry.time, entry.date);

    return append_dir_entry(parent_cluster, entry);
}

int Fat16Fs::fs_rmdir(const char* path) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    // Check directory is empty — scan for non-. and non-.. entries
    std::vector<uint8_t> tmp(BLOCK_SIZE);
    dev_.read_block(cluster_to_block(cluster), tmp.data());
    bool empty = true;
    for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
        auto* e = reinterpret_cast<FAT16DirEntry*>(tmp.data() + i * 32);
        if (e->name[0] == 0) break;
        if (e->name[0] == static_cast<char>(0xE5)) continue;
        if (e->name[0] != '.') { empty = false; break; }
    }
    if (!empty) return -1;

    free_cluster_chain(cluster);

    // Remove entry from parent directory
    std::string upper = to_upper(name);
    if (parent == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<FAT16DirEntry*>(blk.data() + i * 32);
            if (upper == entry_name(*e)) {
                e->name[0] = static_cast<char>(0xE5);
                dev_.write_block(kRootDirBlk, blk.data());
                return 0;
            }
        }
        return -1;
    }
    return remove_dir_entry(parent, name.c_str());
}

int Fat16Fs::fs_chdir(const char* path) {
    if (!mounted_) return -1;

    // Handle root directory specially
    if (std::strcmp(path, "/") == 0) {
        cwd_path_ = "/";
        cwd_cluster_ = kRootCluster;
        return 0;
    }

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    // Also need to check that the final component is a directory
    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e) &&
            (e.attr & FAT16_ATTR_DIRECTORY)) {
            if (path[0] == '/') {
                cwd_path_ = path;
                cwd_cluster_ = e.first_cluster;
            } else {
                if (cwd_path_ == "/")
                    cwd_path_ = "/" + std::string(path);
                else
                    cwd_path_ = cwd_path_ + "/" + std::string(path);
                cwd_cluster_ = e.first_cluster;
            }
            return 0;
        }
    }
    return -1;
}

int Fat16Fs::fs_ls(const char* path, std::vector<DirEntry>& out) {
    if (!mounted_) return -1;

    uint16_t target = cwd_cluster_;
    if (path && path[0] != '\0') {
        uint16_t parent;
        std::string name;
        if (resolve_path(path, target, parent, name) != 0) return -1;
    }

    out.clear();
    std::vector<FAT16DirEntry> entries;
    if (read_dir_entries(target, entries) != 0) return -1;

    for (auto& e : entries) {
        if (e.name[0] == 0) continue;
        if (e.name[0] == static_cast<char>(0xE5)) continue;

        DirEntry ve;
        fat16_to_vfs_entry(e, ve);
        out.push_back(ve);
    }

    return 0;
}

std::string Fat16Fs::fs_pwd() {
    return cwd_path_;
}

// ======================== Metadata ========================

int Fat16Fs::fs_stat(const char* path, FileStat& out) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e)) {
            std::memset(&out, 0, sizeof(out));
            out.mode = (e.attr & FAT16_ATTR_DIRECTORY) ? MODE_DIR : MODE_FILE;
            out.mode |= DEFAULT_MODE;
            out.uid = cur_uid_;
            out.gid = cur_gid_;
            if (e.attr & FAT16_ATTR_DIRECTORY) {
                out.size = 0;
            } else {
                std::vector<uint8_t> file_data;
                read_cluster_chain(e.first_cluster, file_data);
                out.size = data_size(file_data);
            }
            out.nlink = 1;
            out.type = (e.attr & FAT16_ATTR_DIRECTORY) ? TYPE_DIR : TYPE_FILE;
            return 0;
        }
    }
    return -1;
}

int Fat16Fs::fs_chmod(const char* /*path*/, uint16_t /*mode*/) {
    return 0;
}

int Fat16Fs::fs_link(const char* /*src*/, const char* /*dst*/) {
    return -1;
}

std::string Fat16Fs::fs_type_name() const { return "FAT16"; }

DiskUsage Fat16Fs::fs_disk_usage() const {
    DiskUsage usage;
    usage.total_blocks = TOTAL_BLK_NUM;
    usage.total_inodes = 0;

    uint32_t used = kDataStart;
    for (uint32_t i = 2; i < kClustersPerFat; ++i) {
        if (fat_[i] != FAT16_FREE_CLUSTER) ++used;
    }
    usage.used_blocks = used;
    usage.used_inodes = 0;

    return usage;
}

// ======================== Internal Helpers ========================

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return r;
}

static size_t data_size(const std::vector<uint8_t>& data) {
    size_t sz = data.size();
    while (sz > 0 && data[sz - 1] == 0) --sz;
    return sz;
}

static std::string entry_name(const FAT16DirEntry& e) {
    std::string n;
    for (int i = 0; i < 8 && e.name[i] != ' '; ++i) n += e.name[i];
    if (e.ext[0] != ' ') {
        n += '.';
        for (int i = 0; i < 3 && e.ext[i] != ' '; ++i) n += e.ext[i];
    }
    return n;
}

uint32_t Fat16Fs::cluster_to_block(uint16_t cluster) const {
    if (cluster < 2) return 0;
    return kDataStart + (cluster - 2);
}

int Fat16Fs::read_fat_entry(uint16_t cluster, uint16_t& out) const {
    if (cluster >= kClustersPerFat) return -1;
    out = fat_[cluster];
    return 0;
}

int Fat16Fs::write_fat_entry(uint16_t cluster, uint16_t value) {
    if (cluster >= kClustersPerFat) return -1;
    fat_[cluster] = value;
    return 0;
}

uint16_t Fat16Fs::alloc_cluster() {
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) {
            fat_[i] = FAT16_END_OF_CHAIN;
            return i;
        }
    }
    return FAT16_END_OF_CHAIN;
}

int Fat16Fs::free_cluster_chain(uint16_t start_cluster) {
    uint16_t current = start_cluster;
    int count = 0;
    while (current >= 2 && current != FAT16_END_OF_CHAIN && count < 2000) {
        uint16_t next = fat_[current];
        fat_[current] = FAT16_FREE_CLUSTER;
        current = next;
        ++count;
    }
    flush_fat();
    return 0;
}

int Fat16Fs::read_cluster_chain(uint16_t start_cluster,
                                std::vector<uint8_t>& data,
                                size_t max_bytes) const {
    data.clear();
    if (start_cluster < 2 || start_cluster >= kClustersPerFat) return -1;

    uint16_t current = start_cluster;
    int count = 0;
    while (current >= 2 && current != FAT16_END_OF_CHAIN && count < 2000) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        uint32_t blk_no = cluster_to_block(current);
        if (dev_.read_block(blk_no, blk.data()) != 0) return -1;
        size_t to_add = BLOCK_SIZE;
        if (data.size() + to_add > max_bytes)
            to_add = max_bytes - data.size();
        data.insert(data.end(), blk.begin(), blk.begin() + to_add);
        if (data.size() >= max_bytes) break;
        current = fat_[current];
        ++count;
    }
    return 0;
}

int Fat16Fs::write_cluster_chain(uint16_t /*old_first*/,
                                 const void* data, size_t len,
                                 uint16_t& out_first_cluster) {
    size_t blocks_needed = (len + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (blocks_needed == 0) {
        out_first_cluster = FAT16_END_OF_CHAIN;
        return 0;
    }

    std::vector<uint16_t> clusters;
    for (size_t i = 0; i < blocks_needed; ++i) {
        uint16_t c = alloc_cluster();
        if (c == FAT16_END_OF_CHAIN) {
            for (auto prev : clusters) free_cluster_chain(prev);
            return -1;
        }
        clusters.push_back(c);
        if (i > 0) fat_[clusters[i - 1]] = c;
    }
    fat_[clusters.back()] = FAT16_END_OF_CHAIN;
    flush_fat();
    out_first_cluster = clusters[0];

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < blocks_needed; ++i) {
        std::vector<uint8_t> blk(BLOCK_SIZE, 0);
        size_t chunk = std::min(static_cast<size_t>(BLOCK_SIZE),
                                len - i * BLOCK_SIZE);
        std::memcpy(blk.data(), ptr + i * BLOCK_SIZE, chunk);
        dev_.write_block(cluster_to_block(clusters[i]), blk.data());
    }
    return 0;
}

int Fat16Fs::resolve_path(const char* path, uint16_t& out_cluster,
                          uint16_t& out_parent, std::string& out_name) const {
    if (path == nullptr || path[0] == '\0') return -1;

    uint16_t current_dir = (path[0] == '/') ? kRootCluster : cwd_cluster_;
    std::string remaining(path[0] == '/' ? path + 1 : path);

    if (remaining.empty()) {
        out_cluster = current_dir;
        out_parent = current_dir;
        out_name = "";
        return 0;
    }

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < remaining.size()) {
        size_t slash = remaining.find('/', pos);
        if (slash == std::string::npos) {
            parts.push_back(remaining.substr(pos));
            break;
        }
        parts.push_back(remaining.substr(pos, slash - pos));
        pos = slash + 1;
    }

    // Walk through all but the last component
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::vector<FAT16DirEntry> entries;
        read_dir_entries(current_dir, entries);
        bool found = false;
        std::string part_upper = to_upper(parts[i]);
        for (auto& e : entries) {
            if (e.name[0] == 0 || e.name[0] == static_cast<char>(0xE5)) continue;
            if (!(e.attr & FAT16_ATTR_DIRECTORY)) continue;
            if (part_upper == entry_name(e)) {
                current_dir = e.first_cluster;
                found = true;
                break;
            }
        }
        if (!found) return -1;
    }

    out_name = to_upper(parts.back());
    out_parent = current_dir;

    std::vector<FAT16DirEntry> entries;
    read_dir_entries(current_dir, entries);
    for (auto& e : entries) {
        if (e.name[0] == 0 || e.name[0] == static_cast<char>(0xE5)) continue;
        if (out_name == entry_name(e)) {
            out_cluster = e.first_cluster;
            return 0;
        }
    }

    out_cluster = FAT16_END_OF_CHAIN;
    return -2;  // parent exists, name not found
}

int Fat16Fs::read_dir_entries(uint16_t cluster,
                              std::vector<FAT16DirEntry>& out) const {
    out.clear();
    if (cluster == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<const FAT16DirEntry*>(
                blk.data() + i * 32);
            if (e->name[0] == 0) break;
            out.push_back(*e);
        }
        return 0;
    }

    std::vector<uint8_t> data;
    if (read_cluster_chain(cluster, data) != 0) return -1;

    size_t count = data.size() / 32;
    for (size_t i = 0; i < count; ++i) {
        auto* e = reinterpret_cast<const FAT16DirEntry*>(data.data() + i * 32);
        if (e->name[0] == 0) break;
        out.push_back(*e);
    }
    return 0;
}

int Fat16Fs::write_dir_entry(uint16_t dir_cluster,
                             const FAT16DirEntry& entry) {
    return append_dir_entry(dir_cluster, entry);
}

int Fat16Fs::remove_dir_entry(uint16_t dir_cluster, const char* name) {
    std::string upper = to_upper(name);

    if (dir_cluster == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<FAT16DirEntry*>(blk.data() + i * 32);
            if (upper == entry_name(*e)) {
                e->name[0] = static_cast<char>(0xE5);
                dev_.write_block(kRootDirBlk, blk.data());
                return 0;
            }
        }
        return -1;
    }

    std::vector<uint8_t> data;
    read_cluster_chain(dir_cluster, data);
    for (size_t i = 0; i < data.size() / 32; ++i) {
        auto* e = reinterpret_cast<FAT16DirEntry*>(data.data() + i * 32);
        if (upper == entry_name(*e)) {
            e->name[0] = static_cast<char>(0xE5);
            uint16_t unused;
            write_cluster_chain(dir_cluster, data.data(), data.size(), unused);
            return 0;
        }
    }
    return -1;
}

int Fat16Fs::append_dir_entry(uint16_t dir_cluster,
                              const FAT16DirEntry& entry) {
    if (dir_cluster == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<FAT16DirEntry*>(blk.data() + i * 32);
            if (e->name[0] == 0 || e->name[0] == static_cast<char>(0xE5)) {
                std::memcpy(e, &entry, sizeof(FAT16DirEntry));
                dev_.write_block(kRootDirBlk, blk.data());
                return 0;
            }
        }
        return -1;
    }

    std::vector<uint8_t> data;
    int ret = read_cluster_chain(dir_cluster, data);
    if (ret == -1 || data.empty()) {
        data.assign(BLOCK_SIZE, 0);
    }

    size_t count = data.size() / 32;
    for (size_t i = 0; i < count; ++i) {
        auto* e = reinterpret_cast<FAT16DirEntry*>(data.data() + i * 32);
        if (e->name[0] == 0 || e->name[0] == static_cast<char>(0xE5)) {
            std::memcpy(e, &entry, sizeof(FAT16DirEntry));
            dev_.write_block(cluster_to_block(dir_cluster), data.data());
            return 0;
        }
    }

    size_t old_size = data.size();
    size_t new_size = old_size + 32;
    size_t new_blocks = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    new_size = new_blocks * BLOCK_SIZE;
    data.resize(new_size, 0);
    std::memcpy(data.data() + old_size, &entry, sizeof(FAT16DirEntry));

    free_cluster_chain(dir_cluster);
    uint16_t new_first;
    return write_cluster_chain(FAT16_END_OF_CHAIN, data.data(), new_size,
                               new_first);
}

uint16_t Fat16Fs::find_free_cluster() const {
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) return i;
    }
    return FAT16_END_OF_CHAIN;
}

void Fat16Fs::init_fat_from_disk() {
    fat_.assign(kClustersPerFat, 0);
    for (uint32_t i = 0; i < kFatSize; ++i) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kFat1Start + i, blk.data());
        for (uint32_t j = 0; j < kEntriesPerFatBlk; ++j) {
            uint32_t idx = i * kEntriesPerFatBlk + j;
            if (idx >= kClustersPerFat) break;
            fat_[idx] =
                blk[j * 2] | (static_cast<uint16_t>(blk[j * 2 + 1]) << 8);
        }
    }
}

void Fat16Fs::flush_fat() {
    for (uint32_t i = 0; i < kFatSize; ++i) {
        std::vector<uint8_t> blk(BLOCK_SIZE, 0);
        for (uint32_t j = 0; j < kEntriesPerFatBlk; ++j) {
            uint32_t idx = i * kEntriesPerFatBlk + j;
            if (idx >= kClustersPerFat) break;
            blk[j * 2] = fat_[idx] & 0xFF;
            blk[j * 2 + 1] = (fat_[idx] >> 8) & 0xFF;
        }
        dev_.write_block(kFat1Start + i, blk.data());
        dev_.write_block(kFat2Start + i, blk.data());
    }
}

void Fat16Fs::set_dir_entry_name(FAT16DirEntry& e, const char* name) {
    std::memset(e.name, ' ', 8);
    std::memset(e.ext, ' ', 3);

    const char* dot = std::strrchr(name, '.');
    size_t base_len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);

    for (size_t i = 0; i < base_len && i < 8; ++i) {
        e.name[i] = (name[i] >= 'a' && name[i] <= 'z')
                        ? static_cast<char>(name[i] - 32)
                        : name[i];
    }

    if (dot && dot[1] != '\0') {
        const char* ext = dot + 1;
        for (size_t i = 0; i < 3 && ext[i] != '\0'; ++i) {
            e.ext[i] = (ext[i] >= 'a' && ext[i] <= 'z')
                           ? static_cast<char>(ext[i] - 32)
                           : ext[i];
        }
    }
}

void Fat16Fs::fat16_to_vfs_entry(const FAT16DirEntry& fe, DirEntry& ve) const {
    std::memset(&ve, 0, sizeof(ve));
    std::string n = entry_name(fe);
    std::strncpy(ve.name, n.c_str(), MAX_FILENAME - 1);
    ve.name[MAX_FILENAME - 1] = '\0';
    ve.inode_no = fe.first_cluster;
    ve.type = (fe.attr & FAT16_ATTR_DIRECTORY) ? TYPE_DIR : TYPE_FILE;
    ve.size = fe.file_size;
}

void Fat16Fs::timestamp(uint16_t& t, uint16_t& d) const {
    std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    t = static_cast<uint16_t>(
        (tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    d = static_cast<uint16_t>(
        ((tm->tm_year + 1900 - 1980) << 9) | ((tm->tm_mon + 1) << 5) |
        tm->tm_mday);
}

uint32_t Fat16Fs::count_clusters(uint16_t start) const {
    uint32_t count = 0;
    uint16_t cur = start;
    while (cur >= 2 && cur != FAT16_END_OF_CHAIN && count < 2000) {
        ++count;
        cur = fat_[cur];
    }
    return count;
}

}  // namespace pfs
