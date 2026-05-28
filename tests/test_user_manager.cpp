#include <gtest/gtest.h>

#include "core/user_manager.h"

namespace pfs {
namespace {

class UserManagerTest : public ::testing::Test {
protected:
    UserManager um;
};

// --- Initial state ---

TEST_F(UserManagerTest, RootUserExists) {
    auto* r = um.find_user(uint16_t(0));
    ASSERT_NE(r, nullptr);
    EXPECT_STREQ(r->username, "root");
    EXPECT_EQ(r->uid, 0);
    EXPECT_EQ(r->gid, 0);
}

TEST_F(UserManagerTest, NotLoggedInInitially) {
    EXPECT_FALSE(um.is_logged_in());
    EXPECT_EQ(um.current_uid(), 0);
    EXPECT_EQ(um.current_username(), "");
}

// --- Login / Logout ---

TEST_F(UserManagerTest, LoginRoot) {
    int uid = um.login("root", "root");
    EXPECT_EQ(uid, 0);
    EXPECT_TRUE(um.is_logged_in());
    EXPECT_EQ(um.current_username(), "root");
}

TEST_F(UserManagerTest, LoginWrongPassword) {
    EXPECT_EQ(um.login("root", "wrong"), -1);
    EXPECT_FALSE(um.is_logged_in());
}

TEST_F(UserManagerTest, LoginNonexistentUser) {
    EXPECT_EQ(um.login("nobody", "x"), -1);
}

TEST_F(UserManagerTest, LogoutSuccess) {
    um.login("root", "root");
    EXPECT_EQ(um.logout(), 0);
    EXPECT_FALSE(um.is_logged_in());
}

TEST_F(UserManagerTest, LogoutWhenNotLoggedIn) {
    EXPECT_EQ(um.logout(), -1);
}

// --- Add User ---

TEST_F(UserManagerTest, RootCanAddUser) {
    um.login("root", "root");
    EXPECT_EQ(um.add_user("alice", "pass1", 1, 1), 0);

    auto* u = um.find_user(1);
    ASSERT_NE(u, nullptr);
    EXPECT_STREQ(u->username, "alice");
    EXPECT_EQ(u->gid, 1);

    // Verify alice can login
    um.logout();
    EXPECT_EQ(um.login("alice", "pass1"), 1);
}

TEST_F(UserManagerTest, NonRootCannotAddUser) {
    um.login("root", "root");
    um.add_user("alice", "pass1", 1, 1);
    um.logout();

    um.login("alice", "pass1");
    EXPECT_EQ(um.add_user("bob", "pass2", 2, 2), -1);
}

TEST_F(UserManagerTest, AddUserDuplicateNameFails) {
    um.login("root", "root");
    um.add_user("alice", "p", 1, 1);
    EXPECT_EQ(um.add_user("alice", "p2", 2, 2), -1);
}

TEST_F(UserManagerTest, AddUserTableFull) {
    um.login("root", "root");
    // MAX_USER = 8, root occupies slot 0
    for (int i = 1; i < MAX_USER; ++i) {
        char name[PWDSIZ];
        std::snprintf(name, PWDSIZ, "u%d", i);
        EXPECT_EQ(um.add_user(name, "p", i, i), 0);
    }
    EXPECT_EQ(um.add_user("extra", "p", 99, 99), -1);
}

TEST_F(UserManagerTest, AddUserNotLoggedIn) {
    EXPECT_EQ(um.add_user("alice", "p", 1, 1), -1);
}

// --- Change Password ---

TEST_F(UserManagerTest, ChangeOwnPassword) {
    um.login("root", "root");
    EXPECT_EQ(um.change_password(0, "root", "newroot"), 0);
    um.logout();
    EXPECT_EQ(um.login("root", "root"), -1);
    EXPECT_EQ(um.login("root", "newroot"), 0);
}

TEST_F(UserManagerTest, ChangePasswordWrongOld) {
    um.login("root", "root");
    EXPECT_EQ(um.change_password(0, "wrong", "new"), -1);
    um.logout();
    EXPECT_EQ(um.login("root", "root"), 0);  // unchanged
}

TEST_F(UserManagerTest, RootCanChangeOtherPassword) {
    um.login("root", "root");
    um.add_user("alice", "oldpw", 1, 1);
    // Root changes alice's password (old_pw bypassed for root)
    EXPECT_EQ(um.change_password(1, "", "newpw"), 0);

    // Verify alice can login with new password
    um.logout();
    EXPECT_EQ(um.login("alice", "newpw"), 1);
}

TEST_F(UserManagerTest, NonRootCannotChangeOtherPassword) {
    um.login("root", "root");
    um.add_user("alice", "apw", 1, 1);
    um.add_user("bob", "bpw", 2, 2);
    um.logout();

    um.login("alice", "apw");
    EXPECT_EQ(um.change_password(2, "bpw", "newbpw"), -1);
}

TEST_F(UserManagerTest, ChangePasswordNotLoggedIn) {
    EXPECT_EQ(um.change_password(0, "root", "new"), -1);
}

// --- Permission Check ---

TEST_F(UserManagerTest, RootBypassesPermissions) {
    EXPECT_TRUE(um.check_access(0, 5, 5, 0000, ACCESS_READ));
    EXPECT_TRUE(um.check_access(0, 5, 5, 0000, ACCESS_WRITE));
    EXPECT_TRUE(um.check_access(0, 5, 5, 0000, ACCESS_EXEC));
}

TEST_F(UserManagerTest, OwnerPermission) {
    // File owned by uid=1, mode rwx------
    uint16_t mode = PERM_UR | PERM_UW | PERM_UX;
    EXPECT_TRUE(um.check_access(1, 1, 99, mode, ACCESS_READ));
    EXPECT_TRUE(um.check_access(1, 1, 99, mode, ACCESS_WRITE));
    EXPECT_TRUE(um.check_access(1, 1, 99, mode, ACCESS_EXEC));
}

TEST_F(UserManagerTest, OwnerDenied) {
    // File owned by uid=1, mode r-x------
    uint16_t mode = PERM_UR | PERM_UX;
    EXPECT_TRUE(um.check_access(1, 1, 99, mode, ACCESS_READ));
    EXPECT_FALSE(um.check_access(1, 1, 99, mode, ACCESS_WRITE));
}

TEST_F(UserManagerTest, GroupPermission) {
    // Create user uid=2, gid=10 for group access testing
    um.login("root", "root");
    um.add_user("groupuser", "p", 2, 10);
    um.logout();

    // uid=2, gid=10. File uid=1, gid=10, mode ---rwx---
    uint16_t mode = PERM_GR | PERM_GW | PERM_GX;
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_READ));
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_WRITE));
}

TEST_F(UserManagerTest, GroupDenied) {
    um.login("root", "root");
    um.add_user("groupuser", "p", 2, 10);
    um.logout();

    // uid=2, gid=10. File uid=1, gid=10, mode ---r-x---
    uint16_t mode = PERM_GR | PERM_GX;
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_READ));
    EXPECT_FALSE(um.check_access(2, 1, 10, mode, ACCESS_WRITE));
}

TEST_F(UserManagerTest, OtherPermission) {
    // uid=2, gid=20. File uid=1, gid=10, mode ------rwx
    uint16_t mode = PERM_OR | PERM_OW | PERM_OX;
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_READ));
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_WRITE));
}

TEST_F(UserManagerTest, OtherDenied) {
    // uid=2, gid=20. File uid=1, gid=10, mode ------r-x
    uint16_t mode = PERM_OR | PERM_OX;
    EXPECT_TRUE(um.check_access(2, 1, 10, mode, ACCESS_READ));
    EXPECT_FALSE(um.check_access(2, 1, 10, mode, ACCESS_WRITE));
}

TEST_F(UserManagerTest, NoAccessZeroMode) {
    EXPECT_FALSE(um.check_access(1, 1, 99, 0000, ACCESS_READ));
    EXPECT_FALSE(um.check_access(2, 1, 10, 0000, ACCESS_READ));
}

// --- su ---

TEST_F(UserManagerTest, SuByRoot) {
    um.login("root", "root");
    um.add_user("alice", "pw", 1, 1);
    EXPECT_EQ(um.su(1, nullptr), 0);  // root needs no password
    EXPECT_EQ(um.current_uid(), 1);
    EXPECT_EQ(um.current_username(), "alice");
}

TEST_F(UserManagerTest, SuByNonRootWithPassword) {
    um.login("root", "root");
    um.add_user("alice", "alicepw", 1, 1);
    um.add_user("bob", "bobpw", 2, 2);
    um.logout();
    um.login("alice", "alicepw");
    EXPECT_EQ(um.su(2, "bobpw"), 0);
    EXPECT_EQ(um.current_uid(), 2);
}

TEST_F(UserManagerTest, SuByNonRootWrongPassword) {
    um.login("root", "root");
    um.add_user("alice", "apw", 1, 1);
    um.add_user("bob", "bpw", 2, 2);
    um.logout();
    um.login("alice", "apw");
    EXPECT_EQ(um.su(2, "wrong"), -1);
    EXPECT_EQ(um.current_uid(), 1);  // still alice
}

TEST_F(UserManagerTest, SuByNonRootNoPassword) {
    um.login("root", "root");
    um.add_user("alice", "apw", 1, 1);
    um.add_user("bob", "bpw", 2, 2);
    um.logout();
    um.login("alice", "apw");
    EXPECT_EQ(um.su(2, nullptr), -1);  // non-root needs password
}

TEST_F(UserManagerTest, SuInvalidUser) {
    um.login("root", "root");
    EXPECT_EQ(um.su(99, nullptr), -1);
}

TEST_F(UserManagerTest, SuNotLoggedIn) {
    EXPECT_EQ(um.su(0, nullptr), -1);
}

TEST_F(UserManagerTest, FindUserByName) {
    um.login("root", "root");
    um.add_user("charlie", "p", 3, 3);
    auto* u = um.find_user("charlie");
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->uid, 3);
    EXPECT_EQ(um.find_user("nobody"), nullptr);
}

}  // namespace
}  // namespace pfs
