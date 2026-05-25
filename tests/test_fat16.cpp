#include <gtest/gtest.h>

#include <cstring>

#include "fs/fat16/fat16_fs.h"

namespace pfs {
namespace {

class Fat16Test : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = new BlockDevice(TOTAL_BLK_NUM, BLOCK_SIZE);
        fs_ = new Fat16Fs(*dev_);
        fs_->set_user(0, 0);
        fs_->fs_format();
    }

    void TearDown() override {
        delete fs_;
        delete dev_;
    }

    BlockDevice* dev_;
    Fat16Fs* fs_;
};

// --- Format and Mount ---

TEST_F(Fat16Test, FormatInitializesBootSector) {
    EXPECT_EQ(fs_->fs_type_name(), "FAT16");
}

TEST_F(Fat16Test, UnmountAndRemount) {
    fs_->fs_create("/test.txt", DEFAULT_MODE);
    EXPECT_EQ(fs_->fs_unmount(), 0);

    auto* fs2 = new Fat16Fs(*dev_);
    fs2->set_user(0, 0);
    EXPECT_EQ(fs2->fs_mount(), 0);

    std::vector<DirEntry> out;
    EXPECT_EQ(fs2->fs_ls("/", out), 0);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].name, "TEST.TXT");

    delete fs2;
}

// --- File Create ---

TEST_F(Fat16Test, CreateFileInRoot) {
    EXPECT_EQ(fs_->fs_create("/hello.txt", DEFAULT_MODE), 0);

    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].name, "HELLO.TXT");
    EXPECT_EQ(out[0].type, TYPE_FILE);
}

TEST_F(Fat16Test, CreateDuplicateFails) {
    EXPECT_EQ(fs_->fs_create("/dup.txt", DEFAULT_MODE), 0);
    EXPECT_EQ(fs_->fs_create("/dup.txt", DEFAULT_MODE), -1);
}

// --- Mkdir ---

TEST_F(Fat16Test, MkdirInRoot) {
    EXPECT_EQ(fs_->fs_mkdir("/subdir"), 0);

    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].name, "SUBDIR");
    EXPECT_EQ(out[0].type, TYPE_DIR);
}

TEST_F(Fat16Test, MkdirNested) {
    EXPECT_EQ(fs_->fs_mkdir("/a"), 0);
    EXPECT_EQ(fs_->fs_mkdir("/a/b"), 0);

    std::vector<DirEntry> out;
    fs_->fs_ls("/a", out);
    ASSERT_EQ(out.size(), 3u);  // . + .. + b
}

// --- Chdir and Pwd ---

TEST_F(Fat16Test, ChdirAbsolute) {
    fs_->fs_mkdir("/home");
    EXPECT_EQ(fs_->fs_chdir("/home"), 0);
    EXPECT_EQ(fs_->fs_pwd(), "/home");
}

TEST_F(Fat16Test, ChdirRelative) {
    fs_->fs_mkdir("/home");
    fs_->fs_chdir("/home");
    fs_->fs_mkdir("alice");
    fs_->fs_chdir("alice");
    EXPECT_EQ(fs_->fs_pwd(), "/home/alice");
}

TEST_F(Fat16Test, ChdirDotDot) {
    fs_->fs_mkdir("/x");
    fs_->fs_chdir("/x");
    fs_->fs_chdir("/");
    EXPECT_EQ(fs_->fs_pwd(), "/");
}

// --- Rmdir ---

TEST_F(Fat16Test, RmdirEmpty) {
    fs_->fs_mkdir("/empty");
    EXPECT_EQ(fs_->fs_rmdir("/empty"), 0);
    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    EXPECT_TRUE(out.empty());
}

TEST_F(Fat16Test, RmdirNonEmptyFails) {
    fs_->fs_mkdir("/dir");
    fs_->fs_create("/dir/file.txt", DEFAULT_MODE);
    EXPECT_EQ(fs_->fs_rmdir("/dir"), -1);
}

// --- Open / Close / Read / Write ---

TEST_F(Fat16Test, OpenWriteReadClose) {
    fs_->fs_create("/data.bin", DEFAULT_MODE);

    int fd = fs_->fs_open("/data.bin", O_WRITE);
    ASSERT_GE(fd, 0);

    const char* msg = "Hello, FAT16!";
    ssize_t written = fs_->fs_write(fd, msg, std::strlen(msg));
    EXPECT_EQ(written, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_EQ(fs_->fs_close(fd), 0);

    fd = fs_->fs_open("/data.bin", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    ssize_t n = fs_->fs_read(fd, buf, sizeof(buf) - 1);
    EXPECT_EQ(n, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, "Hello, FAT16!");
    fs_->fs_close(fd);
}

TEST_F(Fat16Test, WriteLargeData) {
    fs_->fs_create("/big.bin", DEFAULT_MODE);
    int fd = fs_->fs_open("/big.bin", O_WRITE);
    ASSERT_GE(fd, 0);

    std::vector<char> data(1500, 'X');
    ssize_t written = fs_->fs_write(fd, data.data(), data.size());
    EXPECT_EQ(written, 1500);
    fs_->fs_close(fd);

    fd = fs_->fs_open("/big.bin", O_READ);
    ASSERT_GE(fd, 0);
    std::vector<char> buf(2000, 0);
    ssize_t n = fs_->fs_read(fd, buf.data(), buf.size());
    EXPECT_EQ(n, 1500);
    EXPECT_EQ(std::memcmp(data.data(), buf.data(), 1500), 0);
    fs_->fs_close(fd);
}

TEST_F(Fat16Test, OpenNonExistentFails) {
    EXPECT_EQ(fs_->fs_open("/nope.txt", O_READ), -1);
}

TEST_F(Fat16Test, ReadFromEmptyFile) {
    fs_->fs_create("/empty.txt", DEFAULT_MODE);
    int fd = fs_->fs_open("/empty.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[16];
    ssize_t n = fs_->fs_read(fd, buf, sizeof(buf));
    EXPECT_EQ(n, 0);
    fs_->fs_close(fd);
}

// --- Delete ---

TEST_F(Fat16Test, DeleteFile) {
    fs_->fs_create("/rm.txt", DEFAULT_MODE);
    EXPECT_EQ(fs_->fs_delete("/rm.txt"), 0);
    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    EXPECT_TRUE(out.empty());
}

TEST_F(Fat16Test, DeleteNonExistentFails) {
    EXPECT_EQ(fs_->fs_delete("/ghost.txt"), -1);
}

// --- Stat ---

TEST_F(Fat16Test, StatFile) {
    fs_->fs_create("/info.txt", DEFAULT_MODE);
    int fd = fs_->fs_open("/info.txt", O_WRITE);
    fs_->fs_write(fd, "data", 4);
    fs_->fs_close(fd);

    FileStat st;
    EXPECT_EQ(fs_->fs_stat("/info.txt", st), 0);
    EXPECT_EQ(st.size, 4u);
    EXPECT_EQ(st.type, TYPE_FILE);
}

TEST_F(Fat16Test, StatDir) {
    fs_->fs_mkdir("/mydir");
    FileStat st;
    EXPECT_EQ(fs_->fs_stat("/mydir", st), 0);
    EXPECT_EQ(st.type, TYPE_DIR);
}

// --- Disk Usage ---

TEST_F(Fat16Test, DiskUsageAfterFormat) {
    DiskUsage usage = fs_->fs_disk_usage();
    EXPECT_EQ(usage.total_blocks, TOTAL_BLK_NUM);
    EXPECT_EQ(usage.total_inodes, 0u);
    // System area + 2 reserved FAT entries
    EXPECT_GT(usage.used_blocks, 0u);
}

// --- Chmod and Link (stubs) ---

TEST_F(Fat16Test, ChmodReturnsOK) {
    fs_->fs_create("/chmod.txt", DEFAULT_MODE);
    EXPECT_EQ(fs_->fs_chmod("/chmod.txt", 0755), 0);
}

TEST_F(Fat16Test, HardLinkNotSupported) {
    fs_->fs_create("/src.txt", DEFAULT_MODE);
    EXPECT_EQ(fs_->fs_link("/src.txt", "/dst.txt"), -1);
}

// --- Root ls is empty after format ---

TEST_F(Fat16Test, RootIsEmptyAfterFormat) {
    std::vector<DirEntry> out;
    EXPECT_EQ(fs_->fs_ls("/", out), 0);
    EXPECT_TRUE(out.empty());
}

// --- File with extension ---

TEST_F(Fat16Test, FileNameWithExtension) {
    EXPECT_EQ(fs_->fs_create("/document.txt", DEFAULT_MODE), 0);
    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].name, "DOCUMENT.TXT");
}

// --- Long filename truncation ---

TEST_F(Fat16Test, LongFileNameTruncated) {
    EXPECT_EQ(fs_->fs_create("/verylongfilename.txt", DEFAULT_MODE), 0);
    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    ASSERT_EQ(out.size(), 1u);
    // Should be truncated to 8.3: "VERYLONG.TXT"
    EXPECT_STREQ(out[0].name, "VERYLONG.TXT");
}

}  // namespace
}  // namespace pfs
