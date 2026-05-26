#include <gtest/gtest.h>

#include "shell/command_registry.h"

namespace pfs {
namespace {

// Simulate a mini shell session: register commands and run them in sequence.
// Uses the same buffer-cast pattern as the unit tests since IFileSystem and
// UserManager are only forward-declared.

alignas(64) char buf[512];
IFileSystem& fs = *static_cast<IFileSystem*>(static_cast<void*>(buf));
UserManager& um = *static_cast<UserManager*>(static_cast<void*>(buf + 256));

TEST(Integration, FullShellSession) {
    CommandRegistry reg;

    int call_count = 0;
    std::string last_output;

    reg.register_cmd(
        "login",
        [&](IFileSystem&, UserManager&, const std::vector<std::string>& args,
            std::string& output) -> int {
            call_count++;
            output = "Welcome, " + (args.empty() ? "user" : args[0]);
            return 0;
        },
        "login <username> <password>");

    reg.register_cmd(
        "mkdir",
        [&](IFileSystem&, UserManager&, const std::vector<std::string>& args,
            std::string& output) -> int {
            call_count++;
            output = "Created: " + (args.empty() ? "?" : args[0]);
            return 0;
        },
        "mkdir <path>");

    reg.register_cmd(
        "ls",
        [&](IFileSystem&, UserManager&, const std::vector<std::string>&,
            std::string& output) -> int {
            call_count++;
            output = "dir1/  file1.txt  file2.txt";
            return 0;
        },
        "ls");

    reg.register_cmd(
        "logout",
        [&](IFileSystem&, UserManager&, const std::vector<std::string>&,
            std::string& output) -> int {
            call_count++;
            output = "Goodbye";
            return 0;
        },
        "logout");

    // Verify all commands are registered
    auto cmds = reg.list_commands();
    ASSERT_EQ(cmds.size(), 4u);

    // Simulate a user session
    std::string out;

    // login
    EXPECT_EQ(reg.execute("login root password", fs, um, out), 0);
    EXPECT_EQ(out, "Welcome, root");
    last_output = out;

    // mkdir
    EXPECT_EQ(reg.execute("mkdir /home/test", fs, um, out), 0);
    EXPECT_EQ(out, "Created: /home/test");

    // ls
    EXPECT_EQ(reg.execute("ls", fs, um, out), 0);
    // out should have changed from mkdir's output
    EXPECT_NE(out, last_output);

    // bad command
    EXPECT_EQ(reg.execute("delete_everything", fs, um, out), -1);
    EXPECT_EQ(out, "Unknown command: delete_everything");

    // logout
    EXPECT_EQ(reg.execute("logout", fs, um, out), 0);
    EXPECT_EQ(out, "Goodbye");

    // All 4 registered commands were each called once
    EXPECT_EQ(call_count, 4);
}

TEST(Integration, CommandAliasViaSecondRegistration) {
    CommandRegistry reg;

    reg.register_cmd(
        "ll",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&,
           std::string& output) -> int {
            output = "detailed list";
            return 0;
        },
        "ll — detailed list");

    reg.register_cmd(
        "ls",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&,
           std::string& output) -> int {
            output = "short list";
            return 0;
        },
        "ls — short list");

    std::string out;
    EXPECT_EQ(reg.execute("ll", fs, um, out), 0);
    EXPECT_EQ(out, "detailed list");
    EXPECT_EQ(reg.execute("ls", fs, um, out), 0);
    EXPECT_EQ(out, "short list");
}

TEST(Integration, HandlerReturnsErrorCodes) {
    CommandRegistry reg;

    reg.register_cmd(
        "risky",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&,
           std::string& output) -> int {
            output = "Something went wrong";
            return -5;
        },
        "risky — may fail");

    std::string out;
    int ret = reg.execute("risky", fs, um, out);
    EXPECT_EQ(ret, -5);
    EXPECT_EQ(out, "Something went wrong");
}

}  // namespace
}  // namespace pfs
