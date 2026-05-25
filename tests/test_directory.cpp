#include <gtest/gtest.h>

#include <cstring>

#include "core/block_device.h"
#include "core/constants.h"
#include "fs/unix/directory.h"
#include "fs/unix/inode.h"
#include "fs/unix/super_block.h"

using namespace pfs;

class DirectoryTest : public ::testing::Test {
protected:
    BlockDevice dev{TOTAL_BLK_NUM, BLOCK_SIZE};
    SuperBlock sb{dev};
    InodeManager imng{dev, sb};
    DirectoryManager dmng{dev, imng};
    MemINode* root_ip = nullptr;
    uint16_t root_ino = 0;

    void SetUp() override {
        sb.format(0, 0);

        // Create root directory
        root_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
        root_ino = root_ip->i_ino;
        dmng.init_dir(root_ip, root_ino, root_ino);
        imng.write_back(root_ip);
    }

    void TearDown() override {
        if (root_ip) {
            imng.put(root_ip);
        }
    }
};

TEST_F(DirectoryTest, InitDirHasDotAndDotDot) {
    std::vector<DiskDirEntry> entries;
    EXPECT_EQ(dmng.read_entries(root_ip, entries), 0);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_STREQ(entries[0].d_name, ".");
    EXPECT_EQ(entries[0].d_ino, root_ino);
    EXPECT_STREQ(entries[1].d_name, "..");
    EXPECT_EQ(entries[1].d_ino, root_ino);
}

TEST_F(DirectoryTest, LinkAndLookup) {
    MemINode* file_ip = imng.alloc(MODE_FILE | DEFAULT_MODE, 0, 0);
    uint16_t file_ino = file_ip->i_ino;
    imng.write_back(file_ip);
    imng.put(file_ip);

    EXPECT_EQ(dmng.link(root_ip, "hello.txt", file_ino), 0);
    EXPECT_EQ(dmng.lookup(root_ip, "hello.txt"), file_ino);
}

TEST_F(DirectoryTest, LookupNotFound) {
    EXPECT_EQ(dmng.lookup(root_ip, "nonexistent"), INVALID_BLK);
}

TEST_F(DirectoryTest, DuplicateNameFails) {
    MemINode* f1 = imng.alloc(MODE_FILE, 0, 0);
    imng.write_back(f1);
    EXPECT_EQ(dmng.link(root_ip, "dup", f1->i_ino), 0);

    MemINode* f2 = imng.alloc(MODE_FILE, 0, 0);
    imng.write_back(f2);
    EXPECT_EQ(dmng.link(root_ip, "dup", f2->i_ino), -1);

    imng.put(f1);
    imng.put(f2);
}

TEST_F(DirectoryTest, UnlinkEntry) {
    MemINode* file_ip = imng.alloc(MODE_FILE | DEFAULT_MODE, 0, 0);
    uint16_t file_ino = file_ip->i_ino;
    imng.write_back(file_ip);
    imng.put(file_ip);

    dmng.link(root_ip, "remove_me", file_ino);
    EXPECT_EQ(dmng.lookup(root_ip, "remove_me"), file_ino);

    EXPECT_EQ(dmng.unlink(root_ip, "remove_me"), 0);
    EXPECT_EQ(dmng.lookup(root_ip, "remove_me"), INVALID_BLK);
}

TEST_F(DirectoryTest, UnlinkNotFoundFails) {
    EXPECT_EQ(dmng.unlink(root_ip, "ghost"), -1);
}

TEST_F(DirectoryTest, IsEmptyOnFreshDir) {
    EXPECT_TRUE(dmng.is_empty(root_ip));

    MemINode* f = imng.alloc(MODE_FILE, 0, 0);
    imng.write_back(f);
    dmng.link(root_ip, "file", f->i_ino);
    imng.put(f);

    EXPECT_FALSE(dmng.is_empty(root_ip));
}

TEST_F(DirectoryTest, NameiSimplePath) {
    // Create /home directory
    MemINode* home_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    uint16_t home_ino = home_ip->i_ino;
    dmng.init_dir(home_ip, home_ino, root_ino);
    imng.write_back(home_ip);
    dmng.link(root_ip, "home", home_ino);
    imng.put(home_ip);

    EXPECT_EQ(dmng.namei("home", root_ino, root_ino), home_ino);
    EXPECT_EQ(dmng.namei("/home", root_ino, root_ino), home_ino);
}

TEST_F(DirectoryTest, NameiMultiLevel) {
    // Create /home/alice
    MemINode* home_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    uint16_t home_ino = home_ip->i_ino;
    dmng.init_dir(home_ip, home_ino, root_ino);
    imng.write_back(home_ip);
    dmng.link(root_ip, "home", home_ino);

    MemINode* alice_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    uint16_t alice_ino = alice_ip->i_ino;
    dmng.init_dir(alice_ip, alice_ino, home_ino);
    imng.write_back(alice_ip);
    dmng.link(home_ip, "alice", alice_ino);

    imng.put(home_ip);
    imng.put(alice_ip);

    EXPECT_EQ(dmng.namei("/home/alice", root_ino, root_ino), alice_ino);
}

TEST_F(DirectoryTest, NameiDotDot) {
    MemINode* home_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    uint16_t home_ino = home_ip->i_ino;
    dmng.init_dir(home_ip, home_ino, root_ino);
    imng.write_back(home_ip);
    dmng.link(root_ip, "home", home_ino);
    imng.put(home_ip);

    // From /home, ".." should go back to root
    EXPECT_EQ(dmng.namei("..", home_ino, root_ino), root_ino);
    // "." should stay
    EXPECT_EQ(dmng.namei(".", home_ino, root_ino), home_ino);
}

TEST_F(DirectoryTest, NameiParent) {
    MemINode* home_ip = imng.alloc(MODE_DIR | DEFAULT_MODE, 0, 0);
    uint16_t home_ino = home_ip->i_ino;
    dmng.init_dir(home_ip, home_ino, root_ino);
    imng.write_back(home_ip);
    dmng.link(root_ip, "home", home_ino);
    imng.put(home_ip);

    std::string basename;
    uint16_t parent = dmng.namei_parent("/home/newfile.txt", root_ino, root_ino, basename);
    EXPECT_EQ(parent, home_ino);
    EXPECT_EQ(basename, "newfile.txt");
}

TEST_F(DirectoryTest, NameiNotFound) {
    EXPECT_EQ(dmng.namei("/no/such/path", root_ino, root_ino), INVALID_BLK);
}

TEST_F(DirectoryTest, ReuseDeletedSlot) {
    MemINode* f1 = imng.alloc(MODE_FILE, 0, 0);
    imng.write_back(f1);
    dmng.link(root_ip, "first", f1->i_ino);
    imng.put(f1);

    dmng.unlink(root_ip, "first");

    MemINode* f2 = imng.alloc(MODE_FILE, 0, 0);
    imng.write_back(f2);
    EXPECT_EQ(dmng.link(root_ip, "second", f2->i_ino), 0);

    // Directory should not grow — the deleted slot is reused
    std::vector<DiskDirEntry> entries;
    dmng.read_entries(root_ip, entries);
    // ".", "..", "second" — the deleted "first" slot is reused by "second"
    EXPECT_EQ(entries.size(), 3);

    imng.put(f2);
}
