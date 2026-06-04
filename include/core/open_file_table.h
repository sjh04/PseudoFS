#pragma once

#include <cstdint>

#include "core/constants.h"

namespace pfs {

// 系统打开文件表项。
// 若多个用户打开同一文件则共享此项(用引用计数管理)。
// 偏移放在这里,是为了让共享同一项的多个 fd 一起推进读写位置(dup 语义)。
struct SysOpenFileEntry {
    uint8_t f_flag;       // 打开模式:O_READ, O_WRITE, O_APPEND
    uint16_t f_count;     // 引用计数
    uint16_t f_inode_no;  // inode 编号
    uint32_t f_offset;    // 当前读写位置
};

// 每用户的打开文件表:把用户的 fd(0..MAX_OPEN_FILE-1)
// 映射到系统打开文件表的下标。
// 值为 -1 表示该 fd 槽空闲。
struct UserOpenFiles {
    int16_t ofile[MAX_OPEN_FILE];
};

// 打开文件表:系统表 + 每用户用户表两层。用户表 ofile[fd] 存系统表下标,
// 系统表存 f_flag/f_count/f_inode_no/f_offset;多个 fd 可指向同一系统表项。

class OpenFileTable {
   public:
    OpenFileTable();

    // 给用户分配一个新 fd,并建立一个系统表项。
    // 返回 fd(从 0 起),用户表或系统表已满则返回 -1。
    int alloc_fd(uint16_t user_id, uint16_t inode_no, uint8_t flags);

    // 关闭一个 fd:系统表项引用计数减一,腾空用户表槽。
    // 成功返回 0,fd 非法返回 -1。
    int free_fd(uint16_t user_id, int fd);

    // 取用户某 fd 对应的 inode 编号。fd 非法返回 INVALID_BLK。
    uint16_t get_inode(uint16_t user_id, int fd) const;

    // 读/写某 fd 的读写偏移。
    uint32_t get_offset(uint16_t user_id, int fd) const;
    int set_offset(uint16_t user_id, int fd, uint32_t offset);

    // 取某 fd 的打开标志。
    uint8_t get_flags(uint16_t user_id, int fd) const;

    // 判断某用户的 fd 是否有效。
    bool is_valid(uint16_t user_id, int fd) const;

    // 关闭某用户打开的全部文件(登出时使用)。
    void close_all(uint16_t user_id);

   private:
    SysOpenFileEntry sys_table_[SYS_OPEN_FILE];
    UserOpenFiles user_table_[MAX_USER];

    int alloc_sys_entry();
    bool valid_user_fd(uint16_t user_id, int fd) const;
};

}  // namespace pfs
