#include "shell/command_registry.h"

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

    for (const auto& [name, entry] : commands_) {
        if (name == cmd_name) {
            return entry.handler(fs, um, args, output);
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
