#include "fs/unix/unix_fs.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace pfs {

// 构造时按依赖顺序把下层模块串起来:超级块吃块设备,inode 管理器吃超级块,
// 目录管理器吃 inode 管理器。初值是"未挂载、当前在根、当前用户 root"。
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
      mounted_(false),
      user_slot_(0) {
}

// ---- 生命周期 ----

// format:把裸盘初始化成一个可挂载的文件卷,必答题 Q3 的全过程都在这。
// 顺序:清零整盘 → 超级块建成组空闲块链 + 空闲 inode 栈 → 建根目录(写 . 和 ..)
// → 预置 /etc /home 两个目录 → 回填根 inode 号 → 落盘。
int UnixFs::fs_format() {
    dev_.zero_all();

    // 只保留 inode 0(空哨兵,d_ino==0 表示目录空槽,不能分给真实文件)。
    // 保留数据块 0..2(根目录、etc 目录、密码文件,对齐 PPT)。
    sb_.format(3, 1);

    // 建根目录。ialloc 升序发号,format 后第一个空闲 inode 就是 1 号,
    // 所以根目录固定落在 inode 1(对齐 UNIX 惯例 / 课件)。
    MemINode* root_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    if (root_ip == nullptr) {
        return -1;
    }
    root_ino_ = root_ip->i_ino;
    dmng_.init_dir(root_ip, root_ino_, root_ino_);
    imng_.write_back(root_ip);

    // 建 /etc 目录
    MemINode* etc_ip = imng_.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    if (etc_ip != nullptr) {
        dmng_.init_dir(etc_ip, etc_ip->i_ino, root_ino_);
        dmng_.link(root_ip, "etc", etc_ip->i_ino);
        imng_.write_back(etc_ip);
        imng_.put(etc_ip);
    }

    // 建 /home 目录
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
    sync();
    return 0;
}

// mount:挂载已格式化的旧盘。从盘上读回超级块,取出根 inode 号,
// 当前目录复位到根,空闲链/inode 栈随超级块一并恢复。
int UnixFs::fs_mount() {
    sb_.load();
    root_ino_ = sb_.root_inode();
    cwd_ino_ = root_ino_;
    cwd_path_ = "/";
    mounted_ = true;
    return 0;
}

// unmount:卸载前把超级块刷回盘,保证空闲链/inode 栈的最新状态不丢。
int UnixFs::fs_unmount() {
    if (!mounted_) {
        return -1;
    }
    sb_.flush();
    mounted_ = false;
    return 0;
}

// ---- 文件操作 ----

// create:在父目录下建一个新空文件。先 namei_parent 定位父目录、查父目录写权限、
// 查重名,再 alloc 新 inode、link 进父目录。任一步失败都把已分配的资源回滚干净。
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

    if (!check_access(parent_ip, O_WRITE)) {
        imng_.put(parent_ip);
        return -1;
    }

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
    sync();
    return 0;
}

// open:把路径变成 fd。流程是 namei → 权限校验 →(按需)截断 → 在打开文件表占一项。
// 返回的 fd 是后续 read/write 的句柄,文件位置/模式都记在打开文件表里。
int UnixFs::fs_open(const char* path, int flags) {
    // 跟随末端软链接,让 "cat link" / "cp link ..." 作用到目标文件
    //(下面的权限检查也就用目标的 mode)。
    uint16_t ino = namei_follow(path);
    if (ino == INVALID_BLK) {
        return -1;
    }

    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }
    // 目录不是普通文件,拒绝 open。否则一次写(如 `cp f dir`、`mv f dir`、
    // `open dir w`)会把目录的目录项块覆盖成文件字节而损坏目录——之后
    // ls/find 读到乱码目录项就可能崩。ls/cd 走的是 fs_ls/fs_chdir 而非
    // fs_open,所以这里不存在合法地打开目录的需求。
    if (ip->di.di_mode & MODE_DIR) {
        imng_.put(ip);
        return -1;
    }
    if (!check_access(ip, static_cast<uint8_t>(flags))) {
        imng_.put(ip);
        return -1;
    }
    // O_TRUNC(如 `open f w`):清空文件内容,免得新写的内容后面拖着旧尾巴。
    // 只截断普通文件,绝不动目录或软链接。截断需要的写权限上面 check_access
    // 已经覆盖(O_TRUNC 总和 O_WRITE 一起出现)。
    if ((flags & O_TRUNC) && (ip->di.di_mode & MODE_FILE)) {
        imng_.truncate(ip);  // 释放全部数据块+间址块,大小置 0
        ip->di.di_mtime = static_cast<uint32_t>(std::time(nullptr));
        ip->i_dirty = true;
        imng_.write_back(ip);
    }
    imng_.put(ip);

    int fd = oft_.alloc_fd(user_slot_, ino, static_cast<uint8_t>(flags));
    if (fd < 0) {
        return -1;
    }

    if (flags & O_APPEND) {
        MemINode* ip = imng_.get(ino);
        if (ip != nullptr) {
            oft_.set_offset(user_slot_, fd, ip->di.di_size);
            imng_.put(ip);
        }
    }

    return fd;
}

// close:从打开文件表里释放这个 fd;成功后顺手落一次盘。
int UnixFs::fs_close(int fd) {
    int ret = oft_.free_fd(user_slot_, fd);
    if (ret == 0) {
        sync();
    }
    return ret;
}

// read:读写主链路 fd → 打开文件表 → inode → bmap → 块设备 的读方向。
// 按当前偏移逐块读,bmap 返回 0 的"洞"块直接补零,末尾按文件大小截断。
ssize_t UnixFs::fs_read(int fd, void* buf, size_t len) {
    if (!oft_.is_valid(user_slot_, fd)) {
        return -1;
    }
    if (!(oft_.get_flags(user_slot_, fd) & O_READ)) {
        return -1;
    }

    uint16_t ino = oft_.get_inode(user_slot_, fd);
    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    uint32_t offset = oft_.get_offset(user_slot_, fd);
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

    oft_.set_offset(user_slot_, fd, offset + bytes_read);
    return static_cast<ssize_t>(bytes_read);
}

// write:读写主链路的写方向。逐块走 bmap_alloc——缺块就现场分配,
// 不满整块时先读回再改(读改写),最后按需扩大文件大小。盘满则提前收尾。
ssize_t UnixFs::fs_write(int fd, const void* buf, size_t len) {
    if (!oft_.is_valid(user_slot_, fd)) {
        return -1;
    }
    uint8_t flags = oft_.get_flags(user_slot_, fd);
    if (!(flags & O_WRITE) && !(flags & O_APPEND)) {
        return -1;
    }

    uint16_t ino = oft_.get_inode(user_slot_, fd);
    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }

    uint32_t offset = oft_.get_offset(user_slot_, fd);
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

    oft_.set_offset(user_slot_, fd, new_end);
    return static_cast<ssize_t>(bytes_written);
}

// delete:删一个普通文件(rm)。注意它只做两件事——从父目录 unlink + nlink--。
// 真正回收数据块和 inode 不在这,而是等最后一个引用 put 到 nlink==0 时由 iput 完成。
// 这样 rm 一个还开着的文件也不会出错。
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

    if (!check_access(parent_ip, O_WRITE)) {
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
    sync();
    return 0;
}

// delete_recursive:rm -r。后序删除——先递归清空所有子项,再 rmdir 自己。
// 软链接当叶子处理,只删链接本身、不跟随,目标原封不动。
int UnixFs::fs_delete_recursive(const char* path) {
    FileStat st{};
    if (fs_stat(path, st) != 0) {
        return -1;
    }

    if (st.type != TYPE_DIR) {
        // 文件和软链接都是叶子,直接 unlink(软链接只删自己、从不跟随,
        // 所以它的目标不受影响)。
        return fs_delete(path);
    }

    // 是目录——先把所有子项删掉
    std::vector<DirEntry> entries;
    if (fs_ls(path, entries) != 0) {
        return -1;
    }

    for (auto& e : entries) {
        if (std::strncmp(e.name, ".", MAX_FILENAME) == 0 ||
            std::strncmp(e.name, "..", MAX_FILENAME) == 0) {
            continue;
        }
        std::string child_path = std::string(path);
        if (child_path.back() != '/') child_path += "/";
        child_path += e.name;
        if (fs_delete_recursive(child_path.c_str()) != 0) {
            return -1;
        }
    }

    return fs_rmdir(path);
}

// ---- 目录操作 ----

// mkdir:建子目录。和 create 类似,但额外用 init_dir 写好 . 和 ..,
// 然后把新目录 link 进父目录。
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

    if (!check_access(parent_ip, O_WRITE)) {
        imng_.put(parent_ip);
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

    // 注意:目录的链接计数有意保持为 1,没有用经典的"2 + 子目录数"
    //(目录自己的 "." 加上每个子目录的 "..")。这纯属显示问题——`ll` 里目录
    // 显示为 1——rmdir 也不依赖它(rmdir 查的是是否为空,并强制把 nlink 归零)。
    // 想维护它就得在这里给本 inode 和父 inode 都 ++,并在 fs_rmdir 里对应 --。

    imng_.write_back(new_ip);
    imng_.write_back(parent_ip);
    imng_.put(new_ip);
    imng_.put(parent_ip);
    sync();
    return 0;
}

// rmdir:删空目录。必须先确认目录为空(只剩 . 和 ..)才动手。
// 同样只 unlink + 把 nlink 归零,块的回收交给 iput。
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

    if (!check_access(parent_ip, O_WRITE)) {
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
    sync();
    return 0;
}

// chdir:切换当前目录。两部分活——cwd_ino_ 跟着目标走(真正用于后续相对路径解析),
// cwd_path_ 维护给 pwd 看的路径字符串(自己处理 . 和 .. 的归一)。
int UnixFs::fs_chdir(const char* path) {
    // 跟随末端软链接,让 "cd link_to_dir" 进入目标目录。下面的路径字符串
    // 保留为(逻辑上的)入参;cwd_ino_ 跟踪真实目标,后续相对查找用的是它。
    uint16_t ino = namei_follow(path);
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

    // 更新路径字符串
    if (path[0] == '/') {
        cwd_path_ = std::string(path);
        // 归一末尾斜杠(如 `cd /home/` 或 `cd ~` -> "/home/"),让 pwd 显示
        // "/home" 而不是 "/home/"。根目录保持为 "/"。
        while (cwd_path_.size() > 1 && cwd_path_.back() == '/') cwd_path_.pop_back();
    } else {
        std::string p(path);
        // 处理路径里的 ".." 和 "."
        auto parts = std::vector<std::string>();
        // 先解析当前 cwd_path_
        size_t start = 0;
        std::string base = cwd_path_;
        while (start < base.size()) {
            if (base[start] == '/') {
                start++;
                continue;
            }
            size_t end = base.find('/', start);
            if (end == std::string::npos) end = base.size();
            parts.push_back(base.substr(start, end - start));
            start = end + 1;
        }
        // 再把相对路径的各段叠加上去
        start = 0;
        while (start < p.size()) {
            if (p[start] == '/') {
                start++;
                continue;
            }
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

// ls:列目录。读出原始目录项后,逐项 get 对应 inode 补上类型/大小,
// 跳过 d_ino==0 的空槽。不带 path 时列当前目录。
int UnixFs::fs_ls(const char* path, std::vector<DirEntry>& out) {
    uint16_t ino =
        (path == nullptr || path[0] == '\0') ? cwd_ino_ : dmng_.namei(path, cwd_ino_, root_ino_);
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
            if (eip->di.di_mode & MODE_DIR)
                de.type = TYPE_DIR;
            else if (eip->di.di_mode & MODE_SYMLINK)
                de.type = TYPE_SYMLINK;
            else
                de.type = TYPE_FILE;
            de.size = eip->di.di_size;
            imng_.put(eip);
        }
        out.push_back(de);
    }
    return 0;
}

// pwd:直接返回 chdir 一路维护好的当前路径字符串。
std::string UnixFs::fs_pwd() {
    return cwd_path_;
}

// ---- 元数据 ----

// stat:把 inode 的元信息(mode/uid/gid/大小/三个时间戳/nlink/类型)抄进 FileStat。
// 走的是 lstat 语义——报告链接本身而非目标,这样 ls/ll/stat 能把它认作软链接。
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
    // lstat 语义:报告链接自身而非目标,这样 ls/ll/stat 能显示成软链接。
    if (ip->di.di_mode & MODE_DIR)
        out.type = TYPE_DIR;
    else if (ip->di.di_mode & MODE_SYMLINK)
        out.type = TYPE_SYMLINK;
    else
        out.type = TYPE_FILE;
    imng_.put(ip);
    return 0;
}

// chmod:改权限位。只动低 9 位 rwx(mode & 0x01FF),高位的文件类型(目录/软链接等)
// 原样保留,所以 chmod 不会把目录改成普通文件。
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
    sync();
    return 0;
}

// link:硬链接(ln)。在 dst 父目录里加一条新目录项指向 src 同一个 inode,
// 并把 src 的 nlink++——硬链接的本质就是"多一个名字指向同一 inode"。
// 不允许给目录建硬链接(防成环)。
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
    sync();
    return 0;
}

// symlink:软链接(ln -s)。和硬链接不同,它是一个独立的新 inode,
// 内容就是目标路径字符串(存进它自己的第一个数据块)。
int UnixFs::fs_symlink(const char* target, const char* linkpath) {
    if (target == nullptr || *target == '\0') {
        return -1;
    }
    std::string basename;
    uint16_t parent_ino = dmng_.namei_parent(linkpath, cwd_ino_, root_ino_, basename);
    if (parent_ino == INVALID_BLK) {
        return -1;
    }

    MemINode* parent_ip = imng_.get(parent_ino);
    if (parent_ip == nullptr) {
        return -1;
    }

    if (!check_access(parent_ip, O_WRITE)) {
        imng_.put(parent_ip);
        return -1;
    }

    if (dmng_.lookup(parent_ip, basename.c_str()) != INVALID_BLK) {
        imng_.put(parent_ip);
        return -1;
    }

    // 软链接是一个 inode,数据就是目标路径字符串。路径很短,一个块装得下;
    // 这里防御性地截到 BLOCK_SIZE。
    MemINode* new_ip = imng_.alloc(MODE_SYMLINK | DEFAULT_MODE, cur_uid_, cur_gid_);
    if (new_ip == nullptr) {
        imng_.put(parent_ip);
        return -1;
    }

    size_t tlen = std::strlen(target);
    if (tlen > BLOCK_SIZE) tlen = BLOCK_SIZE;
    uint16_t phys = imng_.bmap_alloc(new_ip, 0);
    if (phys == INVALID_BLK) {
        new_ip->di.di_nlink = 0;  // 置 0,让 put() 时回收这个 inode
        imng_.put(new_ip);
        imng_.put(parent_ip);
        return -1;
    }
    uint8_t blk[BLOCK_SIZE];
    std::memset(blk, 0, BLOCK_SIZE);
    std::memcpy(blk, target, tlen);
    dev_.write_block(DATA_START_BLK + phys, blk);
    new_ip->di.di_size = static_cast<uint32_t>(tlen);
    new_ip->i_dirty = true;

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
    sync();
    return 0;
}

// readlink:读出软链接指向的目标路径(不跟随,只取字符串)。非软链接返回失败。
int UnixFs::fs_readlink(const char* path, std::string& out) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    if (ino == INVALID_BLK) {
        return -1;
    }
    MemINode* ip = imng_.get(ino);
    if (ip == nullptr) {
        return -1;
    }
    if (!(ip->di.di_mode & MODE_SYMLINK)) {
        imng_.put(ip);
        return -1;  // 不是软链接
    }
    out = read_link_target(ip);
    imng_.put(ip);
    return 0;
}

// 内部辅助:从软链接 inode 的首块把目标路径字符串读出来,按 di_size 截取。
std::string UnixFs::read_link_target(MemINode* ip) {
    uint32_t sz = ip->di.di_size;
    if (sz == 0 || sz > BLOCK_SIZE) {
        return std::string();
    }
    uint16_t phys = imng_.bmap(ip, 0);
    if (phys == 0 || phys == INVALID_BLK) {
        return std::string();
    }
    uint8_t blk[BLOCK_SIZE];
    dev_.read_block(DATA_START_BLK + phys, blk);
    return std::string(reinterpret_cast<char*>(blk), sz);
}

// namei_follow:namei 解析后,若结果是软链接就接着解析它的目标,层层跟随。
// 跳数上限把"成环 / 断链"变成一次干净的失败,而不是死循环。
uint16_t UnixFs::namei_follow(const char* path) {
    uint16_t ino = dmng_.namei(path, cwd_ino_, root_ino_);
    // 跟随链式软链接;跳数上限把成环和断链变成干净的失败,而非死循环。
    for (int hops = 0; ino != INVALID_BLK && hops < 16; ++hops) {
        MemINode* ip = imng_.get(ino);
        if (ip == nullptr) {
            return INVALID_BLK;
        }
        if (!(ip->di.di_mode & MODE_SYMLINK)) {
            imng_.put(ip);
            return ino;  // 解析到了真正的文件/目录
        }
        std::string target = read_link_target(ip);
        imng_.put(ip);
        if (target.empty()) {
            return INVALID_BLK;
        }
        // 相对目标从当前目录解析(够应付常见的 "ln -s file link" 和绝对目标)。
        ino = dmng_.namei(target.c_str(), cwd_ino_, root_ino_);
    }
    // 真文件/目录会在上面循环里返回。走到这说明目标无法解析,或撞上跳数上限
    //(成环):失败。
    return INVALID_BLK;
}

// ---- 系统信息 ----

// type_name:引擎名,VFS 切换/标题栏用它区分 UNIX 还是 FAT16。
std::string UnixFs::fs_type_name() const {
    return "UNIX";
}

// disk_usage:给磁盘用量面板算总数/已用数。已用 = 总数 - 空闲(空闲量超级块 O(1) 维护)。
DiskUsage UnixFs::fs_disk_usage() const {
    DiskUsage du{};
    du.total_blocks = sb_.total_blocks();
    du.used_blocks = du.total_blocks - sb_.free_block_count();
    du.total_inodes = sb_.total_inodes();
    du.used_inodes = du.total_inodes - sb_.free_inode_count();
    return du;
}

// block_map:给 TUI 的 F3 磁盘视图算每块的颜色状态(元数据/已用/空闲)。
// 元数据区固定,数据区默认按已用,再用成组空闲链把真正空闲的块标回 FREE。
void UnixFs::fs_block_map(std::vector<uint8_t>& out) {
    out.assign(TOTAL_BLK_NUM, BLK_USED);
    // 引导块 + 超级块 + 整个 inode 区都是固定元数据。
    for (uint32_t b = 0; b < DATA_START_BLK; ++b) out[b] = BLK_META;
    // 数据区先默认 USED;成组空闲链告诉我们哪些是空闲的。
    std::vector<uint16_t> free_blocks;
    sb_.collect_free_blocks(free_blocks);
    for (uint16_t fb : free_blocks) {
        uint32_t abs = DATA_START_BLK + fb;
        if (abs < TOTAL_BLK_NUM) out[abs] = BLK_FREE;
    }
}

// set_user:VFS 在 login/su 后调进来,记下当前 uid/gid 供 check_access 用。
// user_slot_ 固定 0:本引擎的打开文件表按单槽用,不区分多用户的槽位。
void UnixFs::set_user(uint16_t uid, uint16_t gid) {
    cur_uid_ = uid;
    cur_gid_ = gid;
    user_slot_ = 0;
}

// set_disk_path:登记自动落盘的镜像路径,登记后每次写操作都会 sync 一次。
void UnixFs::set_disk_path(const std::string& path) {
    disk_path_ = path;
}

// sync:把超级块刷回内存盘,再把整个内存盘存成宿主机文件,做到崩溃不丢数据。
// 没登记 disk_path_ 就直接跳过(纯内存运行)。
void UnixFs::sync() {
    if (disk_path_.empty()) {
        return;
    }
    sb_.flush();
    dev_.save_to_file(disk_path_.c_str());
}

// check_access:rwx 权限检查。root(uid==0)直接放行;否则按
// owner(同 uid)→ 同组(同 gid)→ 其他人 的顺序选定一组 rwx 位,
// 用这一组来判 required 里要的读/写权限。三组之间是"命中即用",不叠加。
bool UnixFs::check_access(MemINode* ip, uint8_t required) {
    if (cur_uid_ == 0) return true;

    uint16_t mode = ip->di.di_mode;
    uint16_t r_bit, w_bit;

    if (cur_uid_ == ip->di.di_uid) {
        // 属主:用 PERM_U* 这组(owner 权限)
        r_bit = PERM_UR;
        w_bit = PERM_UW;
    } else if (cur_gid_ == ip->di.di_gid) {
        // 同组:用 PERM_G* 这组(group 权限)
        r_bit = PERM_GR;
        w_bit = PERM_GW;
    } else {
        // 其他人:用 PERM_O* 这组(other 权限)
        r_bit = PERM_OR;
        w_bit = PERM_OW;
    }

    if ((required & O_READ) && !(mode & r_bit)) return false;
    // O_APPEND 也是写模式(fs_write 认它),所以和 O_WRITE 一样要写权限位——
    // 否则追加写会悄悄绕过权限检查。
    if ((required & (O_WRITE | O_APPEND)) && !(mode & w_bit)) return false;
    return true;
}

}  // namespace pfs
