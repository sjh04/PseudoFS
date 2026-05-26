#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/vfs.h"
#include "fs/fat16/fat16_fs.h"
#include "fs/unix/unix_fs.h"
#include "shell/command_registry.h"

using namespace pfs;

static const char* UNIX_DISK = "pfs_unix.img";
static const char* FAT16_DISK = "pfs_fat16.img";

static void register_commands(CommandRegistry& reg) {
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
            if (args.empty()) {
                out = "Usage: mkdir <path>";
                return -1;
            }
            int ret = fs.fs_mkdir(args[0].c_str());
            if (ret != 0) out = "mkdir: failed";
            return ret;
        },
        "mkdir <path> — create directory");

    reg.register_cmd(
        "rmdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: rmdir <path>";
                return -1;
            }
            int ret = fs.fs_rmdir(args[0].c_str());
            if (ret != 0) out = "rmdir: directory not empty or not found";
            return ret;
        },
        "rmdir <path> — remove empty directory");

    reg.register_cmd(
        "cd",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            const char* path = args.empty() ? "/" : args[0].c_str();
            int ret = fs.fs_chdir(path);
            if (ret != 0) out = "cd: no such directory";
            return ret;
        },
        "cd [path] — change directory");

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
            const char* path = args.empty() ? "" : args[0].c_str();
            std::vector<DirEntry> entries;
            int ret = fs.fs_ls(path, entries);
            if (ret != 0) {
                out = "ls: cannot list";
                return ret;
            }
            out.clear();
            for (auto& e : entries) {
                if (e.type == TYPE_DIR) out += "\033[32m";
                out += e.name;
                if (e.type == TYPE_DIR)
                    out += "/\033[0m";
                else
                    out += "\033[0m";
                out += "  ";
            }
            if (out.empty()) out = "(empty)";
            return 0;
        },
        "ls [path] — list directory");

    reg.register_cmd(
        "ll",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            const char* path = args.empty() ? "" : args[0].c_str();
            std::vector<DirEntry> entries;
            int ret = fs.fs_ls(path, entries);
            if (ret != 0) {
                out = "ll: cannot list";
                return ret;
            }
            out.clear();
            for (auto& e : entries) {
                FileStat st{};
                fs.fs_stat(e.name, st);
                char line[128];
                std::snprintf(line, sizeof(line), "%c%03o  %5u  %s\n",
                              (e.type == TYPE_DIR) ? 'd' : '-', st.mode & 0x1FF, st.size, e.name);
                out += line;
            }
            if (out.empty()) out = "(empty)";
            return 0;
        },
        "ll [path] — detailed listing");

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
            if (args.empty()) {
                out = "Usage: rm [-r] <path>";
                return -1;
            }
            bool recursive = false;
            std::string target;
            for (auto& a : args) {
                if (a == "-r" || a == "-rf")
                    recursive = true;
                else
                    target = a;
            }
            if (target.empty()) {
                out = "Usage: rm [-r] <path>";
                return -1;
            }
            int ret =
                recursive ? fs.fs_delete_recursive(target.c_str()) : fs.fs_delete(target.c_str());
            if (ret != 0) out = "rm: failed";
            return ret;
        },
        "rm [-r] <path> — delete file or directory");

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
                out = "open: failed";
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
            size_t len = (args.size() > 1) ? std::stoul(args[1]) : 4096;
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
                out = "Usage: cat <file>";
                return -1;
            }
            int fd = fs.fs_open(args[0].c_str(), O_READ);
            if (fd < 0) {
                out = "cat: file not found";
                return -1;
            }
            std::vector<char> buf(8192, 0);
            ssize_t n = fs.fs_read(fd, buf.data(), buf.size() - 1);
            fs.fs_close(fd);
            if (n < 0) {
                out = "cat: read error";
                return -1;
            }
            out = std::string(buf.data(), n);
            return 0;
        },
        "cat <file> — display file content");

    reg.register_cmd(
        "stat",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) {
                out = "Usage: stat <path>";
                return -1;
            }
            FileStat st{};
            int ret = fs.fs_stat(args[0].c_str(), st);
            if (ret != 0) {
                out = "stat: not found";
                return ret;
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "Type: %s\nMode: %04o\nUID: %u  GID: %u\n"
                          "Size: %u bytes\nLinks: %u",
                          st.type == TYPE_DIR ? "directory" : "file", st.mode & 0x1FF, st.uid,
                          st.gid, st.size, st.nlink);
            out = buf;
            return 0;
        },
        "stat <path> — file/directory info");

    reg.register_cmd(
        "chmod",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: chmod <mode> <path>";
                return -1;
            }
            uint16_t mode = static_cast<uint16_t>(std::stoi(args[0], nullptr, 8));
            int ret = fs.fs_chmod(args[1].c_str(), mode);
            if (ret != 0) out = "chmod: failed";
            return ret;
        },
        "chmod <mode> <path> — change permissions");

    reg.register_cmd(
        "ln",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) {
                out = "Usage: ln <src> <dst>";
                return -1;
            }
            int ret = fs.fs_link(args[0].c_str(), args[1].c_str());
            if (ret != 0) out = "ln: failed";
            return ret;
        },
        "ln <src> <dst> — create hard link");

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
                out = "cp: source not found";
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
        "exit",
        [](IFileSystem&, UserManager&, const std::vector<std::string>&, std::string& out) -> int {
            out = "__EXIT__";
            return 0;
        },
        "exit — quit PseudoFS");
}

int main(int argc, char* argv[]) {
    bool use_fat16 = false;
    bool force_format = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--fat16") == 0) use_fat16 = true;
        if (std::strcmp(argv[i], "--format") == 0) force_format = true;
    }

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

    CommandRegistry reg;
    register_commands(reg);

    std::printf("PseudoFS v2.0 [%s] — type 'help' for commands, 'exit' to quit\n",
                fs->fs_type_name().c_str());

    char input[1024];
    while (true) {
        std::printf("\033[32m%s\033[0m$ ", fs->fs_pwd().c_str());
        std::fflush(stdout);
        if (std::fgets(input, sizeof(input), stdin) == nullptr) break;

        size_t len = std::strlen(input);
        if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
        if (input[0] == '\0') continue;

        std::string output;
        alignas(64) char um_buf[128];
        UserManager& um = *reinterpret_cast<UserManager*>(um_buf);
        int ret = reg.execute(input, *fs, um, output);

        if (output == "__EXIT__") break;
        if (!output.empty()) {
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
