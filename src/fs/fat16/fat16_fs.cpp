#include "fs/fat16/fat16_fs.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace pfs {

// 把字符串里的小写字母转大写。FAT16 的 8.3 文件名不区分大小写,
// 比对/落盘前统一转成大写。
static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return r;
}

// 把目录项里分开存的 8.3 名字拼回 "NAME.EXT" 形式:去掉空格填充,
// 没有扩展名就不带点。ls 显示和路径比对都用它还原文件名。
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

// 构造时只把状态置成"未挂载、cwd 在根",真正的盘上结构靠
// fs_format(新盘)或 fs_mount(旧盘)建立。
Fat16Fs::Fat16Fs(BlockDevice& dev)
    : dev_(dev),
      cwd_path_("/"),
      cwd_cluster_(kRootCluster),
      cur_uid_(0),
      cur_gid_(0),
      mounted_(false) {
}

// 切换当前用户(login/su 时调用)。FAT16 没有按用户的权限位,
// 这里只记下 uid/gid 用于 stat 显示,实际打开文件统一走 0 号槽。
void Fat16Fs::set_user(uint16_t uid, uint16_t gid) {
    cur_uid_ = uid;
    cur_gid_ = gid;
    user_slot_ = 0;  // 单会话:所有打开文件表操作都走 0 号槽
}

// 记下虚拟磁盘镜像文件的路径,sync 时往这个文件落盘。
void Fat16Fs::set_disk_path(const std::string& path) {
    disk_path_ = path;
}

// 落盘:把 FAT 表两份和引导扇区写回内存盘,再整盘 dump 到镜像文件。
// 每个会改盘的操作末尾都调一次,保证掉电/退出后能恢复。
void Fat16Fs::sync() {
    if (disk_path_.empty()) return;
    flush_fat();
    dev_.write_block(kBootBlk, &boot_);
    dev_.save_to_file(disk_path_.c_str());
}

// ======================== 生命周期 ========================

// 格式化:铺好引导扇区,把两张 FAT 整体清零,再把 0/1 号项填成
// 保留值(0xFFF8 媒体标志 + 0xFFFF),根目录区清零。簇 2 起才是可用数据簇。
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
    fat_[0] = 0xFFF8;  // 0 号项:媒体描述符,保留不用作数据簇
    fat_[1] = 0xFFFF;  // 1 号项:保留,数据簇从 2 号开始
    flush_fat();

    dev_.write_block(kRootDirBlk, zero_blk.data());

    cwd_path_ = "/";
    cwd_cluster_ = kRootCluster;
    mounted_ = true;
    sync();
    return 0;
}

// 挂载已有盘:读回引导扇区并做合法性校验(块大小、总块数),
// 再把磁盘上的 FAT 表读进内存副本,cwd 复位到根。
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

// 卸载:把内存 FAT 和引导扇区写回内存盘后清挂载标志。
// 注意这里不调 save_to_file,镜像文件的落盘由 sync 负责。
int Fat16Fs::fs_unmount() {
    if (!mounted_) return -1;
    flush_fat();
    dev_.write_block(kBootBlk, &boot_);
    mounted_ = false;
    return 0;
}

// ======================== 文件操作 ========================

// 建空文件:先确认父目录存在且同名不存在,再分配一个簇作链头(置 EOC)、
// 清零,然后往父目录追加一条 8.3 目录项。任一步失败要回收已分配的簇。
int Fat16Fs::fs_create(const char* path, uint16_t /*mode*/) {
    if (!mounted_) return -1;

    uint16_t parent_cluster, unused;
    std::string name;
    int ret = resolve_path(path, unused, parent_cluster, name);
    if (ret == 0) return -1;   // 同名已存在
    if (ret != -2) return -1;  // 父目录不存在

    uint16_t first_cluster = alloc_cluster();
    if (first_cluster == FAT16_END_OF_CHAIN) return -1;
    fat_[first_cluster] = FAT16_END_OF_CHAIN;
    flush_fat();

    // 清零新分配的簇
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

// 打开文件:解析路径找到目录项,在打开文件表登记 fd(以起始簇号当句柄)。
// 处理 O_TRUNC(清空内容)和 O_APPEND(偏移移到文件末尾)两种打开语义。
int Fat16Fs::fs_open(const char* path, int flags) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    // 找到目录项以取出 first_cluster
    std::vector<FAT16DirEntry> entries;
    read_dir_entries(parent, entries);
    for (auto& e : entries) {
        if (to_upper(name) == entry_name(e)) {
            // 拒绝把目录当文件打开:一旦写入会覆盖它的目录项块把目录写坏
            // (例如 cp/mv 到一个目录名、`open dir w`)。
            if (e.attr & FAT16_ATTR_DIRECTORY) return -1;
            // O_TRUNC(如 `open f w`):丢掉原有内容。保留第一个簇作为(现已
            // 清空的)链头,释放其余簇,大小清零。FAT16 没有 rwx 位,不做额外权限检查。
            if ((flags & O_TRUNC) && !(e.attr & FAT16_ATTR_DIRECTORY) && e.first_cluster >= 2) {
                uint16_t rest = fat_[e.first_cluster];
                fat_[e.first_cluster] = FAT16_END_OF_CHAIN;
                free_cluster_chain(rest);  // 释放剩余簇(已是 EOC 则空操作);顺带 flush FAT
                update_file_size(parent, e.first_cluster, 0);
                sync();
            }
            int fd = oft_.alloc_fd(user_slot_, e.first_cluster, static_cast<uint8_t>(flags));
            if (fd >= 0) {
                open_parent_[fd] = parent;  // 记住目录项在哪个父目录里
                if (flags & O_APPEND) oft_.set_offset(user_slot_, fd, e.file_size);
            }
            return fd;
        }
    }
    return -1;
}

// 关闭文件:从打开文件表释放 fd,成功则落盘一次。
int Fat16Fs::fs_close(int fd) {
    int ret = oft_.free_fd(user_slot_, fd);
    if (ret == 0) sync();
    return ret;
}

// 读文件:从当前偏移起,沿簇链(fat_[c] 即下一簇)逐块读出。
// 先按偏移跳过前面的整簇,再在簇内读到 file_size 为止,最后推进偏移。
ssize_t Fat16Fs::fs_read(int fd, void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(user_slot_, fd)) return -1;
    if (!(oft_.get_flags(user_slot_, fd) & O_READ)) return -1;

    uint16_t first_cluster = oft_.get_inode(user_slot_, fd);
    uint32_t offset = oft_.get_offset(user_slot_, fd);

    // 从目录项取文件大小(在该 fd 记下的父目录里查)
    uint32_t file_size = get_file_size(open_parent_[fd], first_cluster);
    if (offset >= file_size) return 0;
    size_t to_read = std::min(len, static_cast<size_t>(file_size - offset));

    // 沿簇链走到偏移所在的位置
    auto* dst = static_cast<uint8_t*>(buf);
    size_t bytes_read = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    uint16_t cur_cluster = first_cluster;
    uint32_t skip_blocks = offset / BLOCK_SIZE;

    // 跳到偏移对应的那个簇
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

    oft_.set_offset(user_slot_, fd, offset + bytes_read);
    return static_cast<ssize_t>(bytes_read);
}

// 写文件:从偏移处开始把数据逐块写入。簇链不够长时一路用 alloc_cluster
// 现场扩链(置 EOC 串上去),不足整块的部分先读后改再写,最后更新文件大小。
ssize_t Fat16Fs::fs_write(int fd, const void* buf, size_t len) {
    if (!mounted_) return -1;
    if (!oft_.is_valid(user_slot_, fd)) return -1;

    uint8_t flags = oft_.get_flags(user_slot_, fd);
    if (!(flags & O_WRITE) && !(flags & O_APPEND)) return -1;

    uint16_t first_cluster = oft_.get_inode(user_slot_, fd);
    uint32_t offset = oft_.get_offset(user_slot_, fd);

    auto* src = static_cast<const uint8_t*>(buf);
    size_t bytes_written = 0;
    uint8_t blk_buf[BLOCK_SIZE];

    // 沿簇链走到偏移所在的簇
    uint16_t cur_cluster = first_cluster;
    uint16_t prev_cluster = 0;
    uint32_t skip_blocks = offset / BLOCK_SIZE;

    for (uint32_t i = 0; i < skip_blocks; ++i) {
        prev_cluster = cur_cluster;
        uint16_t next = fat_[cur_cluster];
        if (next == FAT16_END_OF_CHAIN || next < 2) {
            // 链不够长,给空洞补一个新簇
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
            // 先读出整块原内容,以便支持非整块的部分覆写
            dev_.read_block(cluster_to_block(cur_cluster), blk_buf);

            uint32_t chunk =
                std::min(static_cast<size_t>(BLOCK_SIZE - blk_off), len - bytes_written);
            std::memcpy(blk_buf + blk_off, src + bytes_written, chunk);
            dev_.write_block(cluster_to_block(cur_cluster), blk_buf);
            bytes_written += chunk;
            blk_off = 0;

            if (bytes_written < len) {
                // 前进到下一簇,链到头就再分配一个挂上去
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

    // 把新的文件大小写回目录项
    uint32_t new_end = offset + bytes_written;
    uint32_t old_size = get_file_size(open_parent_[fd], first_cluster);
    if (new_end > old_size) {
        update_file_size(open_parent_[fd], first_cluster, new_end);
    }

    oft_.set_offset(user_slot_, fd, new_end);
    sync();
    return static_cast<ssize_t>(bytes_written);
}

// 删文件:先把它的整条簇链释放回空闲(FAT 项清 0),再把父目录里的
// 那条目录项标删。拒绝删目录(目录要走 rmdir)。
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

// 递归删除(rm -r):文件直接删,目录则先深度优先删光子项再 rmdir 自己。
// 跳过 . 和 ..,靠 fs_ls/fs_stat 复用上层逻辑。
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

// ======================== 目录操作 ========================

// 建目录:分配一个簇当目录的数据块,先写入 . 和 .. 两条目录项
// (. 指自己、.. 指父目录),再往父目录追加指向本目录的目录项。
int Fat16Fs::fs_mkdir(const char* path) {
    if (!mounted_) return -1;

    uint16_t unused, parent_cluster;
    std::string name;
    int ret = resolve_path(path, unused, parent_cluster, name);
    if (ret == 0) return -1;   // 同名已存在
    if (ret != -2) return -1;  // 父目录不存在

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

// 删空目录:先确认目录里除 . 和 .. 外没有任何有效项,否则拒绝;
// 然后释放目录占的簇链,再从父目录里抹掉它的目录项。
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
        if (en != "." && en != "..") return -1;  // 目录非空,不能删
    }

    free_cluster_chain(cluster);
    int ret = remove_dir_entry(parent, name.c_str());
    if (ret == 0) sync();
    return ret;
}

// 切换工作目录:更新 cwd_cluster_,并把 cwd_path_ 维护成规范的绝对路径。
// 相对路径要把当前路径拆段后再逐段应用 . / .. 归一化,绝对路径直接采用。
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

// 列目录:读出目标目录的全部目录项,跳过空槽(name[0]==0)和已删项
// (name[0]==0xE5),把每条 FAT16 目录项转成上层统一的 DirEntry 返回。
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
        if (e.name[0] == 0) continue;                       // 空槽
        if (e.name[0] == static_cast<char>(0xE5)) continue;  // 已删除项
        DirEntry ve;
        fat16_to_vfs_entry(e, ve);
        out.push_back(ve);
    }
    return 0;
}

// 返回当前工作目录的绝对路径(由 fs_chdir 实时维护好)。
std::string Fat16Fs::fs_pwd() {
    return cwd_path_;
}

// ======================== 元数据 ========================

// 取文件/目录的状态。根目录没有盘上目录项,单独合成一份 stat;
// 其余从父目录里找到对应目录项,把 attr 翻译成 mode/type。uid/gid 取当前用户。
int Fat16Fs::fs_stat(const char* path, FileStat& out) {
    if (!mounted_) return -1;

    uint16_t cluster, parent;
    std::string name;
    if (resolve_path(path, cluster, parent, name) != 0) return -1;

    // 末段为空表示路径本身就指向一个目录(例如 "/")。根目录在盘上没有
    // 对应目录项可读,所以这里直接合成它的 stat。
    if (name.empty()) {
        std::memset(&out, 0, sizeof(out));
        out.mode = MODE_DIR | DEFAULT_MODE;
        out.uid = cur_uid_;
        out.gid = cur_gid_;
        out.nlink = 1;
        out.type = TYPE_DIR;
        return 0;
    }

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

// FAT16 的目录项里没有 rwx 权限位,根本无从修改。这里故意返回 0(而非 -1),
// 让 shell 的 chmod 在两种引擎下行为一致——在 FAT16 上它只是个空操作,而不是报错。
int Fat16Fs::fs_chmod(const char* /*path*/, uint16_t /*mode*/) {
    return 0;
}

// 硬链接:FAT16 没有"多名指向同一文件"的概念,直接返回失败。
int Fat16Fs::fs_link(const char* /*src*/, const char* /*dst*/) {
    return -1;
}

// 符号链接是 UNIX 引擎特有功能,FAT16 不支持。
int Fat16Fs::fs_symlink(const char* /*target*/, const char* /*linkpath*/) {
    return -1;
}

// 读符号链接:FAT16 不支持软链接,恒返回失败。
int Fat16Fs::fs_readlink(const char* /*path*/, std::string& /*out*/) {
    return -1;
}

// 引擎名,给 TUI 标题栏和 shell 显示用。
std::string Fat16Fs::fs_type_name() const {
    return "FAT16";
}

// 统计磁盘使用量:元数据区(引导/FAT/根目录)恒计为已用,数据区
// 数已分配的簇(FAT 项非 0)。FAT16 不用 inode,故 inode 计数为 0。
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

// 生成磁盘块位图(给 TUI 的 F3 磁盘视图用):每块标成元数据/已用/空闲。
// 元数据区固定,数据区每个簇对应一块,用/闲看它的 FAT 项是否为 0。
void Fat16Fs::fs_block_map(std::vector<uint8_t>& out) {
    out.assign(TOTAL_BLK_NUM, BLK_FREE);
    // 引导扇区 + 两张 FAT + 根目录区都是固定的元数据
    for (uint32_t b = 0; b < kDataStart; ++b) out[b] = BLK_META;
    // 每个数据簇对应一块;该簇的 FAT 项决定它是已用还是空闲
    for (uint16_t c = 2; c < 2 + kDataBlkCount; ++c) {
        if (c >= fat_.size()) break;
        uint32_t blk = cluster_to_block(c);
        if (blk >= TOTAL_BLK_NUM) break;
        out[blk] = (fat_[c] == FAT16_FREE_CLUSTER) ? BLK_FREE : BLK_USED;
    }
}

// ======================== 内部辅助函数 ========================

// 簇号 → 磁盘块号的换算:数据区从 kDataStart(10)开始,簇号从 2 算起,
// 所以块号 = 10 + (簇号 - 2)。簇 0/1 是保留项,不对应任何数据块。
uint32_t Fat16Fs::cluster_to_block(uint16_t cluster) const {
    return kDataStart + (cluster - 2);
}

// 读一个 FAT 表项(即该簇的"下一簇指针"),越界返回失败。
int Fat16Fs::read_fat_entry(uint16_t cluster, uint16_t& out) const {
    if (cluster >= kClustersPerFat) return -1;
    out = fat_[cluster];
    return 0;
}

// 写一个 FAT 表项,改的是内存副本,落盘靠 flush_fat。
int Fat16Fs::write_fat_entry(uint16_t cluster, uint16_t value) {
    if (cluster >= kClustersPerFat) return -1;
    fat_[cluster] = value;
    return 0;
}

// 分配一个空闲簇:从簇 2 起线性扫描 FAT,找到第一个为 0 的项就占住并置 EOC。
// 这是 O(n) 扫描,和 UNIX 引擎成组链 O(1) 弹栈形成对比——FAT 简单但分配慢。
uint16_t Fat16Fs::alloc_cluster() {
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) {
            fat_[i] = FAT16_END_OF_CHAIN;
            return i;
        }
    }
    return FAT16_END_OF_CHAIN;
}

// 释放整条簇链:沿"下一簇指针"逐个把 FAT 项清 0 还回空闲。
// count < 2000 是防呆上限,避免链被写坏成环时死循环。
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

// 顺着簇链把文件内容读进一个连续缓冲区,最多读 max_bytes 字节。
// 同样用 count < 2000 防止坏链死循环。
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

// 按数据长度一次性分配够用的簇并串成一条新链,再把数据逐块写下去。
// 中途分配失败要回滚已占的簇;返回链头簇号给调用方。
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

// 路径解析(FAT16 版的 namei):从根或 cwd 出发逐段下钻,带回目标簇号、
// 父目录簇号和末段名。返回 0=找到,-2=父目录在但目标不存在(create/mkdir 靠它判空),
// -1=路径中间某级不存在。
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

// 读出一个目录里的全部目录项。根目录(kRootCluster)在固定的根目录区
// 单独一块里;子目录则是普通簇链,沿链读出后按 32 字节切成目录项。
// 读到 name[0]==0 的空槽即停(它后面都是没用过的项)。
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

// 往目录里写一条目录项,实现就是追加,转交 append_dir_entry。
int Fat16Fs::write_dir_entry(uint16_t dir_cluster, const FAT16DirEntry& entry) {
    return append_dir_entry(dir_cluster, entry);
}

// 从目录里删一条目录项:并不真清空,而是按 DOS 习惯把名字首字节标成
// 0xE5(表示"已删除,可复用"),再把改动的那一块写回。根目录走固定块。
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
            // 把改动写回簇链中对应的那一块
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

// 往目录追加一条目录项:优先复用空槽(name[0]==0 或 0xE5),没有空槽
// 就给目录扩一个新簇挂到链尾再写入。根目录是固定一块、容量有限,满了就失败。
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

    // 子目录:沿簇链扫描找一个空槽
    std::vector<uint8_t> data;
    read_cluster_chain(dir_cluster, data);

    for (size_t i = 0; i < data.size() / 32; ++i) {
        auto* e = reinterpret_cast<FAT16DirEntry*>(data.data() + i * 32);
        if (e->name[0] == 0 || e->name[0] == static_cast<char>(0xE5)) {
            std::memcpy(e, &entry, sizeof(FAT16DirEntry));
            // 只把改动的那一块写回
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

    // 没有空槽——给目录扩一个簇接到链尾
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

// 找第一个空闲簇但不占用(只看不改),线性扫描 FAT。
uint16_t Fat16Fs::find_free_cluster() const {
    for (uint16_t i = 2; i < 2 + kDataBlkCount; ++i) {
        if (fat_[i] == FAT16_FREE_CLUSTER) return i;
    }
    return FAT16_END_OF_CHAIN;
}

// 挂载时把磁盘上的 FAT1 读进内存副本 fat_:每项 2 字节,小端序拼回 uint16。
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

// 把内存 FAT 副本写回磁盘,同一份内容同时写 FAT1 和 FAT2——
// FAT2 就是 FAT1 的完整备份,这是 FAT 文件系统的容错设计。
void Fat16Fs::flush_fat() {
    for (uint32_t i = 0; i < kFatSize; ++i) {
        std::vector<uint8_t> blk(BLOCK_SIZE, 0);
        for (uint32_t j = 0; j < kEntriesPerFatBlk; ++j) {
            uint32_t idx = i * kEntriesPerFatBlk + j;
            if (idx >= kClustersPerFat) break;
            blk[j * 2] = fat_[idx] & 0xFF;             // 低字节在前(小端序)
            blk[j * 2 + 1] = (fat_[idx] >> 8) & 0xFF;  // 高字节在后
        }
        dev_.write_block(kFat1Start + i, blk.data());  // 写 FAT1
        dev_.write_block(kFat2Start + i, blk.data());  // 同步写 FAT2(备份)
    }
}

// 把普通文件名填进目录项的 8.3 字段:基名截到 8 字符填 name、扩展名截到
// 3 字符填 ext,不足部分以空格补齐,小写一律转大写。. 和 .. 单独处理。
void Fat16Fs::set_dir_entry_name(FAT16DirEntry& e, const char* name) {
    std::memset(e.name, ' ', 8);
    std::memset(e.ext, ' ', 3);

    // . 和 .. 两个特殊目录项
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

// 把 FAT16 目录项翻译成 VFS 通用的 DirEntry:还原 8.3 文件名、用起始簇号
// 充当 inode 号、由属性位判定文件/目录类型,供上层 ls/tree 统一处理。
void Fat16Fs::fat16_to_vfs_entry(const FAT16DirEntry& fe, DirEntry& ve) const {
    std::memset(&ve, 0, sizeof(ve));
    std::string n = entry_name(fe);
    std::strncpy(ve.name, n.c_str(), MAX_FILENAME - 1);
    ve.name[MAX_FILENAME - 1] = '\0';
    ve.inode_no = fe.first_cluster;
    ve.type = (fe.attr & FAT16_ATTR_DIRECTORY) ? TYPE_DIR : TYPE_FILE;
    ve.size = fe.file_size;
}

// 生成 FAT 格式的时间/日期戳:时间按 时<<11|分<<5|秒/2 打包(秒只有 2 秒精度),
// 日期按 (年-1980)<<9|月<<5|日 打包(FAT 纪元是 1980 年)。
void Fat16Fs::timestamp(uint16_t& t, uint16_t& d) const {
    std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    t = static_cast<uint16_t>((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    d = static_cast<uint16_t>(((tm->tm_year + 1900 - 1980) << 9) | ((tm->tm_mon + 1) << 5) |
                              tm->tm_mday);
}

// 数一条簇链有多少个簇(即文件占了几块),防呆上限 2000。
uint32_t Fat16Fs::count_clusters(uint16_t start) const {
    uint32_t count = 0;
    uint16_t cur = start;
    while (cur >= 2 && cur != FAT16_END_OF_CHAIN && count < 2000) {
        ++count;
        cur = fat_[cur];
    }
    return count;
}

// --- 文件大小管理的辅助函数 ---

// 从目录项读出文件大小。只在文件自己的父目录里找(传入的 dir_cluster),
// 不去扫遍所有簇——否则文件数据可能被误当成"恰好匹配的目录项"。
// 见测试 FileSizeNotConfusedByDataAliasingDirEntry。
uint32_t Fat16Fs::get_file_size(uint16_t dir_cluster, uint16_t first_cluster) const {
    std::vector<FAT16DirEntry> entries;
    read_dir_entries(dir_cluster, entries);
    for (auto& e : entries) {
        if (e.name[0] == 0 || e.name[0] == static_cast<char>(0xE5)) continue;
        if (e.first_cluster == first_cluster && !(e.attr & FAT16_ATTR_DIRECTORY)) {
            return e.file_size;
        }
    }
    return 0;
}

// 写文件时把新大小回填到目录项里。靠 first_cluster 在父目录中定位那条
// 文件项,改 file_size 后只把所在的那一块写回盘。根目录走固定块。
void Fat16Fs::update_file_size(uint16_t dir_cluster, uint16_t first_cluster, uint32_t new_size) {
    if (dir_cluster == kRootCluster) {
        std::vector<uint8_t> blk(BLOCK_SIZE);
        dev_.read_block(kRootDirBlk, blk.data());
        for (uint32_t i = 0; i < kDirEntriesPerBlk; ++i) {
            auto* e = reinterpret_cast<FAT16DirEntry*>(blk.data() + i * 32);
            if (e->first_cluster == first_cluster && !(e->attr & FAT16_ATTR_DIRECTORY)) {
                e->file_size = new_size;
                dev_.write_block(kRootDirBlk, blk.data());
                return;
            }
        }
        return;
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
            return;
        }
    }
}

}  // namespace pfs
