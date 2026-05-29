#include "shell/command_registry.h"

#include <stdexcept>

#include "core/user_manager.h"
#include "core/vfs.h"

namespace pfs {

void CommandRegistry::register_cmd(const std::string& name, CmdHandler handler,
                                   const std::string& usage) {
    CmdEntry entry;
    entry.handler = std::move(handler);
    entry.usage = usage;
    commands_.emplace_back(name, std::move(entry));
}

int CommandRegistry::execute(const std::string& cmdline, IFileSystem& fs, UserManager& um,
                             std::string& output) {
    std::vector<std::string> args = tokenize(cmdline);
    if (args.empty()) {
        return 0;
    }

    std::string cmd_name = args[0];
    args.erase(args.begin());

    // Drive the FS permission layer from the logged-in user, so every command
    // runs with the current user's identity (login/su/logout take effect on the
    // next command). Without this the engine stays at its default uid (root) and
    // rwx checks never fire. Covers CLI, TUI, and post-F2-switch dispatch.
    fs.set_user(um.current_uid(), um.current_gid());

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

std::vector<std::string> CommandRegistry::tokenize(const std::string& cmdline) {
    std::vector<std::string> tokens;
    size_t i = 0;
    const size_t n = cmdline.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && (cmdline[i] == ' ' || cmdline[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        std::string token;

        if (cmdline[i] == '"') {
            // Quoted string — collect until closing quote
            ++i;  // skip opening quote
            while (i < n && cmdline[i] != '"') {
                if (cmdline[i] == '\\' && i + 1 < n) {
                    ++i;  // skip backslash, take next char literally
                }
                token.push_back(cmdline[i]);
                ++i;
            }
            if (i < n) ++i;  // skip closing quote
        } else {
            // Unquoted token — collect until whitespace
            while (i < n && cmdline[i] != ' ' && cmdline[i] != '\t') {
                token.push_back(cmdline[i]);
                ++i;
            }
        }

        tokens.push_back(std::move(token));
    }

    return tokens;
}

}  // namespace pfs
