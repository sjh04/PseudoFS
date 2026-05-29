#include <gtest/gtest.h>

#include <cstring>
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

// --- Per-command help (-h / --help) ---

TEST(CommandRegistryTest, DashHPrintsUsageWithoutRunning) {
    CommandRegistry reg;
    bool ran = false;
    reg.register_cmd(
        "greet",
        [&ran](IFileSystem&, UserManager&, const std::vector<std::string>&,
               std::string& out) -> int {
            ran = true;
            out = "hello";
            return 0;
        },
        "greet [name]");

    StubFs fs;
    UserManager um;
    std::string out;

    EXPECT_EQ(reg.execute("greet -h", fs, um, out), 0);
    EXPECT_EQ(out, "Usage: greet [name]");
    EXPECT_FALSE(ran);  // help short-circuits the handler

    out.clear();
    EXPECT_EQ(reg.execute("greet --help", fs, um, out), 0);
    EXPECT_EQ(out, "Usage: greet [name]");
    EXPECT_FALSE(ran);
}

// --- Wildcard (glob) expansion ---

// A stub whose directory always contains the same three entries, returned out
// of alphabetical order so the sort in expansion is exercised.
class GlobFs : public StubFs {
   public:
    int fs_ls(const char*, std::vector<DirEntry>& out) override {
        out.clear();
        const char* names[] = {"b.txt", "c.log", "a.txt"};
        for (const char* n : names) {
            DirEntry e{};
            std::strncpy(e.name, n, sizeof(e.name) - 1);
            e.type = TYPE_FILE;
            out.push_back(e);
        }
        return 0;
    }
};

// Run `line` and return the argument list the (only) command actually received.
static std::vector<std::string> captured_args(const std::string& line) {
    CommandRegistry reg;
    std::vector<std::string> captured;
    reg.register_cmd(
        "cmd",
        [&captured](IFileSystem&, UserManager&, const std::vector<std::string>& args,
                    std::string&) -> int {
            captured = args;
            return 0;
        },
        "cmd");
    GlobFs fs;
    UserManager um;
    std::string out;
    reg.execute(line, fs, um, out);
    return captured;
}

TEST(GlobTest, StarExpandsAndSorts) {
    auto a = captured_args("cmd *.txt");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0], "a.txt");  // sorted, though the dir returned b.txt first
    EXPECT_EQ(a[1], "b.txt");
}

TEST(GlobTest, StarMatchesSingleExtension) {
    auto a = captured_args("cmd *.log");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], "c.log");
}

TEST(GlobTest, QuestionMarkMatchesOneChar) {
    auto a = captured_args("cmd ?.txt");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0], "a.txt");
    EXPECT_EQ(a[1], "b.txt");
}

TEST(GlobTest, QuotedPatternIsNotExpanded) {
    auto a = captured_args("cmd \"*.txt\"");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], "*.txt");  // quotes exempt it from globbing
}

TEST(GlobTest, NoMatchKeepsLiteralPattern) {
    auto a = captured_args("cmd *.none");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], "*.none");  // nullglob off: literal pattern passes through
}

TEST(GlobTest, PlainArgUnchanged) {
    auto a = captured_args("cmd hello");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], "hello");
}

TEST(GlobTest, DirPrefixPreserved) {
    auto a = captured_args("cmd sub/*.txt");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0], "sub/a.txt");  // matches are re-prefixed with the dir part
    EXPECT_EQ(a[1], "sub/b.txt");
}

}  // namespace
}  // namespace pfs
