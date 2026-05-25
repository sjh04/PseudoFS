#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "fs/unix/inode.h"

namespace pfs {

// On-disk directory entry, 16 bytes.
// A directory file is simply an array of these entries.
// d_ino == 0 means the slot is empty (deleted or unused).
struct DiskDirEntry {
    char d_name[MAX_FILENAME];  // 14 bytes, null-padded
    uint16_t d_ino;             // inode number, 0 = empty
};

static_assert(sizeof(DiskDirEntry) == DIR_ENTRY_SIZE, "DiskDirEntry must be 16 bytes");

class DirectoryManager {
public:
    DirectoryManager(BlockDevice& dev, InodeManager& imng);

    // Resolve a path (absolute or relative to cwd) to an inode number.
    // Returns INVALID_BLK if not found.
    // cwd_ino: inode number of the current working directory.
    // root_ino: inode number of the root directory.
    uint16_t namei(const char* path, uint16_t cwd_ino, uint16_t root_ino);

    // Like namei, but returns the parent directory's inode number and
    // fills 'basename' with the last component of the path.
    // Used by create/mkdir/delete — they need the parent to insert/remove entries.
    uint16_t namei_parent(const char* path, uint16_t cwd_ino, uint16_t root_ino,
                          std::string& basename);

    // Look up a name in a directory inode. Returns the inode number,
    // or INVALID_BLK if not found.
    uint16_t lookup(MemINode* dir_ip, const char* name);

    // Add a new entry (name → ino) to a directory.
    // Returns 0 on success, -1 on failure (dir full or name exists).
    int link(MemINode* dir_ip, const char* name, uint16_t ino);

    // Remove an entry by name from a directory.
    // Returns 0 on success, -1 if not found.
    int unlink(MemINode* dir_ip, const char* name);

    // Read all entries from a directory into a vector.
    int read_entries(MemINode* dir_ip, std::vector<DiskDirEntry>& out);

    // Initialize a new directory: write "." and ".." entries.
    // self_ino: this directory's inode, parent_ino: parent directory's inode.
    int init_dir(MemINode* dir_ip, uint16_t self_ino, uint16_t parent_ino);

    // Check if a directory is empty (only has "." and "..").
    bool is_empty(MemINode* dir_ip);

private:
    BlockDevice& dev_;
    InodeManager& imng_;

    // Split a path into components. "/home/alice" → ["home", "alice"]
    std::vector<std::string> split_path(const char* path);

    // Read/write the raw directory data (array of DiskDirEntry) from an inode.
    int read_dir_data(MemINode* dir_ip, std::vector<DiskDirEntry>& entries);
    int write_dir_data(MemINode* dir_ip, const std::vector<DiskDirEntry>& entries);
};

}  // namespace pfs
