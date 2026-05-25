#include "fs/unix/unix_fs.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace pfs {

UnixFs::UnixFs(BlockDevice& dev)
    : dev_(dev),
      sb_(dev),
      imng_(dev, sb_),
      dmng_(dev, imng_),
      root_ino_(0),
      cwd_ino_(0),
      cwd_path_("/"),
      cur_uid_(0),
      cur_gid_(0),
      mounted_(false) {}

// ---- Lifecycle ----

int UnixFs::fs_format() {
    dev_.zero_all();

    // Reserve inode 0 (unused sentinel) and inodes 1 (root dir).
    // Reserve data blocks 0..2 (root dir, etc dir, password file — matching PPT).
    sb_.format(3, 2);

    // Create root directory at inode 1
    MemINode* root_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    if (root_ip == nullptr) {
        return -1;
    }
    root_ino_ = root_ip->i_ino;
    dmng_.init_dir(root_ip, root_ino_, root_ino_);
    imng_.write_back(root_ip);

    // Create /etc directory
    MemINode* etc_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    if (etc_ip != nullptr) {
        dmng_.init_dir(etc_ip, etc_ip->i_ino, root_ino_);
        dmng_.link(root_ip, "etc", etc_ip->i_ino);
        imng_.write_back(etc_ip);
        imng_.put(etc_ip);
    }

    // Create /home directory
    MemINode* home_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    if (home_ip != nullptr) {
        dmng_.init_dir(home_ip, home_ip->i_ino, root_ino_);
        dmng_.link(root_ip, "home", home_ip->i_ino);
        imng_.write_back(home_ip);
        imng_.put(home_ip);
    }

    imng_.write_back(root_ip);
    imng_.put(root_ip);

    sb_.set_root_inode(root_ino_);
    sb_.flush();
    cwd_ino_ = root_ino_;
    cwd_path_ = "/";
    mounted_ = true;
    return 0;
}

int UnixFs::fs_mount() {
    sb_.load();
    root_ino_ = sb_.root_inode();
    cwd_ino_ = root_ino_;
    cwd_path_ = "/";
    mounted_ = true;
    return 0;
}

int UnixFs::fs_unmount() {
    if (!mounted_) {
        return -1;
    }
    sb_.flush();
    mounted_ = false;
    return 0;
}

// ---- File operations ----

int UnixFs::fs_create(const char* path, uint16_t mode) {
    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(path, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        return -1;
    }

    // Check if name already exists
    if (dmng_.lookup(parent_ip, basename.c_str()) != INVALID_BLK) {
        imng_.put(parent_ip);
        return -1;
    }

    MemINode* new_ip = imng_.alloc(MODE_FILE | (mode & 0x01FF), cur_uid_, cur_gid_);
    if (new_ip == nullptr) {
        imng_.put(parent_ip);
        return -1;
    }

    if (dmng_.link(parent_ip, basename.c_str(), new_ip->i_ino) != 0) {
        new_ip->di.di_nlink = 0;
        imng_.put(new_ip);
        imng_.put(parent_ip);
        return -1;
    }

    imng_.write_back(new_ip);
    imng_.write_back(parent_ip);
    imng_.put(new_ip);
    imng_.put(parent_ip);
    return 0;
}

int UnixFs::fs_open(const char* path, int flags) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }

    int fd = oft_.alloc_fd(cur_uid_, ino, static_cast<uint8_t>(flags));
    if (fd < 0) {
        return -1;
    }

    if (flags & O_APPEND) {
        MemINode* ip = imng_.get(ino);
        if (ip != nullptr) {
            oft_.set_offset(cur_uid_, fd, ip->di.di_size);
            imng_.put(ip);
        }
    }

    return fd;
}

int UnixFs::fs_close(int fd) {
    return oft_.free_fd(cur_uid_, fd);
}

ssize_t UnixFs::fs_read(int fd, void* buf, size_t len) {
    if (!oft_.is_valid(cur_uid_, fd)) {
        return -1;
    }
    if (!(oft_.get_flags(cur_uid_, fd) & O_READ)) {
        return -1;
    }

    uint16_t ino = oft_.get_inode(cur_uid_, fd);
    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    uint32_t offset = oft_.get_offset(cur_uid_, fd);
    uint32_t file_size = ip->di.di_size;

    if (offset >= file_size) {
        imng_.put(ip);
        return 0;
    }
    if (offset + len > file_size) {
        len = file_size - offset;
    }

    auto* dst = static_cast<uint8_t*>(buf);
    size_t bytes_read = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    while (bytes_read < len) {
        uint32_t blk_idx = (offset + bytes_read) / BLOCK_SIZE;
        uint32_t blk_off = (offset + bytes_read) % BLOCK_SIZE;
        uint32_t chunk = std::min(static_cast<size_t>(BLOCK_SIZE - blk_off), len - bytes_read);

        uint16_t phys = imng_.bmap(ip, blk_idx);
        if (phys == 0 || phys == INVALID_BLK) {
            std::memset(dst + bytes_read, 0, chunk);
        } else {
            dev_.read_block(DATA_START_BLK + phys, blk_buf);
            std::memcpy(dst + bytes_read, blk_buf + blk_off, chunk);
        }
        bytes_read += chunk;
    }

    ip->di.di_atime = static_cast<uint32_t>(std::time(nullptr));
    ip->i_dirty = true;
    imng_.put(ip);

    oft_.set_offset(cur_uid_, fd, offset + bytes_read);
    return static_cast<ssize_t>(bytes_read);
}

ssize_t UnixFs::fs_write(int fd, const void* buf, size_t len) {
    if (!oft_.is_valid(cur_uid_, fd)) {
        return -1;
    }
    uint8_t flags = oft_.get_flags(cur_uid_, fd);
    if (!(flags & O_WRITE) && !(flags & O_APPEND)) {
        return -1;
    }

    uint16_t ino = oft_.get_inode(cur_uid_, fd);
    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    uint32_t offset = oft_.get_offset(cur_uid_, fd);
    auto* src = static_cast<const uint8_t*>(buf);
    size_t bytes_written = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    while (bytes_written < len) {
        uint32_t blk_idx = (offset + bytes_written) / BLOCK_SIZE;
        uint32_t blk_off = (offset + bytes_written) % BLOCK_SIZE;
        uint32_t chunk = std::min(static_cast<size_t>(BLOCK_SIZE - blk_off), len - bytes_written);

        uint16_t phys = imng_.bmap_alloc(ip, blk_idx);
        if (phys == INVALID_BLK) {
            break;
        }

        if (chunk < BLOCK_SIZE) {
            dev_.read_block(DATA_START_BLK + phys, blk_buf);
        }
        std::memcpy(blk_buf + blk_off, src + bytes_written, chunk);
        dev_.write_block(DATA_START_BLK + phys, blk_buf);
        bytes_written += chunk;
    }

    uint32_t new_end = offset + bytes_written;
    if (new_end > ip->di.di_size) {
        ip->di.di_size = new_end;
    }
    ip->di.di_mtime = static_cast<uint32_t>(std::time(nullptr));
    ip->i_dirty = true;
    imng_.write_back(ip);
    imng_.put(ip);

    oft_.set_offset(cur_uid_, fd, new_end);
    return static_cast<ssize_t>(bytes_written);
}

int UnixFs::fs_delete(const char* path) {
    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(path, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        return -1;
    }

    uint16_t ino = dmng_.lookup(parent_ip, basename.c_str());
    if (ino == INVALID_BLK) {
        imng_.put(parent_ip);
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        imng_.put(parent_ip);
        return -1;
    }

    if (ip->di.di_mode & MODE_DIR) {
        imng_.put(ip);
        imng_.put(parent_ip);
        return -1;
    }

    dmng_.unlink(parent_ip, basename.c_str());
    ip->di.di_nlink--;
    ip->i_dirty = true;
    imng_.write_back(parent_ip);
    imng_.put(parent_ip);
    imng_.put(ip);
    return 0;
}

// ---- Directory operations ----

int UnixFs::fs_mkdir(const char* path) {
    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(path, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        return -1;
    }

    if (dmng_.lookup(parent_ip, basename.c_str()) != INVALID_BLK) {
        imng_.put(parent_ip);
        return -1;
    }

    MemINode* new_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, cur_uid_, cur_gid_);
    if (new_ip == nullptr) {
        imng_.put(parent_ip);
        return -1;
    }

    dmng_.init_dir(new_ip, new_ip->i_ino, parent_ino);
    dmng_.link(parent_ip, basename.c_str(), new_ip->i_ino);

    imng_.write_back(new_ip);
    imng_.write_back(parent_ip);
    imng_.put(new_ip);
    imng_.put(parent_ip);
    return 0;
}

int UnixFs::fs_rmdir(const char* path) {
    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(path, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        return -1;
    }

    uint16_t ino = dmng_.lookup(parent_ip, basename.c_str());
    if (ino == INVALID_BLK) {
        imng_.put(parent_ip);
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr || !(ip->di.di_mode & MODE_DIR)) {
        if (ip) imng_.put(ip);
        imng_.put(parent_ip);
        return -1;
    }

    if (!dmng_.is_empty(ip)) {
        imng_.put(ip);
        imng_.put(parent_ip);
        return -1;
    }

    dmng_.unlink(parent_ip, basename.c_str());
    ip->di.di_nlink = 0;
    ip->i_dirty = true;
    imng_.write_back(parent_ip);
    imng_.put(parent_ip);
    imng_.put(ip);
    return 0;
}

int UnixFs::fs_chdir(const char* path) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr || !(ip->di.di_mode & MODE_DIR)) {
        if (ip) imng_.put(ip);
        return -1;
    }
    imng_.put(ip);

    cwd_ino_ = ino;

    // Update path string
    if (path[0] == '/') {
        cwd_path_ = std::string(path);
    } else {
        std::string p(path);
        // Handle ".." and "." in path
        auto parts = std::vector<std::string>();
        // Parse current cwd_path_
        size_t start = 0;
        std::string base = cwd_path_;
        while (start < base.size()) {
            if (base[start] == '/') { start++; continue; }
            size_t end = base.find('/', start);
            if (end == std::string::npos) end = base.size();
            parts.push_back(base.substr(start, end - start));
            start = end + 1;
        }
        // Apply relative path components
        start = 0;
        while (start < p.size()) {
            if (p[start] == '/') { start++; continue; }
            size_t end = p.find('/', start);
            if (end == std::string::npos) end = p.size();
            std::string comp = p.substr(start, end - start);
            if (comp == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (comp != ".") {
                parts.push_back(comp);
            }
            start = end + 1;
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

int UnixFs::fs_ls(const char* path, std::vector<DirEntry>& out) {
    uint16_t ino = (path == nullptr || path[0] == '\0')
                       ? cwd_ino_
                       : dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr || !(ip->di.di_mode & MODE_DIR)) {
        if (ip) imng_.put(ip);
        return -1;
    }

    std::vector<DiskDirEntry> raw;
    if (dmng_.read_entries(ip, raw) != 0) {
        imng_.put(ip);
        return -1;
    }
    imng_.put(ip);

    out.clear();
    for (auto& e : raw) {
        if (e.d_ino == 0) continue;
        DirEntry de{};
        std::strncpy(de.name, e.d_name, MAX_FILENAME);
        de.inode_no = e.d_ino;

        MemINode* eip = imng_.get(e.d_ino);
        if (eip != nullptr) {
            de.type = (eip->di.di_mode & MODE_DIR) ? TYPE_DIR : TYPE_FILE;
            de.size = eip->di.di_size;
            imng_.put(eip);
        }
        out.push_back(de);
    }
    return 0;
}

std::string UnixFs::fs_pwd() {
    return cwd_path_;
}

// ---- Metadata ----

int UnixFs::fs_stat(const char* path, FileStat& out) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    out.mode = ip->di.di_mode;
    out.uid = ip->di.di_uid;
    out.gid = ip->di.di_gid;
    out.size = ip->di.di_size;
    out.atime = ip->di.di_atime;
    out.mtime = ip->di.di_mtime;
    out.ctime = ip->di.di_ctime;
    out.nlink = ip->di.di_nlink;
    out.type = (ip->di.di_mode & MODE_DIR) ? TYPE_DIR : TYPE_FILE;
    imng_.put(ip);
    return 0;
}

int UnixFs::fs_chmod(const char* path, uint16_t mode) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    ip->di.di_mode = (ip->di.di_mode & 0xFF00) | (mode & 0x01FF);
    ip->i_dirty = true;
    imng_.write_back(ip);
    imng_.put(ip);
    return 0;
}

int UnixFs::fs_link(const char* src, const char* dst) {
    uint16_t src_ino = dmng_.namei(src, cwd_ino_, root_ino_);
    if (src_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* src_ip = imng_.get(src_ino);
    if (src_ip == nullptr || (src_ip->di.di_mode & MODE_DIR)) {
        if (src_ip) imng_.put(src_ip);
        return -1;
    }

    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(dst, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        imng_.put(src_ip);
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        imng_.put(src_ip);
        return -1;
    }

    if (dmng_.link(parent_ip, basename.c_str(), src_ino) != 0) {
        imng_.put(parent_ip);
        imng_.put(src_ip);
        return -1;
    }

    src_ip->di.di_nlink++;
    src_ip->i_dirty = true;
    imng_.write_back(src_ip);
    imng_.write_back(parent_ip);
    imng_.put(parent_ip);
    imng_.put(src_ip);
    return 0;
}

// ---- System info ----

std::string UnixFs::fs_type_name() const {
    return "UNIX";
}

DiskUsage UnixFs::fs_disk_usage() const {
    DiskUsage du{};
    du.total_blocks = sb_.total_blocks();
    du.used_blocks = du.total_blocks - sb_.free_block_count();
    du.total_inodes = sb_.total_inodes();
    du.used_inodes = du.total_inodes - sb_.free_inode_count();
    return du;
}

void UnixFs::set_user(uint16_t uid, uint16_t gid) {
    cur_uid_ = uid;
    cur_gid_ = gid;
}

}  // namespace pfs
