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

    // Sorted by insertion order, not alphabetically
    std::vector<std::pair<std::string, CmdEntry>> commands_;
};

}  // namespace pfs
