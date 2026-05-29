#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pfs {

class IFileSystem;
class UserManager;

using CmdHandler = std::function<int(IFileSystem& fs, UserManager& um,
                                     const std::vector<std::string>& args, std::string& output)>;

class CommandRegistry {
   public:
    void register_cmd(const std::string& name, CmdHandler handler, const std::string& usage);

    int execute(const std::string& cmdline, IFileSystem& fs, UserManager& um, std::string& output);

    std::vector<std::pair<std::string, std::string>> list_commands() const;

    static std::vector<std::string> tokenize(const std::string& cmdline);

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
    static std::vector<std::string> expand_globs(IFileSystem& fs,
                                                  const std::vector<Token>& tokens);

    // Sorted by insertion order, not alphabetically
    std::vector<std::pair<std::string, CmdEntry>> commands_;
};

}  // namespace pfs
