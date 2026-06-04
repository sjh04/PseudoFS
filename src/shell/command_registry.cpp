#include "shell/command_registry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>

#include "core/user_manager.h"
#include "core/vfs.h"

namespace pfs {

namespace {

// 判断字符串里有没有值得展开的通配符(* 或 ?),没有就不必走 glob 这条路
bool has_glob(const std::string& s) {
    return s.find_first_of("*?") != std::string::npos;
}

// 经典的线性 glob 匹配:'*' 配任意长串,'?' 配单个字符。
// 整串锚定匹配(从头配到尾),刚好对应一个路径段。
bool glob_match(const char* p, const char* s) {
    const char* star = nullptr;
    const char* star_s = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) {
            ++p;
            ++s;
        } else if (*p == '*') {
            star = p++;  // 记下这个 '*' 和回溯位置,后面配不上就退回来
            star_s = s;
        } else if (star != nullptr) {
            p = star + 1;  // 让上一个 '*' 多吞掉 s 的一个字符再试
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

}  // namespace

// 注册一条命令:把名字、handler、usage 串成一项追加进表。
// 用 vector 而非 map,是要保住注册顺序(help 列表照这个顺序输出)。
void CommandRegistry::register_cmd(const std::string& name, CmdHandler handler,
                                   const std::string& usage) {
    CmdEntry entry;
    entry.handler = std::move(handler);
    entry.usage = usage;
    commands_.emplace_back(name, std::move(entry));
}

// 一条命令行从进来到执行的总流水线:分词 → 设当前用户身份 → glob 展开
// → 查表分发 → 记日志 → 调 handler。整条命令的执行入口都走这里。
int CommandRegistry::execute(const std::string& cmdline, IFileSystem& fs, UserManager& um,
                             std::string& output) {
    std::vector<Token> tokens = tokenize_ex(cmdline);
    if (tokens.empty()) {
        return 0;
    }

    std::string cmd_name = tokens[0].text;
    tokens.erase(tokens.begin());

    // 用当前登录用户驱动 FS 的权限层,让每条命令都以当前身份运行
    // (login/su/logout 的效果落在下一条命令上)。不这么做引擎就一直停在
    // 默认 uid(root),rwx 检查永远不触发。CLI、TUI、F2 切换后的分发都覆盖。
    fs.set_user(um.current_uid(), um.current_gid());

    // 通配符展开放在 set_user 之后(这样目录列举遵循当前用户权限),
    // 而且只对参数 token 展开 —— 命令名永远不展开。
    std::vector<std::string> args = expand_globs(fs, tokens);

    for (const auto& [name, entry] : commands_) {
        if (name == cmd_name) {
            // 任意命令带 "-h" / "--help" 都只打印 usage 而不执行 ——
            // 等于给每条命令白送了一个帮助开关(C-04)。
            for (const auto& a : args) {
                if (a == "-h" || a == "--help") {
                    output = "Usage: " + entry.usage;
                    return 0;
                }
            }

            // 执行前先把这次操作记进日志(C-05)。
            record(cmd_name, cmdline, um);

            // 兜底:一个 handler 绝不能把整个程序搞崩。好几条命令用
            // std::stoi/stoul 解析数字参数,遇到非数字输入就抛异常
            // (比如 "close abc"、对不存在的用户 "su alice")。这里若有未捕获的
            // 异常会一路冒出 TUI/CLI 主循环,直接 std::terminate() 进程 ——
            // 在 TUI 里就表现为用户莫名其妙被踢出界面。所以统一把 handler
            // 抛出的异常转成一个普通的错误返回。
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

// 导出 (命令名, usage) 列表给 help 用,顺序就是注册时的先后
std::vector<std::pair<std::string, std::string>> CommandRegistry::list_commands() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(commands_.size());
    for (const auto& [name, entry] : commands_) {
        result.emplace_back(name, entry.usage);
    }
    return result;
}

// ---- 操作日志 / 回放(C-05)----

// 开启持久化日志:绑定文件路径,并把已有日志读回内存,
// 让日志跨会话累积、上一轮会话也能被 replay。
void CommandRegistry::set_log_path(const std::string& path) {
    log_path_ = path;
    // 读回已有日志,使它跨会话累积(上一轮会话也能回放)。
    // 行格式:"<unixtime>\t<user>\t<cmdline>"。
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        LogEntry e;
        e.time = static_cast<uint32_t>(std::strtoul(line.substr(0, t1).c_str(), nullptr, 10));
        e.user = line.substr(t1 + 1, t2 - t1 - 1);
        e.cmdline = line.substr(t2 + 1);  // 剩下整行 —— 保留命令里内嵌的制表符
        log_.push_back(std::move(e));
    }
}

// 清空日志:内存清掉,同时把日志文件截断(若绑了路径)
void CommandRegistry::clear_log() {
    log_.clear();
    if (!log_path_.empty()) {
        std::ofstream out(log_path_, std::ios::trunc);  // 截断文件
    }
}

// 往日志里追加一条操作(同时写内存和文件)。
// 回放期间不重复记录,元命令也跳过 —— 否则 replay 会递归套自己。
void CommandRegistry::record(const std::string& cmd_name, const std::string& cmdline,
                             UserManager& um) {
    if (suppress_log_) return;  // 回放过程中不要重复记日志
    // 元命令不算"操作":跳过它们,日志才能聚焦在真正的文件操作上,
    // 回放也永远不会递归套进自己。
    static const std::set<std::string> skip = {"log", "replay", "help", "history", "exit"};
    if (skip.count(cmd_name)) return;

    LogEntry e;
    e.time = static_cast<uint32_t>(std::time(nullptr));
    e.user = um.is_logged_in() ? um.current_username() : "?";
    e.cmdline = cmdline;
    log_.push_back(e);

    if (!log_path_.empty()) {
        std::ofstream out(log_path_, std::ios::app);  // 追加写,关闭时 flush
        if (out) out << e.time << '\t' << e.user << '\t' << e.cmdline << '\n';
    }
}

// 把日志里的操作按顺序重新执行一遍。回放期间关掉记录,避免越replay越长。
int CommandRegistry::replay(IFileSystem& fs, UserManager& um, std::string& out) {
    // 先把命令行快照出来:execute() 回放时是关日志的,但快照同时也防止
    // 执行过程中 log_ 这个 vector 在脚下被改动。
    std::vector<std::string> cmds;
    cmds.reserve(log_.size());
    for (const auto& e : log_) cmds.push_back(e.cmdline);

    suppress_log_ = true;
    for (const auto& c : cmds) {
        std::string discard;
        execute(c, fs, um, discard);
    }
    suppress_log_ = false;

    out = "Replayed " + std::to_string(cmds.size()) + " operation(s).";
    return 0;
}

// 分词器:按空白切词,支持双引号包裹(可含空格)和反斜杠转义。
// 每个 token 还带 quoted 标记,供后面 glob 展开判断是否跳过。
std::vector<CommandRegistry::Token> CommandRegistry::tokenize_ex(const std::string& cmdline) {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t n = cmdline.size();

    while (i < n) {
        // 跳过空白
        while (i < n && (cmdline[i] == ' ' || cmdline[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        Token tok;
        tok.quoted = false;

        if (cmdline[i] == '"') {
            // 引号串 —— 一直收到右引号为止。标成 quoted,让通配符展开跳过它。
            tok.quoted = true;
            ++i;  // 跳过左引号
            while (i < n && cmdline[i] != '"') {
                if (cmdline[i] == '\\' && i + 1 < n) {
                    ++i;  // 吃掉反斜杠,解释转义
                    switch (cmdline[i]) {
                        case 'n':
                            tok.text.push_back('\n');
                            break;
                        case 't':
                            tok.text.push_back('\t');
                            break;
                        default:
                            tok.text.push_back(cmdline[i]);
                            break;  // \" \\ 之类,原样取字符
                    }
                    ++i;
                    continue;
                }
                tok.text.push_back(cmdline[i]);
                ++i;
            }
            if (i < n) ++i;  // 跳过右引号
        } else {
            // 非引号 token —— 一直收到下一个空白为止
            while (i < n && cmdline[i] != ' ' && cmdline[i] != '\t') {
                tok.text.push_back(cmdline[i]);
                ++i;
            }
        }

        tokens.push_back(std::move(tok));
    }

    return tokens;
}

// tokenize_ex 的瘦身版:只要词本身、丢掉 quoted 标记,给不关心引号的调用方用
std::vector<std::string> CommandRegistry::tokenize(const std::string& cmdline) {
    std::vector<std::string> out;
    for (auto& tok : tokenize_ex(cmdline)) {
        out.push_back(std::move(tok.text));
    }
    return out;
}

// 对参数 token 做 shell 风格通配符展开:能匹配的换成排序后的路径列表,
// 引号 token 和无通配符 token 原样放行。只对路径最后一段做匹配。
std::vector<std::string> CommandRegistry::expand_globs(IFileSystem& fs,
                                                       const std::vector<Token>& tokens) {
    std::vector<std::string> out;
    for (const auto& tok : tokens) {
        if (tok.quoted || !has_glob(tok.text)) {
            out.push_back(tok.text);
            continue;
        }

        // 只有路径最后一段当作模式串,前面的目录前缀(若有)按字面用。
        // 所以 "*.txt" 在当前目录里展开,"docs/*.log" 在 docs/ 里展开,
        // 而 "a*/b" 不会被展开。
        size_t slash = tok.text.rfind('/');
        std::string dir = (slash == std::string::npos) ? std::string() : tok.text.substr(0, slash);
        std::string pat = (slash == std::string::npos) ? tok.text : tok.text.substr(slash + 1);

        std::vector<DirEntry> entries;
        std::vector<std::string> matches;
        if (fs.fs_ls(dir.c_str(), entries) == 0) {
            for (const auto& e : entries) {
                if (std::strcmp(e.name, ".") == 0 || std::strcmp(e.name, "..") == 0) {
                    continue;
                }
                if (glob_match(pat.c_str(), e.name)) {
                    matches.push_back(dir.empty() ? std::string(e.name) : dir + "/" + e.name);
                }
            }
        }

        if (matches.empty()) {
            out.push_back(tok.text);  // nullglob 关:没匹配上就当字面模式串
        } else {
            std::sort(matches.begin(), matches.end());
            for (auto& m : matches) out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace pfs
