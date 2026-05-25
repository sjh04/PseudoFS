#pragma once

#include <cstdint>

#include "core/constants.h"

namespace pfs {

// System-wide open file table entry.
// Shared across users if they open the same file (ref counted).
struct SysOpenFileEntry {
    uint8_t f_flag;         // open mode: O_READ, O_WRITE, O_APPEND
    uint16_t f_count;       // reference count
    uint16_t f_inode_no;    // inode number
    uint32_t f_offset;      // current read/write position
};

// Per-user open file table: maps user fd (0..MAX_OPEN_FILE-1)
// to an index in the system open file table.
// A value of -1 means the fd slot is free.
struct UserOpenFiles {
    int16_t ofile[MAX_OPEN_FILE];
};

class OpenFileTable {
public:
    OpenFileTable();

    // Allocate a new fd for a user. Creates a system table entry.
    // Returns fd (0-based), or -1 if user table or system table is full.
    int alloc_fd(uint16_t user_id, uint16_t inode_no, uint8_t flags);

    // Close a fd: decrement system entry ref count, free user slot.
    // Returns 0 on success, -1 if invalid.
    int free_fd(uint16_t user_id, int fd);

    // Get the inode number for a user's fd. Returns INVALID_BLK if invalid.
    uint16_t get_inode(uint16_t user_id, int fd) const;

    // Get/set the read/write offset for a fd.
    uint32_t get_offset(uint16_t user_id, int fd) const;
    int set_offset(uint16_t user_id, int fd, uint32_t offset);

    // Get open flags for a fd.
    uint8_t get_flags(uint16_t user_id, int fd) const;

    // Check if a fd is valid for a user.
    bool is_valid(uint16_t user_id, int fd) const;

    // Close all files opened by a user (used by logout).
    void close_all(uint16_t user_id);

private:
    SysOpenFileEntry sys_table_[SYS_OPEN_FILE];
    UserOpenFiles user_table_[MAX_USER];

    int alloc_sys_entry();
    bool valid_user_fd(uint16_t user_id, int fd) const;
};

}  // namespace pfs
