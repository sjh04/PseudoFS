#include "shell/command_registry.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/user_manager.h"
#include "core/vfs.h"

namespace pfs {

namespace {

// True if `s` contains a glob metacharacter worth expanding.
bool has_glob(const std::string& s) {
    return s.find_first_of("*?") != std::string::npos;
}

// Classic linear glob matcher supporting '*' (any run) and '?' (one char).
// Matches the whole string (anchored), like a single path component.
bool glob_match(const char* p, const char* s) {
    const char* star = nullptr;
    const char* star_s = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) {
            ++p;
            ++s;
        } else if (*p == '*') {
            star = p++;       // remember the '*' and the position to backtrack to
            star_s = s;
        } else if (star != nullptr) {
            p = star + 1;     // last '*' absorbs one more char of s
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

}  // namespace

void CommandRegistry::register_cmd(const std::string& name, CmdHandler handler,
                                   const std::string& usage) {
    CmdEntry entry;
    entry.handler = std::move(handler);
    entry.usage = usage;
    commands_.emplace_back(name, std::move(entry));
}

int CommandRegistry::execute(const std::string& cmdline, IFileSystem& fs, UserManager& um,
                             std::string& output) {
    std::vector<Token> tokens = tokenize_ex(cmdline);
    if (tokens.empty()) {
        return 0;
    }

    std::string cmd_name = tokens[0].text;
    tokens.erase(tokens.begin());

    // Drive the FS permission layer from the logged-in user, so every command
    // runs with the current user's identity (login/su/logout take effect on the
    // next command). Without this the engine stays at its default uid (root) and
    // rwx checks never fire. Covers CLI, TUI, and post-F2-switch dispatch.
    fs.set_user(um.current_uid(), um.current_gid());

    // Shell-style wildcard expansion happens after set_user (so directory
    // listings respect the current user) and on argument tokens only — never on
    // the command name.
    std::vector<std::string> args = expand_globs(fs, tokens);

    for (const auto& [name, entry] : commands_) {
        if (name == cmd_name) {
            // Safety net: a handler must never crash the whole program. Several
            // commands parse numeric args with std::stoi/stoul, which throw on
            // non-numeric input (e.g. "close abc", "su alice" for an unknown
            // user). An uncaught throw here would propagate out of the TUI/CLI
            // loop and std::terminate() the process — in the TUI that silently
            // drops the user out of the interface. Convert any handler
            // exception into a normal error result instead.
            try {
                return entry.handler(fs, um, args, output);
            } catch (const std::invalid_argument&) {
                output = cmd_name + ": invalid numeric argument";
                return -1;
            } catch (const std::out_of_range&) {
                output = cmd_name + ": numeric argument out of range";
                return -1;
            } catch (const std::exception& e) {
                output = cmd_name + ": error: " + e.what();
                return -1;
            }
        }
    }

    output = "Unknown command: " + cmd_name;
    return -1;
}

std::vector<std::pair<std::string, std::string>> CommandRegistry::list_commands() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(commands_.size());
    for (const auto& [name, entry] : commands_) {
        result.emplace_back(name, entry.usage);
    }
    return result;
}

std::vector<CommandRegistry::Token> CommandRegistry::tokenize_ex(const std::string& cmdline) {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t n = cmdline.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && (cmdline[i] == ' ' || cmdline[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        Token tok;
        tok.quoted = false;

        if (cmdline[i] == '"') {
            // Quoted string — collect until closing quote. Mark it quoted so
            // wildcard expansion skips it.
            tok.quoted = true;
            ++i;  // skip opening quote
            while (i < n && cmdline[i] != '"') {
                if (cmdline[i] == '\\' && i + 1 < n) {
                    ++i;  // skip backslash, take next char literally
                }
                tok.text.push_back(cmdline[i]);
                ++i;
            }
            if (i < n) ++i;  // skip closing quote
        } else {
            // Unquoted token — collect until whitespace
            while (i < n && cmdline[i] != ' ' && cmdline[i] != '\t') {
                tok.text.push_back(cmdline[i]);
                ++i;
            }
        }

        tokens.push_back(std::move(tok));
    }

    return tokens;
}

std::vector<std::string> CommandRegistry::tokenize(const std::string& cmdline) {
    std::vector<std::string> out;
    for (auto& tok : tokenize_ex(cmdline)) {
        out.push_back(std::move(tok.text));
    }
    return out;
}

std::vector<std::string> CommandRegistry::expand_globs(IFileSystem& fs,
                                                       const std::vector<Token>& tokens) {
    std::vector<std::string> out;
    for (const auto& tok : tokens) {
        if (tok.quoted || !has_glob(tok.text)) {
            out.push_back(tok.text);
            continue;
        }

        // Only the last path component is treated as a pattern; the directory
        // prefix (if any) is taken literally. So "*.txt" globs in the cwd and
        // "docs/*.log" globs inside docs/, but "a*/b" is not expanded.
        size_t slash = tok.text.rfind('/');
        std::string dir = (slash == std::string::npos) ? std::string()
                                                        : tok.text.substr(0, slash);
        std::string pat =
            (slash == std::string::npos) ? tok.text : tok.text.substr(slash + 1);

        std::vector<DirEntry> entries;
        std::vector<std::string> matches;
        if (fs.fs_ls(dir.c_str(), entries) == 0) {
            for (const auto& e : entries) {
                if (std::strcmp(e.name, ".") == 0 || std::strcmp(e.name, "..") == 0) {
                    continue;
                }
                if (glob_match(pat.c_str(), e.name)) {
                    matches.push_back(dir.empty() ? std::string(e.name)
                                                  : dir + "/" + e.name);
                }
            }
        }

        if (matches.empty()) {
            out.push_back(tok.text);  // nullglob off: no match → literal pattern
        } else {
            std::sort(matches.begin(), matches.end());
            for (auto& m : matches) out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace pfs
