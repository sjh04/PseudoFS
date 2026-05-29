#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/vfs.h"
#include "fs/unix/unix_fs.h"

using namespace pfs;

class UnixFsTest : public ::testing::Test {
   protected:
    BlockDevice dev{TOTAL_BLK_NUM, BLOCK_SIZE};
    UnixFs fs{dev};

    void SetUp() override {
        ASSERT_EQ(fs.fs_format(), 0);
    }
};

TEST_F(UnixFsTest, FormatCreatesRootDirs) {
    std::vector<DirEntry> entries;
    ASSERT_EQ(fs.fs_ls("", entries), 0);

    bool found_etc = false, found_home = false;
    for (auto& e : entries) {
        if (std::strncmp(e.name, "etc", MAX_FILENAME) == 0) found_etc = true;
        if (std::strncmp(e.name, "home", MAX_FILENAME) == 0) found_home = true;
    }
    EXPECT_TRUE(found_etc);
    EXPECT_TRUE(found_home);
}

TEST_F(UnixFsTest, Pwd) {
    EXPECT_EQ(fs.fs_pwd(), "/");
}

TEST_F(UnixFsTest, MkdirAndLs) {
    ASSERT_EQ(fs.fs_mkdir("testdir"), 0);

    std::vector<DirEntry> entries;
    ASSERT_EQ(fs.fs_ls("", entries), 0);

    bool found = false;
    for (auto& e : entries) {
        if (std::strncmp(e.name, "testdir", MAX_FILENAME) == 0) {
            found = true;
            EXPECT_EQ(e.type, TYPE_DIR);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(UnixFsTest, MkdirDuplicateFails) {
    ASSERT_EQ(fs.fs_mkdir("dup"), 0);
    EXPECT_EQ(fs.fs_mkdir("dup"), -1);
}

TEST_F(UnixFsTest, ChdirAndPwd) {
    fs.fs_mkdir("subdir");
    ASSERT_EQ(fs.fs_chdir("subdir"), 0);
    EXPECT_EQ(fs.fs_pwd(), "/subdir");

    ASSERT_EQ(fs.fs_chdir(".."), 0);
    EXPECT_EQ(fs.fs_pwd(), "/");
}

TEST_F(UnixFsTest, CreateAndStat) {
    ASSERT_EQ(fs.fs_create("hello.txt", 0644), 0);

    FileStat st{};
    ASSERT_EQ(fs.fs_stat("hello.txt", st), 0);
    EXPECT_EQ(st.type, TYPE_FILE);
    EXPECT_EQ(st.size, 0);
    EXPECT_EQ(st.nlink, 1);
}

TEST_F(UnixFsTest, WriteAndRead) {
    fs.fs_create("data.txt", 0644);

    int fd = fs.fs_open("data.txt", O_WRITE);
    ASSERT_GE(fd, 0);

    const char* msg = "Hello, PseudoFS!";
    ssize_t written = fs.fs_write(fd, msg, std::strlen(msg));
    EXPECT_EQ(written, static_cast<ssize_t>(std::strlen(msg)));
    fs.fs_close(fd);

    fd = fs.fs_open("data.txt", O_READ);
    ASSERT_GE(fd, 0);

    char buf[64] = {};
    ssize_t read_bytes = fs.fs_read(fd, buf, sizeof(buf));
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, msg);
    fs.fs_close(fd);
}

TEST_F(UnixFsTest, WriteLargeFile) {
    fs.fs_create("big.bin", 0644);
    int fd = fs.fs_open("big.bin", O_WRITE);
    ASSERT_GE(fd, 0);

    // Write 4KB (8 blocks, crosses into indirect range after 6 direct blocks)
    uint8_t data[4096];
    for (int i = 0; i < 4096; i++) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    ssize_t written = fs.fs_write(fd, data, sizeof(data));
    EXPECT_EQ(written, 4096);
    fs.fs_close(fd);

    // Read back and verify
    fd = fs.fs_open("big.bin", O_READ);
    uint8_t rbuf[4096] = {};
    ssize_t read_bytes = fs.fs_read(fd, rbuf, sizeof(rbuf));
    EXPECT_EQ(read_bytes, 4096);
    EXPECT_EQ(std::memcmp(data, rbuf, 4096), 0);
    fs.fs_close(fd);
}

TEST_F(UnixFsTest, DeleteFile) {
    fs.fs_create("remove_me.txt", 0644);
    ASSERT_EQ(fs.fs_delete("remove_me.txt"), 0);

    FileStat st{};
    EXPECT_EQ(fs.fs_stat("remove_me.txt", st), -1);
}

TEST_F(UnixFsTest, DeleteNonexistentFails) {
    EXPECT_EQ(fs.fs_delete("ghost.txt"), -1);
}

TEST_F(UnixFsTest, RmdirEmpty) {
    fs.fs_mkdir("empty_dir");
    ASSERT_EQ(fs.fs_rmdir("empty_dir"), 0);

    FileStat st{};
    EXPECT_EQ(fs.fs_stat("empty_dir", st), -1);
}

TEST_F(UnixFsTest, RmdirNonEmptyFails) {
    fs.fs_mkdir("parent");
    fs.fs_chdir("parent");
    fs.fs_create("child.txt", 0644);
    fs.fs_chdir("..");

    EXPECT_EQ(fs.fs_rmdir("parent"), -1);
}

TEST_F(UnixFsTest, Chmod) {
    fs.fs_create("perm.txt", 0644);
    ASSERT_EQ(fs.fs_chmod("perm.txt", 0755), 0);

    FileStat st{};
    fs.fs_stat("perm.txt", st);
    EXPECT_EQ(st.mode & 0x01FF, 0x01FF & 0755);
}

TEST_F(UnixFsTest, HardLink) {
    fs.fs_create("original.txt", 0644);

    int fd = fs.fs_open("original.txt", O_WRITE);
    const char* msg = "linked content";
    fs.fs_write(fd, msg, std::strlen(msg));
    fs.fs_close(fd);

    ASSERT_EQ(fs.fs_link("original.txt", "alias.txt"), 0);

    // Both names should read the same content
    fd = fs.fs_open("alias.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    fs.fs_read(fd, buf, sizeof(buf));
    EXPECT_STREQ(buf, msg);
    fs.fs_close(fd);

    // nlink should be 2
    FileStat st{};
    fs.fs_stat("original.txt", st);
    EXPECT_EQ(st.nlink, 2);
}

TEST_F(UnixFsTest, MultiLevelPath) {
    fs.fs_mkdir("a");
    fs.fs_chdir("a");
    fs.fs_mkdir("b");
    fs.fs_chdir("b");
    fs.fs_create("deep.txt", 0644);
    fs.fs_chdir("/");

    FileStat st{};
    ASSERT_EQ(fs.fs_stat("a/b/deep.txt", st), 0);
    EXPECT_EQ(st.type, TYPE_FILE);
}

TEST_F(UnixFsTest, DiskUsage) {
    DiskUsage du = fs.fs_disk_usage();
    EXPECT_EQ(du.total_blocks, DATA_BLK_NUM);
    EXPECT_GT(du.used_blocks, 0);
    EXPECT_EQ(du.total_inodes, INODE_TOTAL);
    EXPECT_GT(du.used_inodes, 0);
}

TEST_F(UnixFsTest, Persistence) {
    fs.fs_create("persist.txt", 0644);
    int fd = fs.fs_open("persist.txt", O_WRITE);
    const char* msg = "saved data";
    fs.fs_write(fd, msg, std::strlen(msg));
    fs.fs_close(fd);

    fs.fs_unmount();
    dev.save_to_file("/tmp/pfs_test_persist.img");

    // Load into a fresh device + fs
    BlockDevice dev2(TOTAL_BLK_NUM, BLOCK_SIZE);
    dev2.load_from_file("/tmp/pfs_test_persist.img");
    UnixFs fs2(dev2);
    fs2.fs_mount();

    fd = fs2.fs_open("persist.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    fs2.fs_read(fd, buf, sizeof(buf));
    EXPECT_STREQ(buf, msg);
    fs2.fs_close(fd);
    fs2.fs_unmount();

    std::remove("/tmp/pfs_test_persist.img");
}

// --- Access control tests ---

TEST_F(UnixFsTest, AccessDeniedOpenReadOnly) {
    fs.set_user(1000, 100);
    fs.fs_create("secret.txt", 0600);

    int fd = fs.fs_open("secret.txt", O_WRITE);
    ASSERT_GE(fd, 0);
    fs.fs_write(fd, "data", 4);
    fs.fs_close(fd);

    // Switch to a different user
    fs.set_user(2000, 200);
    EXPECT_EQ(fs.fs_open("secret.txt", O_READ), -1);
    EXPECT_EQ(fs.fs_open("secret.txt", O_WRITE), -1);
}

TEST_F(UnixFsTest, AccessAllowedForOwner) {
    fs.set_user(1000, 100);
    fs.fs_create("myfile.txt", 0600);

    int fd = fs.fs_open("myfile.txt", O_READ);
    ASSERT_GE(fd, 0);
    fs.fs_close(fd);

    fd = fs.fs_open("myfile.txt", O_WRITE);
    ASSERT_GE(fd, 0);
    fs.fs_close(fd);
}

TEST_F(UnixFsTest, RootBypassesPermissions) {
    fs.set_user(1000, 100);
    fs.fs_create("locked.txt", 0000);

    fs.set_user(0, 0);
    int fd = fs.fs_open("locked.txt", O_READ);
    ASSERT_GE(fd, 0);
    fs.fs_close(fd);
}

TEST_F(UnixFsTest, AccessDeniedCreateInDir) {
    fs.set_user(0, 0);
    fs.fs_mkdir("restricted");
    fs.fs_chmod("restricted", 0500);

    fs.set_user(1000, 100);
    EXPECT_EQ(fs.fs_create("restricted/file.txt", 0644), -1);
}

TEST_F(UnixFsTest, AccessDeniedDeleteFile) {
    fs.set_user(0, 0);
    fs.fs_create("protected.txt", 0644);
    fs.fs_chmod(".", 0555);

    fs.set_user(1000, 100);
    EXPECT_EQ(fs.fs_delete("protected.txt"), -1);

    // Restore so TearDown doesn't fail
    fs.set_user(0, 0);
    fs.fs_chmod(".", 0777);
}

TEST_F(UnixFsTest, GroupAccessAllowed) {
    fs.set_user(1000, 100);
    fs.fs_create("group.txt", 0060);

    // Same group, different user
    fs.set_user(2000, 100);
    int fd = fs.fs_open("group.txt", O_READ);
    ASSERT_GE(fd, 0);
    fs.fs_close(fd);
}

TEST_F(UnixFsTest, OtherAccessAllowed) {
    fs.set_user(1000, 100);
    fs.fs_create("public.txt", 0004);

    fs.set_user(3000, 300);
    int fd = fs.fs_open("public.txt", O_READ);
    ASSERT_GE(fd, 0);
    fs.fs_close(fd);
}

// ---- Symbolic links ----

TEST_F(UnixFsTest, SymlinkCreateAndReadlink) {
    ASSERT_EQ(fs.fs_create("target.txt", 0644), 0);
    ASSERT_EQ(fs.fs_symlink("target.txt", "link.txt"), 0);

    std::string out;
    ASSERT_EQ(fs.fs_readlink("link.txt", out), 0);
    EXPECT_EQ(out, "target.txt");
}

TEST_F(UnixFsTest, SymlinkStatReportsLinkType) {
    fs.fs_create("target.txt", 0644);
    fs.fs_symlink("target.txt", "link.txt");

    FileStat st{};
    ASSERT_EQ(fs.fs_stat("link.txt", st), 0);
    EXPECT_EQ(st.type, TYPE_SYMLINK);  // lstat: the link itself, not the target
}

TEST_F(UnixFsTest, SymlinkAppearsInListing) {
    fs.fs_create("target.txt", 0644);
    fs.fs_symlink("target.txt", "link.txt");

    std::vector<DirEntry> entries;
    ASSERT_EQ(fs.fs_ls("", entries), 0);
    bool found = false;
    for (auto& e : entries) {
        if (std::strncmp(e.name, "link.txt", MAX_FILENAME) == 0) {
            found = true;
            EXPECT_EQ(e.type, TYPE_SYMLINK);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(UnixFsTest, SymlinkOpenFollowsToTarget) {
    fs.fs_create("target.txt", 0644);
    int fd = fs.fs_open("target.txt", O_WRITE);
    ASSERT_GE(fd, 0);
    const char* msg = "through-the-link";
    fs.fs_write(fd, msg, std::strlen(msg));
    fs.fs_close(fd);

    ASSERT_EQ(fs.fs_symlink("target.txt", "link.txt"), 0);

    // Opening the link reads the target's content.
    fd = fs.fs_open("link.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {0};
    ssize_t n = fs.fs_read(fd, buf, sizeof(buf));
    fs.fs_close(fd);
    ASSERT_EQ(n, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, msg);
}

TEST_F(UnixFsTest, DeleteSymlinkLeavesTargetIntact) {
    fs.fs_create("target.txt", 0644);
    fs.fs_symlink("target.txt", "link.txt");

    ASSERT_EQ(fs.fs_delete("link.txt"), 0);

    FileStat st{};
    EXPECT_EQ(fs.fs_stat("link.txt", st), -1);   // link gone
    EXPECT_EQ(fs.fs_stat("target.txt", st), 0);  // target survives
}

TEST_F(UnixFsTest, BrokenSymlinkOpenFails) {
    ASSERT_EQ(fs.fs_symlink("nonexistent.txt", "dangling"), 0);
    EXPECT_LT(fs.fs_open("dangling", O_READ), 0);
}

TEST_F(UnixFsTest, SymlinkCycleDoesNotHang) {
    // a -> b, b -> a. Resolution must give up (hop cap), not loop forever.
    ASSERT_EQ(fs.fs_symlink("b", "a"), 0);
    ASSERT_EQ(fs.fs_symlink("a", "b"), 0);
    EXPECT_LT(fs.fs_open("a", O_READ), 0);
}

TEST_F(UnixFsTest, ReadlinkOnRegularFileFails) {
    fs.fs_create("plain.txt", 0644);
    std::string out;
    EXPECT_EQ(fs.fs_readlink("plain.txt", out), -1);
}
