#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "core/user_manager.h"
#include "shell/command_registry.h"
#include "stub_fs.h"

namespace pfs {
namespace {

// --- Tokenize tests ---

TEST(TokenizeTest, EmptyString) {
    auto tokens = CommandRegistry::tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(TokenizeTest, WhitespaceOnly) {
    auto tokens = CommandRegistry::tokenize("   \t  ");
    EXPECT_TRUE(tokens.empty());
}

TEST(TokenizeTest, SingleToken) {
    auto tokens = CommandRegistry::tokenize("hello");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello");
}

TEST(TokenizeTest, MultipleTokens) {
    auto tokens = CommandRegistry::tokenize("mkdir -p a/b/c");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "mkdir");
    EXPECT_EQ(tokens[1], "-p");
    EXPECT_EQ(tokens[2], "a/b/c");
}

TEST(TokenizeTest, QuotedString) {
    auto tokens = CommandRegistry::tokenize("write \"hello world\"");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "write");
    EXPECT_EQ(tokens[1], "hello world");
}

TEST(TokenizeTest, QuotedStringWithEscape) {
    auto tokens = CommandRegistry::tokenize("echo \"a\\\"b\"");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "echo");
    EXPECT_EQ(tokens[1], "a\"b");
}

TEST(TokenizeTest, MixedQuotedAndUnquoted) {
    auto tokens = CommandRegistry::tokenize("cp \"source file\" dest");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "cp");
    EXPECT_EQ(tokens[1], "source file");
    EXPECT_EQ(tokens[2], "dest");
}

TEST(TokenizeTest, LeadingTrailingWhitespace) {
    auto tokens = CommandRegistry::tokenize("  ls -la  ");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "ls");
    EXPECT_EQ(tokens[1], "-la");
}

TEST(TokenizeTest, MultipleSpacesBetweenTokens) {
    auto tokens = CommandRegistry::tokenize("a   b\t\tc");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
    EXPECT_EQ(tokens[2], "c");
}

TEST(TokenizeTest, UnclosedQuoteTakesRest) {
    auto tokens = CommandRegistry::tokenize("echo \"unclosed");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "echo");
    EXPECT_EQ(tokens[1], "unclosed");
}

// --- Registration tests ---

TEST(CommandRegistryTest, ListCommandsEmpty) {
    CommandRegistry reg;
    auto cmds = reg.list_commands();
    EXPECT_TRUE(cmds.empty());
}

TEST(CommandRegistryTest, RegisterAndList) {
    CommandRegistry reg;
    reg.register_cmd("ls", nullptr, "ls — list directory");
    reg.register_cmd("mkdir", nullptr, "mkdir <path> — create directory");

    auto cmds = reg.list_commands();
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0].first, "ls");
    EXPECT_EQ(cmds[0].second, "ls — list directory");
    EXPECT_EQ(cmds[1].first, "mkdir");
    EXPECT_EQ(cmds[1].second, "mkdir <path> — create directory");
}

// --- Execute tests ---

TEST(CommandRegistryTest, ExecuteUnknownCommand) {
    CommandRegistry reg;
    StubFs fs;
    UserManager um;
    std::string output;

    int ret = reg.execute("foobar", fs, um, output);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(output, "Unknown command: foobar");
}

TEST(CommandRegistryTest, ExecuteCallsRegisteredHandler) {
    CommandRegistry reg;
    reg.register_cmd(
        "greet",
        [](IFileSystem&, UserManager&, const std::vector<std::string>& args,
           std::string& output) -> int {
            output = "Hello, " + (args.empty() ? "world" : args[0]) + "!";
            return 42;
        },
        "greet [name]");

    StubFs fs;
    UserManager um;
    std::string output;
    int ret = reg.execute("greet Alice", fs, um, output);
    EXPECT_EQ(ret, 42);
    EXPECT_EQ(output, "Hello, Alice!");
}

TEST(CommandRegistryTest, ExecuteEmptyCommandLine) {
    CommandRegistry reg;
    StubFs fs;
    UserManager um;
    std::string output;
    int ret = reg.execute("   ", fs, um, output);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(output.empty());
}

// Regression: a handler that throws (e.g. std::stoi on a non-numeric arg like
// "close abc") must not propagate out of execute() and terminate the process.
// Previously this aborted the whole program — in the TUI that silently dropped
// the user out of the interface.
TEST(CommandRegistryTest, ExecuteCatchesInvalidArgument) {
    CommandRegistry reg;
    reg.register_cmd(
        "needsnum",
        [](IFileSystem&, UserManager&, const std::vector<std::string>& args,
           std::string& output) -> int {
            output = std::to_string(std::stoi(args.at(0)));
            return 0;
        },
        "needsnum <n>");

    StubFs fs;
    UserManager um;
    std::string output;
    int ret = reg.execute("needsnum abc", fs, um, output);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(output, "needsnum: invalid numeric argument");
}

TEST(CommandRegistryTest, ExecuteCatchesGenericException) {
    CommandRegistry reg;
    reg.register_cmd(
        "boom",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&,
           std::string&) -> int { throw std::runtime_error("kaboom"); },
        "boom");

    StubFs fs;
    UserManager um;
    std::string output;
    int ret = reg.execute("boom", fs, um, output);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(output, "boom: error: kaboom");
}

}  // namespace
}  // namespace pfs
