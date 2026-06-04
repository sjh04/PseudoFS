#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/user_manager.h"
#include "core/vfs.h"
#include "fs/fat16/fat16_fs.h"
#include "fs/unix/unix_fs.h"
#include "shell/command_registry.h"
#include "tui/tui.h"

using namespace pfs;

static const char* UNIX_DISK = "pfs_unix.img";
static const char* FAT16_DISK = "pfs_fat16.img";
// 用户账户存在 FS 镜像之外,这样一张表能被两个引擎、两种模式(CLI / TUI)共用。
static const char* USERS_FILE = "pfs_users.dat";

// ---- CLI 输入 + Ctrl+C 处理 ----
//
// Ctrl+C 应该像 shell 那样取消当前输入行并重新出提示符,而不是杀掉程序。
// 我们装一个 SIGINT 处理函数(不带 SA_RESTART),让阻塞中的 read(2) 返回 EINTR;
// 下面这个标志告诉输入层这是一次中断,而不是 EOF/错误。所有 CLI 输入都走同一个
// 带缓冲的读取器(read_char)——提示符循环和 `more` 分页器都走它——于是只有一条
// 输入流,中断处理是确定的,也不会被 stdio 缓冲坑到。
static volatile sig_atomic_t g_sigint = 0;
// SIGINT 信号处理函数:只置标志,真正的取消逻辑放在输入层判断
static void handle_sigint(int) {
    g_sigint = 1;
}

static unsigned char g_inbuf[4096];
static size_t g_inbuf_len = 0;
static size_t g_inbuf_pos = 0;

// 读一个字节的 CLI 输入。成功返回 1 并写 *c,EOF 返回 0,
// 被信号打断返回 -1(用 g_sigint 区分是不是 Ctrl+C)。
static int read_char(char* c) {
    if (g_inbuf_pos >= g_inbuf_len) {
        ssize_t n = read(STDIN_FILENO, g_inbuf, sizeof(g_inbuf));
        if (n < 0) return -1;  // EINTR 或硬错误
        if (n == 0) return 0;  // EOF(Ctrl+D / 管道关闭)
        g_inbuf_len = static_cast<size_t>(n);
        g_inbuf_pos = 0;
    }
    *c = static_cast<char>(g_inbuf[g_inbuf_pos++]);
    return 1;
}

enum class LineResult { Ok, Eof, Canceled };

// 用 read_char 读一整行(去掉换行符)。Canceled 表示按了 Ctrl+C。
static LineResult read_line(std::string& out) {
    out.clear();
    while (true) {
        char c;
        int r = read_char(&c);
        if (r == 1) {
            if (c == '\n') return LineResult::Ok;
            if (c != '\r') out.push_back(c);  // 容忍 CRLF 输入
        } else if (r == 0) {
            // EOF:先把末尾这半行交出去一次,然后才报 EOF。
            return out.empty() ? LineResult::Eof : LineResult::Ok;
        } else {  // r < 0
            if (errno == EINTR) {
                if (g_sigint) {
                    g_sigint = 0;
                    return LineResult::Canceled;
                }
                continue;  // 其它信号:继续读
            }
            return LineResult::Eof;
        }
    }
}

// 给"打开失败"找个像样的解释:fs_open 对"不存在"和"被拒绝"都返回 -1。
// 如果用 stat 还能解析出这个路径,说明它存在,失败是被权限检查挡住的;
// 否则就是真不存在。这样命令能报"permission denied"而不是误导人的"file not found"。
static std::string open_error(IFileSystem& fs, const std::string& path) {
    FileStat st{};
    if (fs.fs_stat(path.c_str(), st) != 0) return "no such file";
    // 目录不能当文件打开(fs_open 会拒绝)。
    if (st.type == TYPE_DIR) return "is a directory";
    // 路径存在。如果是符号链接,失败多半是因为目标解析不出来——
    // 跟真正的权限拒绝区分开(fs_stat 是 lstat 语义,所以去 stat 一下目标就知道了)。
    if (st.type == TYPE_SYMLINK) {
        std::string target;
        FileStat tst{};
        if (fs.fs_readlink(path.c_str(), target) == 0 && fs.fs_stat(target.c_str(), tst) != 0) {
            return "broken symbolic link";
        }
    }
    return "permission denied";
}

// 把目录路径和一个目录项名拼起来。base 为 "" 表示当前目录(直接用裸名,相对 cwd 解析)。
static std::string join_path(const std::string& base, const char* name) {
    if (base.empty()) return std::string(name);
    if (base == "/") return "/" + std::string(name);
    return base + "/" + std::string(name);
}

// 给 `ll` 往 out 里追加一个条目的详细行:
//   <类型+rwx>  <nlink>  <owner>  <size>  <mtime>  <display>[/ | @ -> target]
// full 是拿去 stat/readlink 的路径;display 是实际显示的文本。
static void append_ll_line(IFileSystem& fs, UserManager& um, const std::string& full,
                           const std::string& display, uint8_t type, std::string& out) {
    FileStat st{};
    fs.fs_stat(full.c_str(), st);

    // 权限列:类型字符 + rwx 三组,例如 drwxr-xr-x。
    char perm[11];
    perm[0] = (type == TYPE_DIR) ? 'd' : (type == TYPE_SYMLINK) ? 'l' : '-';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) perm[i + 1] = (st.mode & (1 << (8 - i))) ? rwx[i] : '-';
    perm[10] = '\0';

    // 属主:认识 uid 就显示用户名,否则显示裸 uid。
    char owner[16];
    const UserRecord* u = um.find_user(st.uid);
    if (u != nullptr)
        std::snprintf(owner, sizeof(owner), "%s", u->username);
    else
        std::snprintf(owner, sizeof(owner), "%u", st.uid);

    // 修改时间,格式 "MM-DD HH:MM"。
    char when[16] = "-";
    std::time_t t = static_cast<std::time_t>(st.mtime);
    std::tm* lt = std::localtime(&t);
    if (lt) std::strftime(when, sizeof(when), "%m-%d %H:%M", lt);

    // 名字后缀:目录加 "/",符号链接加 "@ -> 目标"。
    std::string suffix;
    if (type == TYPE_DIR) {
        suffix = "/";
    } else if (type == TYPE_SYMLINK) {
        std::string tgt;
        suffix = (fs.fs_readlink(full.c_str(), tgt) == 0) ? "@ -> " + tgt : "@";
    }

    // 定宽前缀用 snprintf 拼;名字+后缀以字符串追加,这样长的符号链接目标不会被截断。
    char head[64];
    std::snprintf(head, sizeof(head), "%s %2u %-8s %6u  %s  ", perm, st.nlink, owner, st.size,
                  when);
    out += head;
    out += display;
    out += suffix;
    out += "\n";
}

// 把所有 shell 命令的 handler 注册进命令表。每个 handler 是个 lambda,签名统一为
// (文件系统, 用户管理, 参数表, 输出串) -> 返回码(0 成功, 非 0 失败)。
static void register_commands(CommandRegistry& reg) {
    // login <user> <pw>:校验账号密码并登录,成功后欢迎语带上 uid。
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
            out = "Welcome, " + um.current_username() + " (uid=" + std::to_string(uid) + ")";
            return 0;
        },
        "login <user> <pw> — log in");

    // logout:注销当前用户。已是未登录态则报错。
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

    // useradd <user> <pw> <uid> <gid>:仅 root 可建用户,顺带在 /home 下建用户主目录。
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
            // 给新用户建主目录。/home 仍归 root;用户自己那一级以新用户身份创建,
            // 这样它归用户所有(登录后才能真正在里面写东西)。
            std::string home = "/home/" + std::string(args[0]);
            fs.fs_mkdir("/home");
            fs.set_user(uid, gid);
            fs.fs_mkdir(home.c_str());
            fs.set_user(um.current_uid(), um.current_gid());  // 恢复成 root
            out = "User " + std::string(args[0]) + " created.";
            return 0;
        },
        "useradd <user> <pw> <uid> <gid> — create user (root only)");

    // passwd <old> <new>:改当前用户密码,要先验旧密码。
    reg.register_cmd(
        "passwd",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: passwd <old-password> <new-password>";
                return -1;
            }
            int ret = um.change_password(um.current_uid(), args[0].c_str(), args[1].c_str());
            if (ret != 0) {
                out = "passwd: failed (check old password)";
                return -1;
            }
            out = "Password changed.";
            return 0;
        },
        "passwd <old> <new> — change password");

    // format:格式化虚拟磁盘,重建整个文件卷(超级块/空闲链/根目录)。
    reg.register_cmd(
        "format",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            int ret = fs.fs_format();
            out = (ret == 0) ? "Filesystem formatted." : "Format failed.";
            return ret;
        },
        "format — format the virtual disk");

    // mkdir [-p] <path>:建目录。带 -p 时逐级建,中间目录已存在不报错。
    reg.register_cmd(
        "mkdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            bool mkdir_p = false;
            std::string path;
            for (auto& a : args) {
                if (a == "-p")
                    mkdir_p = true;
                else
                    path = a;
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
            // -p:逐级创建
            std::string cur;
            size_t pos = 0;
            if (path[0] == '/') {
                cur = "/";
                pos = 1;
            }
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
            // 最后兜底检查:整条路径现在到底存不存在?
            FileStat st;
            if (fs.fs_stat(path.c_str(), st) == 0) return 0;
            out = "mkdir: failed";
            return -1;
        },
        "mkdir [-p] <path> — create directory");

    // su <uid|name> [pw]:切换用户。目标可以写用户名或数字 uid;切到他人需密码(root 免)。
    reg.register_cmd(
        "su",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: su <uid|username> [password]";
                return -1;
            }
            // 解析目标:先按名字找,找不到再当数字 uid。
            // 只有参数全是数字才尝试解析 uid——否则对一个坏名字(比如 "su alice")
            // 调 std::stoi 会抛异常,冒出个莫名其妙的"invalid numeric argument"盖住真正的问题。
            const UserRecord* u = um.find_user(args[0].c_str());
            if (u == nullptr) {
                const std::string& who = args[0];
                bool numeric =
                    !who.empty() && who.find_first_not_of("0123456789") == std::string::npos;
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

    // more <file>:分页查看文件内容。读完整文件后加 PAGER_PREFIX 前缀,交给调用方的分页器。
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
            // 内容前面加前缀交给调用方的分页器(TUI 全屏 / CLI --More--),让它一屏一屏地显示。
            out = std::string(PAGER_PREFIX) + std::string(buf.data(), n);
            return 0;
        },
        "more <file> — view file content one page at a time");

    // find <path> <name>:从 path 起递归找名字含 name 子串的项,逐条输出全路径。
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
            // 递归搜索。深度上限是纵深防御:正常树绝不会这么深,但若某个目录被早先(或将来)
            // 的 bug 写坏了,可能驱动无限递归把程序搞崩——干脆给它封顶。
            std::function<void(const std::string&, int)> search = [&](const std::string& dir,
                                                                      int depth) {
                if (depth > 64) return;
                std::vector<DirEntry> entries;
                if (fs.fs_ls(dir.c_str(), entries) != 0) return;
                for (auto& e : entries) {
                    if (std::strcmp(e.name, ".") == 0 || std::strcmp(e.name, "..") == 0) continue;
                    std::string full =
                        (dir == "/") ? "/" + std::string(e.name) : dir + "/" + std::string(e.name);
                    if (std::string(e.name).find(pattern) != std::string::npos) {
                        out += full + "\n";
                    }
                    if (e.type == TYPE_DIR) search(full, depth + 1);
                }
            };
            search(start, 0);
            if (out.empty()) out = "(no matches)";
            return 0;
        },
        "find <path> <name> — search files by name");

    // rmdir <path>...:删空目录,可一次删多个。非空或不存在则逐个报错但继续。
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

    // cd [path]:切换工作目录。无参回根,开头 ~ 展开成 /home/当前用户。
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

    // pwd:打印当前工作目录的绝对路径。
    reg.register_cmd(
        "pwd",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            out = fs.fs_pwd();
            return 0;
        },
        "pwd — print working directory");

    // ls [path]...:列目录/文件,按类型上色。文件参数先列、目录参数后列,多目标时加表头。
    reg.register_cmd(
        "ls",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            // 按类型给名字上色追加(目录=绿色/,符号链接=青色@,普通文件原样)。
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
            if (targets.empty()) targets.push_back("");  // 当前目录

            std::string files;  // 文件/符号链接参数,先一起列出来
            std::string dirs;   // 目录参数,多目标时各带一个表头
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
                    paint(files, tp, st.type);  // 命中的文件(例如 ls *.txt)
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

    // ll [path]...:详细列表,每行带权限/链接数/属主/大小/修改时间,见 append_ll_line。
    reg.register_cmd(
        "ll",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            out.clear();
            int rc = 0;
            bool multi = args.size() > 1;
            std::vector<std::string> targets = args;
            if (targets.empty()) targets.push_back("");  // 当前目录

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
                    // 每个条目用完整路径去 stat(裸 e.name 会相对 cwd 解析,列别的目录时就错了)。
                    for (auto& e : entries)
                        append_ll_line(fs, um, join_path(tp, e.name), e.name, e.type, out);
                } else {
                    // 文件/符号链接参数(例如来自 "ll *.txt")。
                    append_ll_line(fs, um, tp, tp, st.type, out);
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            if (out.empty()) out = "(empty)";
            return rc;
        },
        "ll [path]... — detailed listing (perm/links/owner/size/mtime)");

    // touch <file>...:创建文件,可一次建多个。
    reg.register_cmd(
        "touch",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: touch <file>...";
                return -1;
            }
            // 逐个创建;像其它多目标命令(rm/rmdir/ls/cat)一样,单个失败照报但继续。
            int rc = 0;
            for (const auto& f : args) {
                if (fs.fs_create(f.c_str(), DEFAULT_MODE) != 0) {
                    out += "touch: '" + f + "': failed (may already exist)\n";
                    rc = -1;
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "touch <file>... — create file(s)");

    // rm [-r] <path>...:删文件;带 -r/-rf 时递归删目录。可一次删多个。
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
            // 把每个目标都删掉(这样像 "rm *.log" 的通配能一次删完);单个失败照报但继续。
            int rc = 0;
            for (auto& t : targets) {
                int ret = recursive ? fs.fs_delete_recursive(t.c_str()) : fs.fs_delete(t.c_str());
                if (ret != 0) {
                    out += "rm: cannot remove '" + t + "'\n";
                    rc = -1;
                }
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "rm [-r] <path>... — delete files or directories");

    // open <file> [r|w|rw|a]:打开文件返回 fd。模式默认只读,w 会截断,a 追加,rw 读写。
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
                    flags = O_WRITE | O_TRUNC;  // `w` 丢掉旧内容(对应 fopen 的 "w")
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

    // close <fd>:关闭文件描述符,从打开文件表移除。
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

    // read <fd> [len]:从 fd 当前偏移读至多 len 字节(默认 4096)并显示。
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
                // 在 std::stoul 之前先挡掉负数/空串——否则 "-5" 会被悄悄绕成接近 ULONG_MAX 的值,
                // 下面分配缓冲区时炸出个莫名其妙的 length_error。
                const std::string& len_arg = args[1];
                if (len_arg.empty() || len_arg[0] == '-') {
                    out = "read: invalid length";
                    return -1;
                }
                len = std::stoul(len_arg);
            }
            // 给请求长度封顶,免得手滑打错触发好几个 GB 的分配。
            // 反正整个数据区也远比这个小。
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

    // write <fd> <text>:把 text(多个词用空格拼)写到 fd 当前偏移,返回写入字节数。
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

    // cat <file>...:依次读出并拼接多个文件的全部内容。单个失败照报但继续。
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

    // stat <path>...:打印文件/目录/符号链接的详细元信息(类型/权限/uid/gid/大小/链接数)。
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
                              p.c_str(), type_str, st.mode & 0x1FF, st.uid, st.gid, st.size,
                              st.nlink);
                out += buf;
                if (st.type == TYPE_SYMLINK) {
                    std::string tgt;
                    if (fs.fs_readlink(p.c_str(), tgt) == 0) out += "\nTarget: " + tgt;
                }
                out += "\n";
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return rc;
        },
        "stat <path>... — file/directory info");

    // chmod <mode> <path>...:改权限,mode 按八进制解析(如 755),可一次改多个。
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

    // ln [-s] <target> <link>:默认建硬链接;带 -s 建软(符号)链接。
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
                out = symbolic
                          ? "ln: symlink failed (target name taken, or FS has no symlink support)"
                          : "ln: failed";
            return ret;
        },
        "ln [-s] <target> <link> — hard link, or -s for soft (symbolic) link");

    // readlink <path>:打印一个符号链接指向的目标路径,不是软链接则报错。
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

    // cp <src> <dst>:复制文件。建好 dst 后边读边写,读完两端都关闭。
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
            // 用 O_TRUNC,这样覆盖一个更长的旧文件时不会留尾巴。
            // (fs_open 拒绝目录作 dst,所以 `cp f dir` 报错而非写坏。)
            int dst_fd = fs.fs_open(args[1].c_str(), O_WRITE | O_TRUNC);
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

    // disk:显示磁盘占用,块和 inode 的已用/总数,以及当前引擎名。
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

    // tree [-d N] [path]:树状打印目录结构,-d 限定最大深度(默认 6),跳过 . 和 ..。
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
            // 递归构建树形输出
            std::function<void(const std::vector<DirEntry>&, const std::string&, int,
                               const std::string&)>
                walk = [&](const std::vector<DirEntry>& entries, const std::string& base, int depth,
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
                        if (e.type == TYPE_DIR && depth < max_depth) {
                            std::string child = base + "/" + std::string(e.name);
                            std::vector<DirEntry> kids;
                            if (fs.fs_ls(child.c_str(), kids) == 0) {
                                std::string next_prefix = prefix + (last ? "    " : "|   ");
                                walk(kids, child, depth + 1, next_prefix);
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

    // mv <src> <dst>:移动/重命名文件,实现是"复制内容到 dst 再删 src"。暂不支持目录。
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
            // 复制文件内容
            int sfd = fs.fs_open(args[0].c_str(), O_READ);
            if (sfd < 0) {
                out = "mv: cannot read source";
                return -1;
            }
            fs.fs_create(args[1].c_str(), DEFAULT_MODE);
            int dfd = fs.fs_open(args[1].c_str(), O_WRITE | O_TRUNC);
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

    // help:列出所有命令及其用法说明(从命令表里取)。
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

    // log [clear]:打印操作日志(时间/用户/命令行);带 clear 则清空日志。
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
                // 定宽的序号+时间戳用 snprintf 拼;用户名和命令行(长度不定)以字符串追加,
                // 这样谁都不会溢出/截断那个定长缓冲区。
                char head[40];
                std::snprintf(head, sizeof(head), "%3zu  %s  ", i + 1, ts);
                out += head;
                std::string user = entries[i].user;
                out += user;
                if (user.size() < 8) out += std::string(8 - user.size(), ' ');
                out += "  ";
                out += entries[i].cmdline;
                out += "\n";
            }
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return 0;
        },
        "log [clear] — show (or clear) the operation log");

    // replay:按记录顺序把日志里的每条操作重新执行一遍。
    reg.register_cmd(
        "replay",
        [&reg](IFileSystem& fs, UserManager& um, const std::vector<std::string>&,
               std::string& out) -> int { return reg.replay(fs, um, out); },
        "replay — re-run every logged operation in order");

    // exit:退出 PseudoFS。返回特殊串 __EXIT__,由主循环识别后跳出。
    reg.register_cmd(
        "exit",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&, std::string& out) -> int {
            out = "__EXIT__";
            return 0;
        },
        "exit — quit PseudoFS");
}

// `more` 的 CLI 分页器。交互式(TTY)下带 --More-- 提示一屏一屏地显示 content;
// 被管道/重定向时直接全量输出,脚本和测试不受影响。
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
        for (int i = top; i < top + page && i < total; ++i) std::printf("%s\n", lines[i].c_str());
        if (top + page >= total) break;
        std::printf("\033[7m--More-- (%d/%d) [Enter=more, q=quit]\033[0m",
                    std::min(top + page, total), total);
        std::fflush(stdout);
        // 按键也走那个共享的带缓冲读取器。Ctrl+C(或 EOF)退出分页器回到提示符,而不是杀程序。
        char ch;
        int r = read_char(&ch);
        if (r <= 0) {  // EOF 或被打断 -> 退出分页器
            if (r < 0 && errno == EINTR && g_sigint) g_sigint = 0;
            std::printf("\r\033[K");
            break;
        }
        int c = static_cast<unsigned char>(ch);
        if (c != '\n') {
            while (true) {  // 把这一行剩下的输入吞掉
                char d;
                if (read_char(&d) != 1 || d == '\n') break;
            }
        }
        std::printf("\r\033[K");  // 擦掉 --More-- 提示
        if (c == 'q' || c == 'Q') break;
    }
}

// 程序入口:解析命令行开关 → 初始化命令表/用户/信号 → 按 TUI 或 CLI 模式起引擎跑主循环。
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
    reg.set_log_path("pfs_ops.log");  // 持久化操作日志(C-05)

    UserManager um;
    if (!force_format) um.load_from_file(USERS_FILE);  // 恢复以前的账户
    um.set_persist_path(USERS_FILE);                   // useradd/passwd 时自动落盘
    if (force_format) um.save_to_file(USERS_FILE);     // --format:重置为只有 root
    // 会话以 root 身份开始,这样登录态跟引擎默认的 uid=0 上下文一致:`useradd` 开箱即用,
    // 界面显示 "root" 而不是那种"已注销却仍以 root 身份操作"的误导态。`logout` 仍会丢掉会话;
    // 之后用户可以 `login`/`su` 到任意账户。
    um.login_root();

    // 在两种模式启动之前先装好 SIGINT(Ctrl+C)处理。
    //  - CLI:read_line() 见到 EINTR 后取消当前输入行。
    //  - TUI:wgetch() 返回 ERR(当成普通的"刷新"节拍处理),所以 Ctrl+C 直接被忽略——退出用 F10。
    //    因为我们先装了它,ncurses 就把 SIGINT 留给我们(它只接管那些还是 SIG_DFL 的信号),
    //    于是 Ctrl+C 不再杀掉 TUI 也不会搞乱终端。
    // 不带 SA_RESTART,这样阻塞中的 read(2)/wgetch 会返回 EINTR。
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    if (use_tui) {
        // --- TUI 模式:每个引擎用各自独立的 BlockDevice ---
        // UNIX 和 FAT16 的磁盘布局互不兼容;共用一个设备的话,后格式化/加载的那个引擎会把
        // 另一个的字节覆盖掉,而 fs_mount()(只是重读设备缓冲区)接着就读到一堆垃圾。
        // 独立设备让两者能同时挂载,于是 F2 只需切换活动指针,各自存到各自的镜像。
        const char* unix_img = "pfs_tui_unix.img";
        const char* fat16_img = "pfs_tui_fat16.img";

        BlockDevice unix_dev(TOTAL_BLK_NUM, BLOCK_SIZE);
        BlockDevice fat16_dev(TOTAL_BLK_NUM, BLOCK_SIZE);
        UnixFs unix_fs(unix_dev);
        Fat16Fs fat16_fs(fat16_dev);
        unix_fs.set_disk_path(unix_img);
        fat16_fs.set_disk_path(fat16_img);

        // 每个引擎:加载自己的镜像并挂载,失败则全新格式化。
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
            use_fat16 ? static_cast<IFileSystem*>(&fat16_fs) : static_cast<IFileSystem*>(&unix_fs);
        IFileSystem* alt =
            use_fat16 ? static_cast<IFileSystem*>(&unix_fs) : static_cast<IFileSystem*>(&fat16_fs);

        Tui tui(*primary, *alt, um, reg);
        tui.run();

        // 各引擎各存各的镜像——不会互相覆盖。
        unix_fs.fs_unmount();
        fat16_fs.fs_unmount();
        unix_dev.save_to_file(unix_img);
        fat16_dev.save_to_file(fat16_img);
        std::printf("Disks saved. Goodbye.\n");
        return 0;
    }

    // --- CLI 模式 ---
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
    // history 命令依赖 history 向量,所以等它存在后再注册
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

    // 基于 read(2) 的带缓冲输入:命令行长度无上限(不再有 char[1024]+fgets 的截断问题——
    // 那会把长 `write` 内容截掉,溢出部分还漏成一条莫名其妙的下一条命令),Ctrl+C 也处理得干净。
    std::string input;
    while (true) {
        std::printf("\033[32m%s\033[0m$ ", fs->fs_pwd().c_str());
        std::fflush(stdout);
        LineResult lr = read_line(input);
        if (lr == LineResult::Eof) break;       // Ctrl+D / 管道关闭 -> 退出
        if (lr == LineResult::Canceled) {  // Ctrl+C -> 丢弃本行,重新出提示符
            // 终端已经回显了 "^C";像 shell 那样换个行就行。
            std::printf("\n");
            continue;
        }
        if (input.empty()) continue;

        history.push_back(input);

        std::string output;
        int ret = reg.execute(input, *fs, um, output);

        if (output == "__EXIT__") break;
        const std::string pager(PAGER_PREFIX);
        if (output.rfind(pager, 0) == 0) {
            cli_page(output.substr(pager.size()));  // `more`:分页显示
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
