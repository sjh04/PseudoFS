#include "fs/fat16/fat16_fs.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace pfs {

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return r;
}

static std::string entry_name(const FAT16DirEntry& e) {
    std::string n;
    for (int i = 0; i < 8 && e.name[i] != ' ' && e.name[i] != '\0'; ++i) n += e.name[i];
    bool has_ext = false;
    for (int i = 0; i < 3; ++i) {
        if (e.ext[i] != ' ' && e.ext[i] != '\0') {
            has_ext = true;
            break;
        }
    }
    if (has_ext) {
        n += '.';
        for (int i = 0; i < 3 && e.ext[i] != ' ' && e.ext[i] != '\0'; ++i) n += e.ext[i];
    }
    return n;
}

Fat16Fs::Fat16Fs(BlockDevice& dev)
    : dev_(dev),
      cwd_path_("/"),
      cwd_cluster_(kRootCluster),
      cur_uid_(0),
      cur_gid_(0),
      mounted_(false) {
}

void Fat16Fs::set_user(uint16_t uid, uint16_t gid) {
    cur_uid_ = uid;
    cur_gid_ = gid;
}

void Fat16Fs::set_disk_path(const std::string& path) {
    disk_path_ = path;
}

void Fat16Fs::sync() {
    if (disk_path_.empty()) return;
    flush_fat();
    dev_.write_block(kBootBlk, &boot_);
    dev_.save_to_file(disk_path_.c_str());
}

// ======================== Lifecycle ========================

int Fat16Fs::fs_format() {
    std::memset(&boot_, 0, sizeof(boot_));
    std::strncpy(boot_.volume_label, "PFS-FAT16  ", 11);
    boot_.bytes_per_sector = 512;
    boot_.sectors_per_cluster = 1;
    boot_.reserved_sectors = 1;
    boot_.num_fats = 2;
    boot_.root_entry_count = kDirEntriesPerBlk;
    boot_.total_sectors = TOTAL_BLK_NUM;
    boot_.fat_size_sectors = kFatSize;
    dev_.write_block(kBootBlk, &boot_);

    std::vector<uint8_t> zero_blk(BLOCK_SIZE, 0);
    for (uint32_t i = 0; i < kFatSize; ++i) {
        dev_.write_block(kFat1Start + i, zero_blk.data());
        dev_.write_block(kFat2Start + i, zero_blk.data());
    }

    fat_.assign(kClustersPerFat, 0);
    fat_[0] = 0xFFF8;
    fat_[1] = 0xFFFF;
    flush_fat();

    dev_.write_block(kRootDirBlk, zero_blk.data());

    cwd_path_ = "/";
    cwd_cluster_ = kRootCluster;
    mounted_ = true;
    sync();
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
    if (ret == 0) return -1;   // already exists
    if (ret != -2) return -1;  // parent not found

    uint16_t first_cluster = alloc_cluster();
    if (first_cluster == FAT16_END_OF_CHAIN) return -1;
    fat_[first_cluster] = FAT16_END_OF_CHAIN;
    flush_fat();

    // Zero the new cluster
    uint8_t zero_blk[BLOCK_SIZE] = {};
    dev_.write_block(cluster_to_block(first_cluster), zero_blk);

    FAT16DirEntry entry;
    std::memset(&entry, 0, sizeof(entry));
    set_dir_entry_name(entry, name.c_str());
    entry.attr = FAT16_ATTR_ARCHIVE;
    entry.first_cluster = first_cluster;
    entry.file_size = 0;
    timestamp(entry.time, entry.date);

    if (append_dir_entry(parent_cluster, entry) != 0) {
        free_cluster_chain(first_cluster);
        flush_fat();
        return -1;
    }
    sync();
    return 0;
}

int Fat16Fs::fs_open(const char* path, int flags) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    // Find the dir entry to get first_cluster
    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e)) {
            int fd = oft_.alloc_fd(cur_uid_, e.first_cluster, static_cast<uint8_t>(flags));
            if (fd >= 0 && (flags & O_APPEND)) {
                oft_.set_offset(cur_uid_, fd, e.file_size);
            }
            return fd;
        }
    }
    return -1;
}

int Fat16Fs::fs_close(int fd) {
    int ret = oft_.free_fd(cur_uid_, fd);
    if (ret == 0) sync();
    return ret;
}

ssize_t Fat16Fs::fs_read(int fd, void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(cur_uid_, fd)) return -1;
    if (!(oft_.get_flags(cur_uid_, fd) & O_READ)) return -1;

    uint16_t first_cluster = oft_.get_inode(cur_uid_, fd);
    uint32_t offset = oft_.get_offset(cur_uid_, fd);

    // Find file_size from directory entry
    uint32_t file_size = get_file_size(first_cluster);
    if (offset >= file_size) return 0;
    size_t to_read = std::min(len, static_cast<size_t>(file_size - offset));

    // Walk cluster chain to the right position
    auto* dst = static_cast<uint8_t*>(buf);
    size_t bytes_read = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    uint16_t cur_cluster = first_cluster;
    uint32_t skip_blocks = offset / BLOCK_SIZE;

    // Skip to the right cluster
    for (uint32_t i = 0; i < skip_blocks && cur_cluster >= 2 && cur_cluster != FAT16_END_OF_CHAIN;
         ++i) {
        cur_cluster = fat_[cur_cluster];
    }

    uint32_t blk_off = offset % BLOCK_SIZE;

    while (bytes_read < to_read && cur_cluster >= 2 && cur_cluster != FAT16_END_OF_CHAIN) {
        dev_.read_block(cluster_to_block(cur_cluster), blk_buf);
        uint32_t chunk = std::min(static_cast<size_t>(BLOCK_SIZE - blk_off), to_read - bytes_read);
        std::memcpy(dst + bytes_read, blk_buf + blk_off, chunk);
        bytes_read += chunk;
        blk_off = 0;
        cur_cluster = fat_[cur_cluster];
    }

    oft_.set_offset(cur_uid_, fd, offset + bytes_read);
    return static_cast<ssize_t>(bytes_read);
}

ssize_t Fat16Fs::fs_write(int fd, const void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(cur_uid_, fd)) return -1;

    uint8_t flags = oft_.get_flags(cur_uid_, fd);
    if (!(flags & O_WRITE) && !(flags & O_APPEND)) return -1;

    uint16_t first_cluster = oft_.get_inode(cur_uid_, fd);
    uint32_t offset = oft_.get_offset(cur_uid_, fd);

    auto* src = static_cast<const uint8_t*>(buf);
    size_t bytes_written = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    // Walk to the cluster at offset
    uint16_t cur_cluster = first_cluster;
    uint16_t prev_cluster = 0;
    uint32_t skip_blocks = offset / BLOCK_SIZE;

    for (uint32_t i = 0; i < skip_blocks; ++i) {
        prev_cluster = cur_cluster;
        uint16_t next = fat_[cur_cluster];
        if (next == FAT16_END_OF_CHAIN || next < 2) {
            // Allocate a new cluster for the gap
            uint16_t new_c = alloc_cluster();
            if (new_c == FAT16_END_OF_CHAIN) goto done;
            fat_[cur_cluster] = new_c;
            fat_[new_c] = FAT16_END_OF_CHAIN;
            std::memset(blk_buf, 0, BLOCK_SIZE);
            dev_.write_block(cluster_to_block(new_c), blk_buf);
            cur_cluster = new_c;
        } else {
            cur_cluster = next;
        }
    }

    {
        uint32_t blk_off = offset % BLOCK_SIZE;

        while (bytes_written < len) {
            // Read existing block content for partial writes
            dev_.read_block(cluster_to_block(cur_cluster), blk_buf);

            uint32_t chunk =
                std::min(static_cast<size_t>(BLOCK_SIZE - blk_off), len - bytes_written);
            std::memcpy(blk_buf + blk_off, src + bytes_written, chunk);
            dev_.write_block(cluster_to_block(cur_cluster), blk_buf);
            bytes_written += chunk;
            blk_off = 0;

            if (bytes_written < len) {
                // Move to next cluster, allocate if needed
                prev_cluster = cur_cluster;
                uint16_t next = fat_[cur_cluster];
                if (next == FAT16_END_OF_CHAIN || next < 2) {
                    uint16_t new_c = alloc_cluster();
                    if (new_c == FAT16_END_OF_CHAIN) goto done;
                    fat_[cur_cluster] = new_c;
                    fat_[new_c] = FAT16_END_OF_CHAIN;
                    std::memset(blk_buf, 0, BLOCK_SIZE);
                    dev_.write_block(cluster_to_block(new_c), blk_buf);
                    cur_cluster = new_c;
                } else {
                    cur_cluster = next;
                }
            }
        }
    }

done:
    flush_fat();

    // Update file size in directory entry
    uint32_t new_end = offset + bytes_written;
    uint32_t old_size = get_file_size(first_cluster);
    if (new_end > old_size) {
        update_file_size(first_cluster, new_end);
    }

    oft_.set_offset(cur_uid_, fd, new_end);
    sync();
    return static_cast<ssize_t>(bytes_written);
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
            if (e.first_cluster >= 2) free_cluster_chain(e.first_cluster);
            break;
        }
    }

    int ret = remove_dir_entry(parent, name.c_str());
    if (ret == 0) sync();
    return ret;
}

int Fat16Fs::fs_delete_recursive(const char* path) {
    if (!mounted_) return -1;

    FileStat st{};
    if (fs_stat(path, st) != 0) return -1;

    if (st.type == TYPE_FILE) {
        return fs_delete(path);
    }

    std::vector<DirEntry> entries;
    if (fs_ls(path, entries) != 0) return -1;

    for (auto& e : entries) {
        std::string n(e.name);
        if (n == "." || n == "..") continue;
        std::string child = std::string(path);
        if (child.back() != '/') child += "/";
        child += n;
        if (fs_delete_recursive(child.c_str()) != 0) return -1;
    }

    return fs_rmdir(path);
}

// ======================== Directory Operations ========================

int Fat16Fs::fs_mkdir(const char* path) {
    if (!mounted_) return -1;

    uint16_t unused, parent_cluster;
    std::string name;
    int ret = resolve_path(path, unused, parent_cluster, name);
    if (ret == 0) return -1;
    if (ret != -2) return -1;

    uint16_t dir_cluster = alloc_cluster();
    if (dir_cluster == FAT16_END_OF_CHAIN) return -1;
    fat_[dir_cluster] = FAT16_END_OF_CHAIN;
    flush_fat();

    std::vector<uint8_t> dir_block(BLOCK_SIZE, 0);
    auto* dot = reinterpret_cast<FAT16DirEntry*>(dir_block.data());
    std::memset(dot, 0, sizeof(FAT16DirEntry));
    std::memcpy(dot->name, ".       ", 8);
    std::memset(dot->ext, ' ', 3);
    dot->attr = FAT16_ATTR_DIRECTORY;
    dot->first_cluster = dir_cluster;
    timestamp(dot->time, dot->date);

    auto* dotdot = reinterpret_cast<FAT16DirEntry*>(dir_block.data() + 32);
    std::memset(dotdot, 0, sizeof(FAT16DirEntry));
    std::memcpy(dotdot->name, "..      ", 8);
    std::memset(dotdot->ext, ' ', 3);
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

    if (append_dir_entry(parent_cluster, entry) != 0) {
        free_cluster_chain(dir_cluster);
        flush_fat();
        return -1;
    }
    sync();
    return 0;
}

int Fat16Fs::fs_rmdir(const char* path) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    std::vector<FAT16DirEntry> dir_entries;
    read_dir_entries(cluster, dir_entries);
    for (auto& e : dir_entries) {
        if (e.name[0] == 0 || e.name[0] == static_cast<char>(0xE5)) continue;
        std::string en = entry_name(e);
        if (en != "." && en != "..") return -1;
    }

    free_cluster_chain(cluster);
    int ret = remove_dir_entry(parent, name.c_str());
    if (ret == 0) sync();
    return ret;
}

int Fat16Fs::fs_chdir(const char* path) {
    if (!mounted_) return -1;

    if (std::strcmp(path, "/") == 0) {
        cwd_path_ = "/";
        cwd_cluster_ = kRootCluster;
        return 0;
    }

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    cwd_cluster_ = cluster;

    if (cwd_cluster_ == kRootCluster) {
        cwd_path_ = "/";
        return 0;
    }

    if (path[0] == '/') {
        cwd_path_ = path;
    } else {
        std::vector<std::string> parts;
        size_t s = 0;
        while (s < cwd_path_.size()) {
            if (cwd_path_[s] == '/') {
                s++;
                continue;
            }
            size_t e2 = cwd_path_.find('/', s);
            if (e2 == std::string::npos) e2 = cwd_path_.size();
            parts.push_back(cwd_path_.substr(s, e2 - s));
            s = e2 + 1;
        }
        std::string rel(path);
        s = 0;
        while (s < rel.size()) {
            if (rel[s] == '/') {
                s++;
                continue;
            }
            size_t e2 = rel.find('/', s);
            if (e2 == std::string::npos) e2 = rel.size();
            std::string comp = rel.substr(s, e2 - s);
            if (comp == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (comp != ".") {
                parts.push_back(comp);
            }
            s = e2 + 1;
        }
        cwd_path_ = "/";
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) cwd_path_ += "/";
            cwd_path_ += parts[i];
        }
        if (cwd_path_.empty()) cwd_path_ = "/";
    }
    return 0;
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
            out.size = e.file_size;
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

std::string Fat16Fs::fs_type_name() const {
    return "FAT16";
}

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

void Fat16Fs::fs_block_map(std::vector<uint8_t>& out) {
    out.assign(TOTAL_BLK_NUM, BLK_FREE);
    // Boot sector + both FATs + the root directory region are fixed metadata.
    for (uint32_t b = 0; b < kDataStart; ++b) out[b] = BLK_META;
    // Each data cluster maps to one block; the FAT entry says used vs free.
    for (uint16_t c = 2; c < 2 + kDataBlkCount; ++c) {
        if (c >= fat_.size()) break;
        uint32_t blk = cluster_to_block(c);
        if (blk >= TOTAL_BLK_NUM) break;
        out[blk] = (fat_[c] == FAT16_FREE_CLUSTER) ? BLK_FREE : BLK_USED;
    }
}

// ======================== Internal Helpers ========================

uint32_t Fat16Fs::cluster_to_block(uint16_t cluster) const {
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

int Fat16Fs::read_cluster_chain(uint16_t start_cluster, std::vector<uint8_t>& data,
                                size_t max_bytes) const {
    data.clear();
    if (start_cluster < 2 || start_cluster >= kClustersPerFat) return -1;

    uint16_t current = start_cluster;
    int count = 0;
    while (current >= 2 && current != FAT16_END_OF_CHAIN && count < 2000) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(cluster_to_block(current), blk.data());
        size_t to_add = BLOCK_SIZE;
        if (data.size() + to_add > max_bytes) to_add = max_bytes - data.size();
        data.insert(data.end(), blk.begin(), blk.begin() + to_add);
        if (data.size() >= max_bytes) break;
        current = fat_[current];
        ++count;
    }
    return 0;
}

int Fat16Fs::write_cluster_chain(uint16_t /*old_first*/, const void* data, size_t len,
                                 uint16_t& out_first_cluster) {
    size_t blocks_needed = (len + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (blocks_needed == 0) {
        out_first_cluster = 0;
        return 0;
    }

    std::vector<uint16_t> clusters;
    for (size_t i = 0; i < blocks_needed; ++i) {
        uint16_t c = alloc_cluster();
        if (c == FAT16_END_OF_CHAIN) {
            for (auto prev : clusters) {
                fat_[prev] = FAT16_FREE_CLUSTER;
            }
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
        size_t chunk = std::min(static_cast<size_t>(BLOCK_SIZE), len - i * BLOCK_SIZE);
        std::memcpy(blk.data(), ptr + i * BLOCK_SIZE, chunk);
        dev_.write_block(cluster_to_block(clusters[i]), blk.data());
    }
    return 0;
}

int Fat16Fs::resolve_path(const char* path, uint16_t& out_cluster, uint16_t& out_parent,
                          std::string& out_name) const {
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
        if (slash > pos) parts.push_back(remaining.substr(pos, slash - pos));
        pos = slash + 1;
    }
    if (parts.empty()) {
        out_cluster = current_dir;
        out_parent = current_dir;
        out_name = "";
        return 0;
    }

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
    return -2;
}

int Fat16Fs::read_dir_entries(uint16_t cluster, std::vector<FAT16DirEntry>& out) const {
    out.clear();
    if (cluster == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<const FAT16DirEntry*>(blk.data() + i * 32);
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

int Fat16Fs::write_dir_entry(uint16_t dir_cluster, const FAT16DirEntry& entry) {
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
            // Write back to the correct block in the chain
            uint16_t cur = dir_cluster;
            size_t blk_idx = (i * 32) / BLOCK_SIZE;
            for (size_t b = 0; b < blk_idx && cur != FAT16_END_OF_CHAIN; ++b) {
                cur = fat_[cur];
            }
            if (cur >= 2 && cur != FAT16_END_OF_CHAIN) {
                dev_.write_block(cluster_to_block(cur), data.data() + blk_idx * BLOCK_SIZE);
            }
            return 0;
        }
    }
    return -1;
}

int Fat16Fs::append_dir_entry(uint16_t dir_cluster, const FAT16DirEntry& entry) {
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

    // For subdirectories, scan the chain for an empty slot
    std::vector<uint8_t> data;
    read_cluster_chain(dir_cluster, data);

    for (size_t i = 0; i < data.size() / 32; ++i) {
        auto* e = reinterpret_cast<FAT16DirEntry*>(data.data() + i * 32);
        if (e->name[0] == 0 || e->name[0] == static_cast<char>(0xE5)) {
            std::memcpy(e, &entry, sizeof(FAT16DirEntry));
            // Write back the block that changed
            uint16_t cur = dir_cluster;
            size_t blk_idx = (i * 32) / BLOCK_SIZE;
            for (size_t b = 0; b < blk_idx && cur != FAT16_END_OF_CHAIN; ++b) {
                cur = fat_[cur];
            }
            if (cur >= 2) {
                dev_.write_block(cluster_to_block(cur), data.data() + blk_idx * BLOCK_SIZE);
            }
            return 0;
        }
    }

    // No empty slot — extend the directory by one cluster
    uint16_t last = dir_cluster;
    while (fat_[last] != FAT16_END_OF_CHAIN && fat_[last] >= 2) {
        last = fat_[last];
    }
    uint16_t new_c = alloc_cluster();
    if (new_c == FAT16_END_OF_CHAIN) return -1;
    fat_[last] = new_c;
    fat_[new_c] = FAT16_END_OF_CHAIN;
    flush_fat();

    std::vector<uint8_t> new_blk(BLOCK_SIZE, 0);
    std::memcpy(new_blk.data(), &entry, sizeof(FAT16DirEntry));
    dev_.write_block(cluster_to_block(new_c), new_blk.data());
    return 0;
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
            fat_[idx] = blk[j * 2] | (static_cast<uint16_t>(blk[j * 2 + 1]) << 8);
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

    // Special case for "." and ".."
    if (std::strcmp(name, ".") == 0) {
        e.name[0] = '.';
        return;
    }
    if (std::strcmp(name, "..") == 0) {
        e.name[0] = '.';
        e.name[1] = '.';
        return;
    }

    const char* dot = std::strrchr(name, '.');
    size_t base_len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);

    for (size_t i = 0; i < base_len && i < 8; ++i) {
        e.name[i] = (name[i] >= 'a' && name[i] <= 'z') ? static_cast<char>(name[i] - 32) : name[i];
    }

    if (dot && dot[1] != '\0') {
        const char* ext = dot + 1;
        for (size_t i = 0; i < 3 && ext[i] != '\0'; ++i) {
            e.ext[i] = (ext[i] >= 'a' && ext[i] <= 'z') ? static_cast<char>(ext[i] - 32) : ext[i];
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
    t = static_cast<uint16_t>((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    d = static_cast<uint16_t>(((tm->tm_year + 1900 - 1980) << 9) | ((tm->tm_mon + 1) << 5) |
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

// --- New helpers for file_size management ---

uint32_t Fat16Fs::get_file_size(uint16_t first_cluster) const {
    // Search all directories for the entry with this first_cluster
    // This is a brute-force approach; a real FS would cache this
    auto search_dir = [&](uint16_t dir_cluster) -> int32_t {
        std::vector<FAT16DirEntry> entries;
        read_dir_entries(dir_cluster, entries);
        for (auto& e : entries) {
            if (e.name[0] == 0 || e.name[0] == static_cast<char>(0xE5)) continue;
            if (e.first_cluster == first_cluster && !(e.attr & FAT16_ATTR_DIRECTORY)) {
                return static_cast<int32_t>(e.file_size);
            }
        }
        return -1;
    };

    // Check root
    int32_t sz = search_dir(kRootCluster);
    if (sz >= 0) return static_cast<uint32_t>(sz);

    // Check all allocated clusters that might be directories
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) continue;
        sz = search_dir(i);
        if (sz >= 0) return static_cast<uint32_t>(sz);
    }
    return 0;
}

void Fat16Fs::update_file_size(uint16_t first_cluster, uint32_t new_size) {
    auto update_in_dir = [&](uint16_t dir_cluster) -> bool {
        if (dir_cluster == kRootCluster) {
            std::vector<uint8_t> blk(BLOCK_SIZE);
            dev_.read_block(kRootDirBlk, blk.data());
            for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
                auto* e = reinterpret_cast<FAT16DirEntry*>(blk.data() + i * 32);
                if (e->first_cluster == first_cluster && !(e->attr & FAT16_ATTR_DIRECTORY)) {
                    e->file_size = new_size;
                    dev_.write_block(kRootDirBlk, blk.data());
                    return true;
                }
            }
            return false;
        }

        std::vector<uint8_t> data;
        read_cluster_chain(dir_cluster, data);
        for (size_t i = 0; i < data.size() / 32; ++i) {
            auto* e = reinterpret_cast<FAT16DirEntry*>(data.data() + i * 32);
            if (e->first_cluster == first_cluster && !(e->attr & FAT16_ATTR_DIRECTORY)) {
                e->file_size = new_size;
                uint16_t cur = dir_cluster;
                size_t blk_idx = (i * 32) / BLOCK_SIZE;
                for (size_t b = 0; b < blk_idx && cur != FAT16_END_OF_CHAIN; ++b) {
                    cur = fat_[cur];
                }
                if (cur >= 2) {
                    dev_.write_block(cluster_to_block(cur), data.data() + blk_idx * BLOCK_SIZE);
                }
                return true;
            }
        }
        return false;
    };

    if (update_in_dir(kRootCluster)) return;
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) continue;
        if (update_in_dir(i)) return;
    }
}

}  // namespace pfs
