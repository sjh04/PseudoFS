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

// Regression (Bug F3): stat on the root directory must succeed under FAT16
// (it already does under the UNIX engine). resolve_path("/") yields an empty
// final component, which matched no directory entry, so stat returned -1.
TEST_F(Fat16Test, StatRoot) {
    FileStat st{};
    EXPECT_EQ(fs_->fs_stat("/", st), 0);
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

// --- Additional coverage ---

TEST_F(Fat16Test, ChdirDotDotReal) {
    fs_->fs_mkdir("/aaa");
    EXPECT_EQ(fs_->fs_chdir("/aaa"), 0);
    EXPECT_EQ(fs_->fs_chdir(".."), 0);
    EXPECT_EQ(fs_->fs_pwd(), "/");
}

TEST_F(Fat16Test, CreateFileInSubdir) {
    fs_->fs_mkdir("/sub");
    EXPECT_EQ(fs_->fs_create("/sub/inner.txt", DEFAULT_MODE), 0);

    int fd = fs_->fs_open("/sub/inner.txt", O_WRITE);
    ASSERT_GE(fd, 0);
    const char* msg = "inside subdir";
    fs_->fs_write(fd, msg, std::strlen(msg));
    fs_->fs_close(fd);

    fd = fs_->fs_open("/sub/inner.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    ssize_t n = fs_->fs_read(fd, buf, sizeof(buf));
    EXPECT_EQ(n, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, msg);
    fs_->fs_close(fd);
}

TEST_F(Fat16Test, DeepPathStat) {
    fs_->fs_mkdir("/d1");
    fs_->fs_mkdir("/d1/d2");
    fs_->fs_create("/d1/d2/deep.txt", DEFAULT_MODE);

    FileStat st;
    EXPECT_EQ(fs_->fs_stat("/d1/d2/deep.txt", st), 0);
    EXPECT_EQ(st.type, TYPE_FILE);
}

TEST_F(Fat16Test, WriteLargeMultiCluster) {
    fs_->fs_create("/multi.bin", DEFAULT_MODE);
    int fd = fs_->fs_open("/multi.bin", O_WRITE);
    ASSERT_GE(fd, 0);

    // 3KB = 6 blocks, well beyond single cluster
    std::vector<uint8_t> data(3072);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(i % 251);
    }
    ssize_t written = fs_->fs_write(fd, data.data(), data.size());
    EXPECT_EQ(written, 3072);
    fs_->fs_close(fd);

    fd = fs_->fs_open("/multi.bin", O_READ);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> rbuf(3072, 0);
    ssize_t n = fs_->fs_read(fd, rbuf.data(), rbuf.size());
    EXPECT_EQ(n, 3072);
    EXPECT_EQ(std::memcmp(data.data(), rbuf.data(), 3072), 0);
    fs_->fs_close(fd);
}

TEST_F(Fat16Test, DeleteFreesSpace) {
    DiskUsage before = fs_->fs_disk_usage();

    fs_->fs_create("/waste.bin", DEFAULT_MODE);
    int fd = fs_->fs_open("/waste.bin", O_WRITE);
    std::vector<char> data(1024, 'Z');
    fs_->fs_write(fd, data.data(), data.size());
    fs_->fs_close(fd);

    DiskUsage after_write = fs_->fs_disk_usage();
    EXPECT_GT(after_write.used_blocks, before.used_blocks);

    fs_->fs_delete("/waste.bin");

    DiskUsage after_delete = fs_->fs_disk_usage();
    EXPECT_LT(after_delete.used_blocks, after_write.used_blocks);
}

TEST_F(Fat16Test, ReadModeBlocksWrite) {
    fs_->fs_create("/readonly.txt", DEFAULT_MODE);
    int fd = fs_->fs_open("/readonly.txt", O_READ);
    ASSERT_GE(fd, 0);

    ssize_t n = fs_->fs_write(fd, "bad", 3);
    EXPECT_EQ(n, -1);
    fs_->fs_close(fd);
}

TEST_F(Fat16Test, PersistenceFull) {
    fs_->fs_mkdir("/pdir");
    fs_->fs_create("/pdir/saved.txt", DEFAULT_MODE);
    int fd = fs_->fs_open("/pdir/saved.txt", O_WRITE);
    const char* msg = "persistent data";
    fs_->fs_write(fd, msg, std::strlen(msg));
    fs_->fs_close(fd);

    fs_->fs_unmount();
    dev_->save_to_file("/tmp/pfs_fat16_test.img");

    BlockDevice dev2(TOTAL_BLK_NUM, BLOCK_SIZE);
    dev2.load_from_file("/tmp/pfs_fat16_test.img");
    Fat16Fs fs2(dev2);
    fs2.set_user(0, 0);
    ASSERT_EQ(fs2.fs_mount(), 0);

    fd = fs2.fs_open("/pdir/saved.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    ssize_t n = fs2.fs_read(fd, buf, sizeof(buf));
    EXPECT_EQ(n, static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, msg);
    fs2.fs_close(fd);
    fs2.fs_unmount();

    std::remove("/tmp/pfs_fat16_test.img");
}

TEST_F(Fat16Test, LsSubdirectory) {
    fs_->fs_mkdir("/lsdir");
    fs_->fs_create("/lsdir/a.txt", DEFAULT_MODE);
    fs_->fs_create("/lsdir/b.txt", DEFAULT_MODE);

    std::vector<DirEntry> out;
    EXPECT_EQ(fs_->fs_ls("/lsdir", out), 0);
    // . + .. + a.txt + b.txt = 4
    EXPECT_EQ(out.size(), 4u);
}

// --- Regression: a user whose uid >= MAX_USER must still be able to open
// files. FAT16 has no permission model, so the only thing a high uid should
// affect is which open-file-table slot is used — never whether open succeeds.
// (Bug F1: fs_open passed cur_uid_ straight into OpenFileTable as the per-user
// slot index, which rejects user_id >= MAX_USER, so uid>=8 users could open
// nothing in FAT16 while the same user worked fine under the UNIX engine.)
TEST_F(Fat16Test, HighUidCanOpenAndReadWrite) {
    fs_->fs_create("/shared.txt", DEFAULT_MODE);

    fs_->set_user(100, 100);  // uid well past MAX_USER (8)

    int fd = fs_->fs_open("/shared.txt", O_WRITE);
    ASSERT_GE(fd, 0) << "uid>=MAX_USER could not open a FAT16 file";

    const char* msg = "uid 100 was here";
    EXPECT_EQ(fs_->fs_write(fd, msg, std::strlen(msg)), static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_EQ(fs_->fs_close(fd), 0);

    fd = fs_->fs_open("/shared.txt", O_READ);
    ASSERT_GE(fd, 0);
    char buf[64] = {};
    EXPECT_EQ(fs_->fs_read(fd, buf, sizeof(buf) - 1), static_cast<ssize_t>(std::strlen(msg)));
    EXPECT_STREQ(buf, msg);
    fs_->fs_close(fd);
}

// Regression (Bug F2): the file-size lookup must read the file's actual parent
// directory, never "every allocated cluster interpreted as a directory". Here
// we force the victim's data cluster to be numbered LOWER than its parent
// directory's cluster, then write content that aliases a self-referential,
// non-directory entry. The old brute-force scan reached the data cluster first,
// matched the fake entry, and updated/read the wrong place — so the real
// directory entry's size was never written. Observable as stat reporting 0.
TEST_F(Fat16Test, FileSizeNotConfusedByDataAliasingDirEntry) {
    fs_->fs_create("/a", DEFAULT_MODE);           // takes the lowest cluster
    fs_->fs_mkdir("/sub");                        // takes the next cluster
    fs_->fs_delete("/a");                         // frees the lowest cluster
    fs_->fs_create("/sub/victim", DEFAULT_MODE);  // reuses it: data cluster < parent

    // victim's first cluster is exposed as inode_no in the FAT16 listing.
    std::vector<DirEntry> sub;
    ASSERT_EQ(fs_->fs_ls("/sub", sub), 0);
    uint16_t vc = 0;
    for (auto& e : sub)
        if (std::string(e.name) == "VICTIM") vc = e.inode_no;
    ASSERT_GE(vc, 2);

    // 32 bytes parsing as FAT16DirEntry{name="X", attr=ARCHIVE,
    // first_cluster=vc, file_size=0}: a non-directory entry pointing at itself.
    uint8_t craft[32] = {};
    craft[0] = 'X';
    craft[11] = FAT16_ATTR_ARCHIVE;  // no FAT16_ATTR_DIRECTORY bit
    craft[26] = static_cast<uint8_t>(vc & 0xFF);
    craft[27] = static_cast<uint8_t>(vc >> 8);

    int fd = fs_->fs_open("/sub/victim", O_WRITE);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(fs_->fs_write(fd, craft, sizeof(craft)), 32);
    fs_->fs_close(fd);

    FileStat st{};
    ASSERT_EQ(fs_->fs_stat("/sub/victim", st), 0);
    EXPECT_EQ(st.size, 32u) << "size lost: the lookup matched the file's data cluster instead of "
                               "its real parent directory entry";
}

TEST_F(Fat16Test, DeleteThenCreateSameName) {
    fs_->fs_create("/reuse.txt", DEFAULT_MODE);
    fs_->fs_delete("/reuse.txt");
    EXPECT_EQ(fs_->fs_create("/reuse.txt", DEFAULT_MODE), 0);

    std::vector<DirEntry> out;
    fs_->fs_ls("/", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].name, "REUSE.TXT");
}

}  // namespace
}  // namespace pfs
