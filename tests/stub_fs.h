#pragma once

#include <string>
#include <vector>

#include "core/vfs.h"

namespace pfs {

// A no-op IFileSystem for tests that only exercise command dispatch (not a real
// disk). execute() calls fs.set_user(...) on every command, so the reference it
// receives must point at a live object — use this instead of a buffer cast.
class StubFs : public IFileSystem {
   public:
    int fs_format() override { return 0; }
    int fs_mount() override { return 0; }
    int fs_unmount() override { return 0; }
    void set_user(uint16_t, uint16_t) override {}

    int fs_create(const char*, uint16_t) override { return 0; }
    int fs_open(const char*, int) override { return 0; }
    int fs_close(int) override { return 0; }
    ssize_t fs_read(int, void*, size_t) override { return 0; }
    ssize_t fs_write(int, const void*, size_t) override { return 0; }
    int fs_delete(const char*) override { return 0; }
    int fs_delete_recursive(const char*) override { return 0; }

    int fs_mkdir(const char*) override { return 0; }
    int fs_rmdir(const char*) override { return 0; }
    int fs_chdir(const char*) override { return 0; }
    int fs_ls(const char*, std::vector<DirEntry>&) override { return 0; }
    std::string fs_pwd() override { return "/"; }

    int fs_stat(const char*, FileStat&) override { return 0; }
    int fs_chmod(const char*, uint16_t) override { return 0; }
    int fs_link(const char*, const char*) override { return 0; }
    int fs_symlink(const char*, const char*) override { return 0; }
    int fs_readlink(const char*, std::string&) override { return 0; }

    std::string fs_type_name() const override { return "STUB"; }
    DiskUsage fs_disk_usage() const override { return {}; }
    void fs_block_map(std::vector<uint8_t>& out) override { out.clear(); }
};

}  // namespace pfs
