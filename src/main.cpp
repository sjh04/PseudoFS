#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/vfs.h"
#include "fs/fat16/fat16_fs.h"
#include "fs/unix/unix_fs.h"
#include "core/user_manager.h"
#include "shell/command_registry.h"
#include "tui/tui.h"

using namespace pfs;

static const char* UNIX_DISK = "pfs_unix.img";
static const char* FAT16_DISK = "pfs_fat16.img";
// User accounts live outside the FS image so a single table is shared across
// both engines and both (CLI / TUI) modes.
static const char* USERS_FILE = "pfs_users.dat";

// Explain a failed open: fs_open returns -1 for both "missing" and "denied".
// If the path still resolves via stat, it exists and the open was blocked by a
// permission check; otherwise it really isn't there. Lets commands report
// "permission denied" instead of a misleading "file not found".
static std::string open_error(IFileSystem& fs, const std::string& path) {
    FileStat st{};
    if (fs.fs_stat(path.c_str(), st) != 0) return "no such file";
    // The path exists. If it is a symlink, the open most likely failed because
    // its target is unresolvable — distinguish that from a real permission
    // denial (fs_stat is lstat-style, so statting the target tells us).
    if (st.type == TYPE_SYMLINK) {
        std::string target;
        FileStat tst{};
        if (fs.fs_readlink(path.c_str(), target) == 0 &&
            fs.fs_stat(target.c_str(), tst) != 0) {
            return "broken symbolic link";
        }
    }
    return "permission denied";
}

// Join a directory path and an entry name. "" means the cwd (so the name is
// used bare, resolving against the cwd).
static std::string join_path(const std::string& base, const char* name) {
    if (base.empty()) return std::string(name);
    if (base == "/") return "/" + std::string(name);
    return base + "/" + std::string(name);
}

// Append one `ll` detail line for an entry to `out`:
//   <type+rwx>  <nlink>  <owner>  <size>  <mtime>  <display>[/ | @ -> target]
// `full` is the path used for stat/readlink; `display` is the text shown.
static void append_ll_line(IFileSystem& fs, UserManager& um,
                           const std::string& full, const std::string& display,
                           uint8_t type, std::string& out) {
    FileStat st{};
    fs.fs_stat(full.c_str(), st);

    // Permission column: type char + rwx triplets, e.g. drwxr-xr-x.
    char perm[11];
    perm[0] = (type == TYPE_DIR) ? 'd' : (type == TYPE_SYMLINK) ? 'l' : '-';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        perm[i + 1] = (st.mode & (1 << (8 - i))) ? rwx[i] : '-';
    perm[10] = '\0';

    // Owner: username if known, else the raw uid.
    char owner[16];
    const UserRecord* u = um.find_user(st.uid);
    if (u != nullptr)
        std::snprintf(owner, sizeof(owner), "%s", u->username);
    else
        std::snprintf(owner, sizeof(owner), "%u", st.uid);

    // Modification time as "MM-DD HH:MM".
    char when[16] = "-";
    std::time_t t = static_cast<std::time_t>(st.mtime);
    std::tm* lt = std::localtime(&t);
    if (lt) std::strftime(when, sizeof(when), "%m-%d %H:%M", lt);

    // Name suffix: "/" for dirs, "@ -> target" for symlinks.
    std::string suffix;
    if (type == TYPE_DIR) {
        suffix = "/";
    } else if (type == TYPE_SYMLINK) {
        std::string tgt;
        suffix = (fs.fs_readlink(full.c_str(), tgt) == 0) ? "@ -> " + tgt : "@";
    }

    // Fixed-width prefix via snprintf; append name+suffix as a string so a long
    // symlink target is never truncated.
    char head[64];
    std::snprintf(head, sizeof(head), "%s %2u %-8s %6u  %s  ", perm, st.nlink,
                  owner, st.size, when);
    out += head;
    out += display;
    out += suffix;
    out += "\n";
}

static void register_commands(CommandRegistry& reg) {
    reg.register_cmd(
        "login",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: login <username> <password>";
                return -1;
            }
            int uid = um.login(args[0].c_str(), args[1].c_str());
            if (uid < 0) {
                out = "Login failed.";
                return -1;
            }
            out = "Welcome, " + um.current_username() +
                  " (uid=" + std::to_string(uid) + ")";
            return 0;
        },
        "login <user> <pw> — log in");

    reg.register_cmd(
        "logout",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>&,
           std::string& out) -> int {
            if (!um.is_logged_in()) {
                out = "Not logged in.";
                return -1;
            }
            um.logout();
            out = "Logged out.";
            return 0;
        },
        "logout — log out");

    reg.register_cmd(
        "useradd",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 4) {
                out = "Usage: useradd <username> <password> <uid> <gid>";
                return -1;
            }
            uint16_t uid = static_cast<uint16_t>(std::stoi(args[2]));
            uint16_t gid = static_cast<uint16_t>(std::stoi(args[3]));
            int ret = um.add_user(args[0].c_str(), args[1].c_str(), uid, gid);
            if (ret != 0) {
                out = "useradd: failed (need root, unique name, and free slot)";
                return -1;
            }
            // Create home directory for the new user. /home stays root-owned;
            // the per-user dir is created as the new user so they own it (and
            // can actually write in it after logging in).
            std::string home = "/home/" + std::string(args[0]);
            fs.fs_mkdir("/home");
            fs.set_user(uid, gid);
            fs.fs_mkdir(home.c_str());
            fs.set_user(um.current_uid(), um.current_gid());  // restore to root
            out = "User " + std::string(args[0]) + " created.";
            return 0;
        },
        "useradd <user> <pw> <uid> <gid> — create user (root only)");

    reg.register_cmd(
        "passwd",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: passwd <old-password> <new-password>";
                return -1;
            }
            int ret =
                um.change_password(um.current_uid(), args[0].c_str(), args[1].c_str());
            if (ret != 0) {
                out = "passwd: failed (check old password)";
                return -1;
            }
            out = "Password changed.";
            return 0;
        },
        "passwd <old> <new> — change password");

    reg.register_cmd(
        "format",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            int ret = fs.fs_format();
            out = (ret == 0) ? "Filesystem formatted." : "Format failed.";
            return ret;
        },
        "format — format the virtual disk");

    reg.register_cmd(
        "mkdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            bool mkdir_p = false;
            std::string path;
            for (auto& a : args) {
                if (a == "-p") mkdir_p = true;
                else path = a;
            }
            if (path.empty()) {
                out = "Usage: mkdir [-p] <path>";
                return -1;
            }
            if (!mkdir_p) {
                int ret = fs.fs_mkdir(path.c_str());
                if (ret != 0) out = "mkdir: failed";
                return ret;
            }
            // -p: create each level
            std::string cur;
            size_t pos = 0;
            if (path[0] == '/') { cur = "/"; pos = 1; }
            while (pos < path.size()) {
                size_t slash = path.find('/', pos);
                if (slash == std::string::npos) slash = path.size();
                if (slash > pos) {
                    cur += path.substr(pos, slash - pos);
                    fs.fs_mkdir(cur.c_str());
                    cur += "/";
                }
                pos = slash + 1;
            }
            // Final check: does the full path exist now?
            FileStat st;
            if (fs.fs_stat(path.c_str(), st) == 0) return 0;
            out = "mkdir: failed";
            return -1;
        },
        "mkdir [-p] <path> — create directory");

    reg.register_cmd(
        "su",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: su <uid|username> [password]";
                return -1;
            }
            // Resolve the target: try by name first, then as a numeric uid.
            // Only attempt the uid parse when the arg is actually all-digits —
            // otherwise std::stoi on a bad name (e.g. "su alice") would throw,
            // surfacing a confusing "invalid numeric argument" instead of the
            // real problem.
            const UserRecord* u = um.find_user(args[0].c_str());
            if (u == nullptr) {
                const std::string& who = args[0];
                bool numeric = !who.empty() &&
                               who.find_first_not_of("0123456789") ==
                                   std::string::npos;
                if (numeric) {
                    u = um.find_user(static_cast<uint16_t>(std::stoi(who)));
                }
            }
            if (u == nullptr) {
                out = "su: user not found";
                return -1;
            }
            const char* pw = args.size() > 1 ? args[1].c_str() : nullptr;
            int ret = um.su(u->uid, pw);
            if (ret != 0) {
                out = "su: failed (need password or root)";
                return -1;
            }
            out = "Switched to " + um.current_username();
            return 0;
        },
        "su <uid|name> [pw] — switch user");

    reg.register_cmd(
        "more",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: more <file>";
                return -1;
            }
            int fd = fs.fs_open(args[0].c_str(), O_READ);
            if (fd < 0) {
                out = "more: " + open_error(fs, args[0]);
                return -1;
            }
            std::vector<char> buf(65536, 0);
            ssize_t n = fs.fs_read(fd, buf.data(), buf.size() - 1);
            fs.fs_close(fd);
            if (n < 0) {
                out = "more: read error";
                return -1;
            }
            // Hand the content to the caller's pager (TUI full-screen / CLI
            // --More--) via the prefix, so it is shown one screen at a time.
            out = std::string(PAGER_PREFIX) + std::string(buf.data(), n);
            return 0;
        },
        "more <file> — view file content one page at a time");

    reg.register_cmd(
        "find",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: find <path> <name>";
                return -1;
            }
            const char* start = args[0].c_str();
            const std::string& pattern = args[1];
            out.clear();
            // Recursive search
            std::function<void(const std::string&)> search =
                [&](const std::string& dir) {
                    std::vector<DirEntry> entries;
                    if (fs.fs_ls(dir.c_str(), entries) != 0) return;
                    for (auto& e : entries) {
                        if (std::strcmp(e.name, ".") == 0 ||
                            std::strcmp(e.name, "..") == 0)
                            continue;
                        std::string full =
                            (dir == "/") ? "/" + std::string(e.name)
                                         : dir + "/" + std::string(e.name);
                        if (std::string(e.name).find(pattern) !=
                            std::string::npos) {
                            out += full + "\n";
                        }
                        if (e.type == TYPE_DIR) search(full);
                    }
                };
            search(start);
            if (out.empty()) out = "(no matches)";
            return 0;
        },
        "find <path> <name> — search files by name");

    reg.register_cmd(
        "rmdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: rmdir <path>...";
                return -1;
            }
            int rc = 0;
            for (auto& d : args) {
                if (fs.fs_rmdir(d.c_str()) != 0) {
                    out += "rmdir: '" + d + "': not empty or not found\n";
                    rc = -1;
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "rmdir <path>... — remove empty directories");

    reg.register_cmd(
        "cd",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            std::string path = args.empty() ? "/" : args[0];
            if (!path.empty() && path[0] == '~') {
                path = "/home/" + um.current_username() + path.substr(1);
            }
            int ret = fs.fs_chdir(path.c_str());
            if (ret != 0) out = "cd: no such directory";
            return ret;
        },
        "cd [path] — change directory (~ = home)");

    reg.register_cmd(
        "pwd",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            out = fs.fs_pwd();
            return 0;
        },
        "pwd — print working directory");

    reg.register_cmd(
        "ls",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            // Append a name colored by type (dir=green/, symlink=cyan@, plain).
            auto paint = [](std::string& dst, const std::string& name, uint8_t type) {
                if (type == TYPE_DIR) {
                    dst += "\033[32m";
                    dst += name;
                    dst += "/\033[0m";
                } else if (type == TYPE_SYMLINK) {
                    dst += "\033[36m";
                    dst += name;
                    dst += "@\033[0m";
                } else {
                    dst += name;
                }
                dst += "  ";
            };

            int rc = 0;
            bool multi = args.size() > 1;
            std::vector<std::string> targets = args;
            if (targets.empty()) targets.push_back("");  // cwd

            std::string files;  // file/symlink args, listed together first
            std::string dirs;   // directory args, each with a header when multi
            for (const std::string& tp : targets) {
                FileStat st{};
                if (!tp.empty() && fs.fs_stat(tp.c_str(), st) != 0) {
                    dirs += "ls: cannot access '" + tp + "'\n";
                    rc = -1;
                    continue;
                }
                bool is_dir = tp.empty() || st.type == TYPE_DIR;
                if (is_dir) {
                    std::vector<DirEntry> entries;
                    if (fs.fs_ls(tp.c_str(), entries) != 0) {
                        dirs += "ls: cannot list '" + tp + "'\n";
                        rc = -1;
                        continue;
                    }
                    std::string body;
                    for (auto& e : entries) paint(body, e.name, e.type);
                    if (body.empty()) body = "(empty)";
                    if (multi) dirs += (tp.empty() ? "." : tp) + ":\n";
                    dirs += body + "\n";
                } else {
                    paint(files, tp, st.type);  // a matched file (e.g. ls *.txt)
                }
            }

            out.clear();
            if (!files.empty()) {
                out += files;
                if (!dirs.empty()) out += "\n";
            }
            out += dirs;
            while (!out.empty() && out.back() == '\n') out.pop_back();
            if (out.empty()) out = "(empty)";
            return rc;
        },
        "ls [path]... — list directories / files");

    reg.register_cmd(
        "ll",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            out.clear();
            int rc = 0;
            bool multi = args.size() > 1;
            std::vector<std::string> targets = args;
            if (targets.empty()) targets.push_back("");  // cwd

            for (const std::string& tp : targets) {
                FileStat st{};
                if (!tp.empty() && fs.fs_stat(tp.c_str(), st) != 0) {
                    out += "ll: cannot access '" + tp + "'\n";
                    rc = -1;
                    continue;
                }
                bool is_dir = tp.empty() || st.type == TYPE_DIR;
                if (is_dir) {
                    std::vector<DirEntry> entries;
                    if (fs.fs_ls(tp.c_str(), entries) != 0) {
                        out += "ll: cannot list '" + tp + "'\n";
                        rc = -1;
                        continue;
                    }
                    if (multi) out += (tp.empty() ? "." : tp) + ":\n";
                    // Stat each entry by its full path (bare e.name would resolve
                    // against the cwd, wrong when listing another directory).
                    for (auto& e : entries)
                        append_ll_line(fs, um, join_path(tp, e.name), e.name,
                                       e.type, out);
                } else {
                    // A file/symlink argument (e.g. from "ll *.txt").
                    append_ll_line(fs, um, tp, tp, st.type, out);
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            if (out.empty()) out = "(empty)";
            return rc;
        },
        "ll [path]... — detailed listing (perm/links/owner/size/mtime)");

    reg.register_cmd(
        "touch",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: touch <file>";
                return -1;
            }
            int ret = fs.fs_create(args[0].c_str(), DEFAULT_MODE);
            if (ret != 0) out = "touch: failed (file may already exist)";
            return ret;
        },
        "touch <file> — create file");

    reg.register_cmd(
        "rm",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            bool recursive = false;
            std::vector<std::string> targets;
            for (auto& a : args) {
                if (a == "-r" || a == "-rf")
                    recursive = true;
                else
                    targets.push_back(a);
            }
            if (targets.empty()) {
                out = "Usage: rm [-r] <path>...";
                return -1;
            }
            // Remove every target (so a glob like "rm *.log" deletes them all);
            // report each failure but keep going.
            int rc = 0;
            for (auto& t : targets) {
                int ret = recursive ? fs.fs_delete_recursive(t.c_str())
                                    : fs.fs_delete(t.c_str());
                if (ret != 0) {
                    out += "rm: cannot remove '" + t + "'\n";
                    rc = -1;
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "rm [-r] <path>... — delete files or directories");

    reg.register_cmd(
        "open",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: open <file> [r|w|rw|a]";
                return -1;
            }
            int flags = O_READ;
            if (args.size() > 1) {
                if (args[1] == "w")
                    flags = O_WRITE;
                else if (args[1] == "rw")
                    flags = O_READ | O_WRITE;
                else if (args[1] == "a")
                    flags = O_WRITE | O_APPEND;
            }
            int fd = fs.fs_open(args[0].c_str(), flags);
            if (fd < 0) {
                out = "open: " + open_error(fs, args[0]);
                return -1;
            }
            out = "fd=" + std::to_string(fd);
            return 0;
        },
        "open <file> [r|w|rw|a] — open file");

    reg.register_cmd(
        "close",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: close <fd>";
                return -1;
            }
            int fd = std::stoi(args[0]);
            int ret = fs.fs_close(fd);
            if (ret != 0) out = "close: invalid fd";
            return ret;
        },
        "close <fd> — close file");

    reg.register_cmd(
        "read",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: read <fd> [len]";
                return -1;
            }
            int fd = std::stoi(args[0]);
            size_t len = 4096;
            if (args.size() > 1) {
                // Reject negative/empty before std::stoul, which would silently
                // wrap "-5" into a near-ULONG_MAX value and blow up the buffer
                // allocation below with a confusing length_error.
                const std::string& len_arg = args[1];
                if (len_arg.empty() || len_arg[0] == '-') {
                    out = "read: invalid length";
                    return -1;
                }
                len = std::stoul(len_arg);
            }
            // Cap the request so a typo can't trigger a multi-gigabyte alloc.
            // The whole data region is far smaller than this anyway.
            constexpr size_t kMaxReadLen = 1u << 20;  // 1 MiB
            if (len > kMaxReadLen) len = kMaxReadLen;
            std::vector<char> buf(len + 1, 0);
            ssize_t n = fs.fs_read(fd, buf.data(), len);
            if (n < 0) {
                out = "read: failed";
                return -1;
            }
            out = std::string(buf.data(), n);
            return 0;
        },
        "read <fd> [len] — read from file");

    reg.register_cmd(
        "write",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: write <fd> <text>";
                return -1;
            }
            int fd = std::stoi(args[0]);
            std::string text;
            for (size_t i = 1; i < args.size(); i++) {
                if (i > 1) text += " ";
                text += args[i];
            }
            ssize_t n = fs.fs_write(fd, text.c_str(), text.size());
            if (n < 0) {
                out = "write: failed";
                return -1;
            }
            out = std::to_string(n) + " bytes written";
            return 0;
        },
        "write <fd> <text> — write to file");

    reg.register_cmd(
        "cat",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: cat <file>...";
                return -1;
            }
            out.clear();
            int rc = 0;
            for (auto& f : args) {
                int fd = fs.fs_open(f.c_str(), O_READ);
                if (fd < 0) {
                    out += "cat: " + f + ": " + open_error(fs, f) + "\n";
                    rc = -1;
                    continue;
                }
                std::vector<char> buf(8192, 0);
                ssize_t n = fs.fs_read(fd, buf.data(), buf.size() - 1);
                fs.fs_close(fd);
                if (n < 0) {
                    out += "cat: " + f + ": read error\n";
                    rc = -1;
                    continue;
                }
                out.append(buf.data(), n);
            }
            return rc;
        },
        "cat <file>... — display file content");

    reg.register_cmd(
        "stat",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: stat <path>...";
                return -1;
            }
            out.clear();
            int rc = 0;
            for (const std::string& p : args) {
                FileStat st{};
                if (fs.fs_stat(p.c_str(), st) != 0) {
                    out += "stat: '" + p + "': not found\n";
                    rc = -1;
                    continue;
                }
                const char* type_str = st.type == TYPE_DIR       ? "directory"
                                       : st.type == TYPE_SYMLINK ? "symlink"
                                                                 : "file";
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "File: %s\nType: %s\nMode: %04o\nUID: %u  GID: %u\n"
                              "Size: %u bytes\nLinks: %u",
                              p.c_str(), type_str, st.mode & 0x1FF, st.uid,
                              st.gid, st.size, st.nlink);
                out += buf;
                if (st.type == TYPE_SYMLINK) {
                    std::string tgt;
                    if (fs.fs_readlink(p.c_str(), tgt) == 0)
                        out += "\nTarget: " + tgt;
                }
                out += "\n";
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "stat <path>... — file/directory info");

    reg.register_cmd(
        "chmod",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: chmod <mode> <path>...";
                return -1;
            }
            uint16_t mode = static_cast<uint16_t>(std::stoi(args[0], nullptr, 8));
            int rc = 0;
            for (size_t i = 1; i < args.size(); ++i) {
                if (fs.fs_chmod(args[i].c_str(), mode) != 0) {
                    out += "chmod: '" + args[i] + "': failed\n";
                    rc = -1;
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "chmod <mode> <path>... — change permissions");

    reg.register_cmd(
        "ln",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            bool symbolic = false;
            std::vector<std::string> pos;
            for (auto& a : args) {
                if (a == "-s")
                    symbolic = true;
                else
                    pos.push_back(a);
            }
            if (pos.size() < 2) {
                out = "Usage: ln [-s] <target> <linkname>";
                return -1;
            }
            int ret = symbolic ? fs.fs_symlink(pos[0].c_str(), pos[1].c_str())
                               : fs.fs_link(pos[0].c_str(), pos[1].c_str());
            if (ret != 0)
                out = symbolic ? "ln: symlink failed (target name taken, or FS has no symlink support)"
                               : "ln: failed";
            return ret;
        },
        "ln [-s] <target> <link> — hard link, or -s for soft (symbolic) link");

    reg.register_cmd(
        "readlink",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: readlink <path>";
                return -1;
            }
            std::string target;
            if (fs.fs_readlink(args[0].c_str(), target) != 0) {
                out = "readlink: not a symbolic link";
                return -1;
            }
            out = target;
            return 0;
        },
        "readlink <path> — print a symlink's target");

    reg.register_cmd(
        "cp",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: cp <src> <dst>";
                return -1;
            }
            int src_fd = fs.fs_open(args[0].c_str(), O_READ);
            if (src_fd < 0) {
                out = "cp: source: " + open_error(fs, args[0]);
                return -1;
            }
            fs.fs_create(args[1].c_str(), DEFAULT_MODE);
            int dst_fd = fs.fs_open(args[1].c_str(), O_WRITE);
            if (dst_fd < 0) {
                fs.fs_close(src_fd);
                out = "cp: cannot create dest";
                return -1;
            }
            char buf[4096];
            ssize_t n;
            while ((n = fs.fs_read(src_fd, buf, sizeof(buf))) > 0) {
                fs.fs_write(dst_fd, buf, n);
            }
            fs.fs_close(src_fd);
            fs.fs_close(dst_fd);
            out = "Copied.";
            return 0;
        },
        "cp <src> <dst> — copy file");

    reg.register_cmd(
        "disk",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            DiskUsage du = fs.fs_disk_usage();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "FS: %s\nBlocks: %u/%u used\nInodes: %u/%u used",
                          fs.fs_type_name().c_str(), du.used_blocks, du.total_blocks,
                          du.used_inodes, du.total_inodes);
            out = buf;
            return 0;
        },
        "disk — show disk usage");

    reg.register_cmd(
        "tree",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            int max_depth = 6;
            const char* path = "";
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "-d" && i + 1 < args.size()) {
                    max_depth = std::stoi(args[++i]);
                    if (max_depth < 1) max_depth = 1;
                } else {
                    path = args[i].c_str();
                }
            }
            std::vector<DirEntry> top;
            if (fs.fs_ls(path, top) != 0) {
                out = "tree: cannot list";
                return -1;
            }
            // Build tree recursively
            std::function<void(const std::vector<DirEntry>&,
                               const std::string&, int, const std::string&)>
                walk = [&](const std::vector<DirEntry>& entries,
                           const std::string& base, int depth,
                           const std::string& prefix) {
                    if (depth > max_depth) return;
                    for (size_t idx = 0; idx < entries.size(); ++idx) {
                        auto& e = entries[idx];
                        if (std::strcmp(e.name, ".") == 0) continue;
                        if (std::strcmp(e.name, "..") == 0) continue;
                        bool last = (idx == entries.size() - 1);
                        out += prefix + (last ? "\\-- " : "|-- ");
                        out += e.name;
                        if (e.type == TYPE_DIR) out += "/";
                        out += "\n";
                        if (e.type == TYPE_DIR &&
                            depth < max_depth) {
                            std::string child =
                                base + "/" + std::string(e.name);
                            std::vector<DirEntry> kids;
                            if (fs.fs_ls(child.c_str(), kids) == 0) {
                                std::string next_prefix =
                                    prefix + (last ? "    " : "|   ");
                                walk(kids, child, depth + 1,
                                     next_prefix);
                            }
                        }
                    }
                };
            out.clear();
            walk(top, ".", 1, "");
            if (out.empty()) out = "(empty)";
            return 0;
        },
        "tree [-d N] [path] — show directory tree");

    reg.register_cmd(
        "mv",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: mv <src> <dst>";
                return -1;
            }
            FileStat st{};
            if (fs.fs_stat(args[0].c_str(), st) != 0) {
                out = "mv: source not found";
                return -1;
            }
            if (st.type == TYPE_DIR) {
                out = "mv: directory move not supported (use cp+rm)";
                return -1;
            }
            // Copy file content
            int sfd = fs.fs_open(args[0].c_str(), O_READ);
            if (sfd < 0) {
                out = "mv: cannot read source";
                return -1;
            }
            fs.fs_create(args[1].c_str(), DEFAULT_MODE);
            int dfd = fs.fs_open(args[1].c_str(), O_WRITE);
            if (dfd < 0) {
                fs.fs_close(sfd);
                out = "mv: cannot create dest";
                return -1;
            }
            char buf[4096];
            ssize_t n;
            while ((n = fs.fs_read(sfd, buf, sizeof(buf))) > 0) {
                fs.fs_write(dfd, buf, n);
            }
            fs.fs_close(sfd);
            fs.fs_close(dfd);
            fs.fs_delete(args[0].c_str());
            out = "Moved.";
            return 0;
        },
        "mv <src> <dst> — move/rename file");

    reg.register_cmd(
        "help",
        [&reg](IFileSystem&, UserManager&, const std::vector<std::string>&,
               std::string& out) -> int {
            auto cmds = reg.list_commands();
            out.clear();
            for (auto& [name, usage] : cmds) {
                out += usage + "\n";
            }
            return 0;
        },
        "help — list all commands");

    reg.register_cmd(
        "log",
        [&reg](IFileSystem&, UserManager&, const std::vector<std::string>& args,
               std::string& out) -> int {
            if (!args.empty() && args[0] == "clear") {
                reg.clear_log();
                out = "Operation log cleared.";
                return 0;
            }
            const auto& entries = reg.op_log();
            if (entries.empty()) {
                out = "(operation log empty)";
                return 0;
            }
            out.clear();
            for (size_t i = 0; i < entries.size(); ++i) {
                char ts[24] = "-";
                std::time_t t = static_cast<std::time_t>(entries[i].time);
                std::tm* lt = std::localtime(&t);
                if (lt) std::strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", lt);
                char head[48];
                std::snprintf(head, sizeof(head), "%3zu  %s  %-8s  ", i + 1, ts,
                              entries[i].user.c_str());
                out += head;
                out += entries[i].cmdline;
                out += "\n";
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return 0;
        },
        "log [clear] — show (or clear) the operation log");

    reg.register_cmd(
        "replay",
        [&reg](IFileSystem& fs, UserManager& um, const std::vector<std::string>&,
               std::string& out) -> int { return reg.replay(fs, um, out); },
        "replay — re-run every logged operation in order");

    reg.register_cmd(
        "exit",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&, std::string& out) -> int {
            out = "__EXIT__";
            return 0;
        },
        "exit — quit PseudoFS");
}

// CLI pager for `more`. Interactively (a TTY) shows `content` one screen at a
// time with an --More-- prompt; when piped/redirected it just dumps everything
// so scripts and tests are unaffected.
static void cli_page(const std::string& content) {
    if (!isatty(fileno(stdin))) {
        std::fwrite(content.data(), 1, content.size(), stdout);
        if (!content.empty() && content.back() != '\n') std::printf("\n");
        return;
    }
    std::vector<std::string> lines;
    std::string cur;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    const int page = 20;
    const int total = static_cast<int>(lines.size());
    for (int top = 0; top < total; top += page) {
        for (int i = top; i < top + page && i < total; ++i)
            std::printf("%s\n", lines[i].c_str());
        if (top + page >= total) break;
        std::printf("\033[7m--More-- (%d/%d) [Enter=more, q=quit]\033[0m",
                    std::min(top + page, total), total);
        std::fflush(stdout);
        int c = std::getchar();
        if (c != '\n' && c != EOF) {
            while (true) {  // swallow the rest of the input line
                int d = std::getchar();
                if (d == '\n' || d == EOF) break;
            }
        }
        std::printf("\r\033[K");  // erase the --More-- prompt
        if (c == 'q' || c == 'Q' || c == EOF) break;
    }
}

int main(int argc, char* argv[]) {
    bool use_tui = false;
    bool use_fat16 = false;
    bool force_format = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--tui") == 0) use_tui = true;
        if (std::strcmp(argv[i], "--fat16") == 0) use_fat16 = true;
        if (std::strcmp(argv[i], "--format") == 0) force_format = true;
    }

    CommandRegistry reg;
    register_commands(reg);
    reg.set_log_path("pfs_ops.log");  // persistent operation log (C-05)

    UserManager um;
    if (!force_format) um.load_from_file(USERS_FILE);  // restore prior accounts
    um.set_persist_path(USERS_FILE);  // auto-save on useradd/passwd
    if (force_format) um.save_to_file(USERS_FILE);  // --format: reset to root

    if (use_tui) {
        // --- TUI mode: each engine gets its OWN BlockDevice ---
        // UNIX and FAT16 have incompatible on-disk layouts; sharing one device
        // means whichever engine formats/loads last clobbers the other's bytes,
        // and fs_mount() (which only re-reads the device buffer) then sees
        // garbage. Independent devices let both stay mounted at once, so F2
        // just swaps the active pointer and each saves to its own image.
        const char* unix_img = "pfs_tui_unix.img";
        const char* fat16_img = "pfs_tui_fat16.img";

        BlockDevice unix_dev(TOTAL_BLK_NUM, BLOCK_SIZE);
        BlockDevice fat16_dev(TOTAL_BLK_NUM, BLOCK_SIZE);
        UnixFs unix_fs(unix_dev);
        Fat16Fs fat16_fs(fat16_dev);
        unix_fs.set_disk_path(unix_img);
        fat16_fs.set_disk_path(fat16_img);

        // Each engine: load its image + mount, else format fresh.
        if (force_format || unix_dev.load_from_file(unix_img) != 0) {
            unix_fs.fs_format();
        } else if (unix_fs.fs_mount() != 0) {
            unix_fs.fs_format();
        }
        if (force_format || fat16_dev.load_from_file(fat16_img) != 0) {
            fat16_fs.fs_format();
        } else if (fat16_fs.fs_mount() != 0) {
            fat16_fs.fs_format();
        }

        IFileSystem* primary =
            use_fat16 ? static_cast<IFileSystem*>(&fat16_fs)
                      : static_cast<IFileSystem*>(&unix_fs);
        IFileSystem* alt =
            use_fat16 ? static_cast<IFileSystem*>(&unix_fs)
                      : static_cast<IFileSystem*>(&fat16_fs);

        Tui tui(*primary, *alt, um, reg);
        tui.run();

        // Persist each engine to its own image — no cross-clobber.
        unix_fs.fs_unmount();
        fat16_fs.fs_unmount();
        unix_dev.save_to_file(unix_img);
        fat16_dev.save_to_file(fat16_img);
        std::printf("Disks saved. Goodbye.\n");
        return 0;
    }

    // --- CLI mode ---
    const char* disk_path = use_fat16 ? FAT16_DISK : UNIX_DISK;

    BlockDevice dev(TOTAL_BLK_NUM, BLOCK_SIZE);
    UnixFs unix_fs(dev);
    Fat16Fs fat16_fs(dev);

    IFileSystem* fs =
        use_fat16 ? static_cast<IFileSystem*>(&fat16_fs) : static_cast<IFileSystem*>(&unix_fs);

    unix_fs.set_disk_path(disk_path);
    fat16_fs.set_disk_path(disk_path);

    if (!force_format && dev.load_from_file(disk_path) == 0) {
        if (fs->fs_mount() != 0) {
            std::printf("Mount failed, reformatting...\n");
            fs->fs_format();
        }
    } else {
        fs->fs_format();
    }

    std::printf("PseudoFS v2.0 [%s] — type 'help' for commands, 'exit' to quit\n",
                fs->fs_type_name().c_str());

    std::vector<std::string> history;
    // Register history command after history vector exists
    reg.register_cmd(
        "history",
        [&history](IFileSystem&, UserManager&, const std::vector<std::string>&,
                   std::string& out) -> int {
            out.clear();
            for (size_t i = 0; i < history.size(); ++i) {
                out += std::to_string(i + 1) + "  " + history[i] + "\n";
            }
            if (out.empty()) out = "(no history)";
            return 0;
        },
        "history — show command history");

    char input[1024];
    while (true) {
        std::printf("\033[32m%s\033[0m$ ", fs->fs_pwd().c_str());
        std::fflush(stdout);
        if (std::fgets(input, sizeof(input), stdin) == nullptr) break;

        size_t len = std::strlen(input);
        if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
        if (input[0] == '\0') continue;

        history.push_back(input);

        std::string output;
        int ret = reg.execute(input, *fs, um, output);

        if (output == "__EXIT__") break;
        const std::string pager(PAGER_PREFIX);
        if (output.rfind(pager, 0) == 0) {
            cli_page(output.substr(pager.size()));  // `more`: paged display
        } else if (!output.empty()) {
            if (ret != 0)
                std::printf("\033[31m%s\033[0m\n", output.c_str());
            else
                std::printf("%s\n", output.c_str());
        }
    }

    fs->fs_unmount();
    dev.save_to_file(disk_path);
    std::printf("Disk saved. Goodbye.\n");
    return 0;
}
