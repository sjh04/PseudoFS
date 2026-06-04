#include "core/open_file_table.h"

#include <cstring>

namespace pfs {

// 打开文件表:系统表 + 每用户用户表两层结构。
// 系统表清零(f_count==0 即空闲),用户表每个 fd 槽置 -1 表示未占用。
OpenFileTable::OpenFileTable() {
    std::memset(sys_table_, 0, sizeof(sys_table_));
    for (uint32_t u = 0; u < MAX_USER; u++) {
        for (uint32_t f = 0; f < MAX_OPEN_FILE; f++) {
            user_table_[u].ofile[f] = -1;
        }
    }
}

// 在系统表里找一个空闲项(f_count==0)并返回其下标,满了返回 -1。
// 偏移/引用计数都放系统表,多个 fd 才能共享同一打开实例(dup 语义)。
int OpenFileTable::alloc_sys_entry() {
    for (uint32_t i = 0; i < SYS_OPEN_FILE; i++) {
        if (sys_table_[i].f_count == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 校验 (user_id, fd) 合法:用户号和 fd 都在范围内,且该 fd 槽确实
// 指向一个有效的系统表下标。所有读写接口先过这一关再碰系统表。
bool OpenFileTable::valid_user_fd(uint16_t user_id, int fd) const {
    if (user_id >= MAX_USER || fd < 0 || fd >= static_cast<int>(MAX_OPEN_FILE)) {
        return false;
    }
    int16_t sys_idx = user_table_[user_id].ofile[fd];
    return sys_idx >= 0 && sys_idx < static_cast<int16_t>(SYS_OPEN_FILE);
}

// open 的核心:给用户分一个 fd。先在用户表占一个空 fd 槽,再在系统表
// 建一个全新打开实例(f_count 置 1、偏移归零),最后把槽指向该系统表项。
// 这里每次 open 都新建系统表项,fd 共享只在 dup 等场景才会出现。
int OpenFileTable::alloc_fd(uint16_t user_id, uint16_t inode_no, uint8_t flags) {
    if (user_id >= MAX_USER) {
        return -1;
    }

    // 在用户表里找一个空闲 fd 槽
    int fd = -1;
    for (uint32_t i = 0; i < MAX_OPEN_FILE; i++) {
        if (user_table_[user_id].ofile[i] == -1) {
            fd = static_cast<int>(i);
            break;
        }
    }
    if (fd == -1) {
        return -1;
    }

    // 分配一个系统表项
    int sys_idx = alloc_sys_entry();
    if (sys_idx == -1) {
        return -1;
    }

    sys_table_[sys_idx].f_flag = flags;
    sys_table_[sys_idx].f_count = 1;
    sys_table_[sys_idx].f_inode_no = inode_no;
    sys_table_[sys_idx].f_offset = 0;

    user_table_[user_id].ofile[fd] = static_cast<int16_t>(sys_idx);
    return fd;
}

// close 的核心:腾空用户表 fd 槽,再把系统表项引用计数减一。
// 只有最后一个 fd 关掉(f_count 归零)才真正清空系统表项——多个 fd
// 可能指向同一项,过早清空会殃及其他还开着的 fd。
int OpenFileTable::free_fd(uint16_t user_id, int fd) {
    if (!valid_user_fd(user_id, fd)) {
        return -1;
    }

    int16_t sys_idx = user_table_[user_id].ofile[fd];
    user_table_[user_id].ofile[fd] = -1;

    sys_table_[sys_idx].f_count--;
    if (sys_table_[sys_idx].f_count == 0) {
        std::memset(&sys_table_[sys_idx], 0, sizeof(SysOpenFileEntry));
    }
    return 0;
}

// 由 fd 取出它对应的 inode 号(经用户表槽转到系统表读 f_inode_no)。
uint16_t OpenFileTable::get_inode(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return INVALID_BLK;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_inode_no;
}

// 读 fd 当前读写偏移。偏移存在系统表,共享同一项的 fd 会一起推进。
uint32_t OpenFileTable::get_offset(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return 0;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_offset;
}

// 写回 fd 读写偏移,read/write 推进位置和 seek 都靠它落到系统表上。
int OpenFileTable::set_offset(uint16_t user_id, int fd, uint32_t offset) {
    if (!valid_user_fd(user_id, fd)) {
        return -1;
    }
    sys_table_[user_table_[user_id].ofile[fd]].f_offset = offset;
    return 0;
}

// 取 fd 的打开标志(读/写/追加),read/write 据此判断是否越权操作。
uint8_t OpenFileTable::get_flags(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return 0;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_flag;
}

// 对外的 fd 合法性查询,内部直接转 valid_user_fd。
bool OpenFileTable::is_valid(uint16_t user_id, int fd) const {
    return valid_user_fd(user_id, fd);
}

// 关掉某用户名下所有还开着的 fd,logout 时调用,免得 fd 和系统表项泄漏。
void OpenFileTable::close_all(uint16_t user_id) {
    if (user_id >= MAX_USER) {
        return;
    }
    for (uint32_t f = 0; f < MAX_OPEN_FILE; f++) {
        if (user_table_[user_id].ofile[f] != -1) {
            free_fd(user_id, static_cast<int>(f));
        }
    }
}

}  // namespace pfs
