#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pfs {

// 命令可以在输出最前面加上 PAGER_PREFIX,告诉调用方(TUI 或 CLI 主循环)
// 把后面的内容用分页器一屏一屏地显示,而不是一次全倒出来。开头那个控制字节
// 在正常文本和文件名里绝不会出现,所以不会和真实文件内容撞上。`more` 用它。
constexpr char PAGER_PREFIX[] = "\x01PFS_PAGE\x01";

class IFileSystem;
class UserManager;

using CmdHandler = std::function<int(IFileSystem& fs, UserManager& um,
                                     const std::vector<std::string>& args, std::string& output)>;

// 命令日志(C-05)里的一条操作记录。
struct LogEntry {
    uint32_t time;        // unix 时间戳
    std::string user;     // 谁执行的
    std::string cmdline;  // 原封不动的命令行(可逐字回放)
};

// 命令注册表 + 分发器:负责命令的注册、命令行的分词/glob 展开/查表执行,
// 以及操作日志的记录与回放。是 shell 层的核心。
class CommandRegistry {
   public:
    void register_cmd(const std::string& name, CmdHandler handler, const std::string& usage);

    int execute(const std::string& cmdline, IFileSystem& fs, UserManager& um, std::string& output);

    std::vector<std::pair<std::string, std::string>> list_commands() const;

    static std::vector<std::string> tokenize(const std::string& cmdline);

    // --- 操作日志 / 回放(C-05)---

    // 开启持久化日志:从 `path` 读回已有记录,之后命令运行时把新记录追加进去。
    void set_log_path(const std::string& path);

    // 已记录的操作,最早的在前。
    const std::vector<LogEntry>& op_log() const {
        return log_;
    }

    // 清空全部记录(若设过日志文件,也一并截断)。
    void clear_log();

    // 按顺序对 `fs`/`um` 把每条日志操作重跑一遍。回放出的命令本身不再记日志。
    // 返回 0,并往 out 写一条摘要。
    int replay(IFileSystem& fs, UserManager& um, std::string& out);

   private:
    // 注册表里一条命令的内容:执行用的 handler + 帮助显示用的 usage。
    struct CmdEntry {
        CmdHandler handler;
        std::string usage;
    };

    // 一个命令行 token,外加它是不是被双引号包过。带引号的 token 不参与 glob
    // 展开(所以 `write 0 "*"` 写进去的是一个字面星号)。
    struct Token {
        std::string text;
        bool quoted;
    };

    // 跟 tokenize() 一样,但额外报告每个 token 的引号状态。tokenize() 转调它,
    // 然后把这些标记丢掉。
    static std::vector<Token> tokenize_ex(const std::string& cmdline);

    // 拿 `tokens` 对着 `fs` 展开 shell 通配符(`*`、`?`),返回展开后的参数表。
    // 带通配符的非引号 token 被替换成排序后的匹配路径集合;一个都没匹配上就
    // 保留字面值(nullglob 关,和常见 shell 行为一致)。
    static std::vector<std::string> expand_globs(IFileSystem& fs, const std::vector<Token>& tokens);

    // 往日志追加一条操作(内存 + 文件),除非日志被抑制(回放中)或这是元命令。
    void record(const std::string& cmd_name, const std::string& cmdline, UserManager& um);

    // 按插入顺序排,不是按字母序
    std::vector<std::pair<std::string, CmdEntry>> commands_;

    std::vector<LogEntry> log_;
    std::string log_path_;
    bool suppress_log_ = false;  // replay() 运行期间为 true
};

}  // namespace pfs
