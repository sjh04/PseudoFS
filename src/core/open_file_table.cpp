#include "core/open_file_table.h"

#include <cstring>

namespace pfs {

OpenFileTable::OpenFileTable() {
    std::memset(sys_table_, 0, sizeof(sys_table_));
    for (uint32_t u = 0; u < MAX_USER; u++) {
        for (uint32_t f = 0; f < MAX_OPEN_FILE; f++) {
            user_table_[u].ofile[f] = -1;
        }
    }
}

int OpenFileTable::alloc_sys_entry() {
    for (uint32_t i = 0; i < SYS_OPEN_FILE; i++) {
        if (sys_table_[i].f_count == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool OpenFileTable::valid_user_fd(uint16_t user_id, int fd) const {
    if (user_id >= MAX_USER || fd < 0 || fd >= static_cast<int>(MAX_OPEN_FILE)) {
        return false;
    }
    int16_t sys_idx = user_table_[user_id].ofile[fd];
    return sys_idx >= 0 && sys_idx < static_cast<int16_t>(SYS_OPEN_FILE);
}

int OpenFileTable::alloc_fd(uint16_t user_id, uint16_t inode_no, uint8_t flags) {
    if (user_id >= MAX_USER) {
        return -1;
    }

    // Find free user fd slot
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

    // Allocate system table entry
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

uint16_t OpenFileTable::get_inode(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return INVALID_BLK;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_inode_no;
}

uint32_t OpenFileTable::get_offset(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return 0;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_offset;
}

int OpenFileTable::set_offset(uint16_t user_id, int fd, uint32_t offset) {
    if (!valid_user_fd(user_id, fd)) {
        return -1;
    }
    sys_table_[user_table_[user_id].ofile[fd]].f_offset = offset;
    return 0;
}

uint8_t OpenFileTable::get_flags(uint16_t user_id, int fd) const {
    if (!valid_user_fd(user_id, fd)) {
        return 0;
    }
    return sys_table_[user_table_[user_id].ofile[fd]].f_flag;
}

bool OpenFileTable::is_valid(uint16_t user_id, int fd) const {
    return valid_user_fd(user_id, fd);
}

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
