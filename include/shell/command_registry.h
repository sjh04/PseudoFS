#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pfs {

// A command may prefix its output with PAGER_PREFIX to ask the caller (the TUI
// or CLI loop) to show the rest through a pager, one screen at a time, instead
// of dumping it all at once. The leading control byte never occurs in normal
// text or filenames, so it cannot collide with real file content. Used by
// `more`.
constexpr char PAGER_PREFIX[] = "\x01PFS_PAGE\x01";

class IFileSystem;
class UserManager;

using CmdHandler = std::function<int(IFileSystem& fs, UserManager& um,
                                     const std::vector<std::string>& args, std::string& output)>;

// One recorded operation in the command log (C-05).
struct LogEntry {
    uint32_t time;        // unix timestamp
    std::string user;     // who ran it
    std::string cmdline;  // the exact command line (replayable verbatim)
};

class CommandRegistry {
   public:
    void register_cmd(const std::string& name, CmdHandler handler, const std::string& usage);

    int execute(const std::string& cmdline, IFileSystem& fs, UserManager& um, std::string& output);

    std::vector<std::pair<std::string, std::string>> list_commands() const;

    static std::vector<std::string> tokenize(const std::string& cmdline);

    // --- Operation log / replay (C-05) ---

    // Enable persistent logging: load any existing entries from `path` and
    // append new ones there as commands run.
    void set_log_path(const std::string& path);

    // The recorded operations, oldest first.
    const std::vector<LogEntry>& op_log() const {
        return log_;
    }

    // Forget all entries (and truncate the log file if one is set).
    void clear_log();

    // Re-run every logged operation in order against `fs`/`um`. Replayed
    // commands are not themselves logged. Returns 0; writes a summary to out.
    int replay(IFileSystem& fs, UserManager& um, std::string& out);

   private:
    struct CmdEntry {
        CmdHandler handler;
        std::string usage;
    };

    // A command-line token plus whether it was double-quoted. Quoted tokens are
    // exempt from glob expansion (so `write 0 "*"` writes a literal asterisk).
    struct Token {
        std::string text;
        bool quoted;
    };

    // Like tokenize(), but also reports per-token quoting. tokenize() delegates
    // here and drops the flags.
    static std::vector<Token> tokenize_ex(const std::string& cmdline);

    // Expand shell wildcards (`*`, `?`) in `tokens` against `fs`, returning the
    // resulting argument list. An unquoted token with metacharacters is
    // replaced by the sorted set of matching paths; if nothing matches it is
    // left literal (nullglob off, matching common shell behavior).
    static std::vector<std::string> expand_globs(IFileSystem& fs, const std::vector<Token>& tokens);

    // Append one operation to the log (in memory + file), unless logging is
    // suppressed (during replay) or the command is a meta-command.
    void record(const std::string& cmd_name, const std::string& cmdline, UserManager& um);

    // Sorted by insertion order, not alphabetically
    std::vector<std::pair<std::string, CmdEntry>> commands_;

    std::vector<LogEntry> log_;
    std::string log_path_;
    bool suppress_log_ = false;  // true while replay() is running
};

}  // namespace pfs
