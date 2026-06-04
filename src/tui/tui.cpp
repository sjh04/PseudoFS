#include "tui/tui.h"

#include <ncurses.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include "core/user_manager.h"
#include "core/vfs.h"
#include "shell/command_registry.h"

namespace pfs {

// 六个 ncurses 窗口的集中存放处,加上当前屏幕尺寸 max_y/max_x。
struct Tui::Windows {
    WINDOW* title_bar;
    WINDOW* tree_win;
    WINDOW* disk_win;
    WINDOW* term_box;  // 终端面板带边框的外层窗口
    WINDOW* term_win;  // 内层内容窗口(derwin)——滚动时不碰外层边框
    WINDOW* status_bar;
    int max_y;
    int max_x;
};

// 给窗口画边框 + 左上角标题。box() 画四边,mvwprintw 把标题压在顶边上。
static void draw_box_title(WINDOW* win, const char* title) {
    box(win, 0, 0);                          // 画 0,0 默认的横竖边框线
    wattron(win, A_BOLD);
    mvwprintw(win, 0, 1, " %s ", title);     // 标题写在第 0 行第 1 列,压住顶边
    wattroff(win, A_BOLD);
}

// 用 ACS 方块字符画一条 "标签 ████▒▒▒▒ used/total (pct%)" 进度条。
// 已用段按占用率配色(< 70% 绿、< 90% 黄、否则红),空段统一青色棋盘格。
static void draw_meter(WINDOW* win, int y, int x, const char* label, int width, uint32_t used,
                       uint32_t total) {
    if (width < 1) width = 1;
    int filled = (total > 0) ? static_cast<int>(static_cast<uint64_t>(used) * width / total) : 0;
    if (filled > width) filled = width;
    int pct = (total > 0) ? static_cast<int>(static_cast<uint64_t>(used) * 100 / total) : 0;
    int fill_pair = (pct >= 90) ? 5 : (pct >= 70 ? 3 : 1);  // 红 / 黄 / 绿

    mvwprintw(win, y, x, "%-6s ", label);
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            wattron(win, COLOR_PAIR(fill_pair));   // 已用:实心方块 ACS_BLOCK
            waddch(win, ACS_BLOCK);
            wattroff(win, COLOR_PAIR(fill_pair));
        } else {
            wattron(win, COLOR_PAIR(4));           // 空闲:青色棋盘格 ACS_CKBOARD
            waddch(win, ACS_CKBOARD);
            wattroff(win, COLOR_PAIR(4));
        }
    }
    wprintw(win, " %u/%u (%d%%)", used, total, pct);
}

// 一组布局尺寸计算:目录树占上方 55% 高、左侧 40% 宽,
// 终端占剩余高度,磁盘面板占剩余宽度。窗口随终端尺寸自适应。
static int tree_h(int max_y) {
    return max_y * 0.55;
}
static int term_h(int max_y) {
    return max_y - tree_h(max_y) - 2;  // 减去标题栏和状态栏各 1 行
}
static int tree_w(int max_x) {
    return max_x * 0.4;
}
static int disk_w(int max_x) {
    return max_x - tree_w(max_x);
}

// 从字符串里剥掉 ANSI CSI 转义序列(如 ls 的 "\033[32m")。
// shell 在 CLI 模式下用 ANSI 码上色,但 ncurses 会把它们原样打出来,
// 所以送进 TUI 终端前先清掉。
static std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            // 跳过参数/中间字节;遇到结束字节(0x40-0x7E)停下。
            while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E)) ++i;
            // 结束字节由循环的 ++i 顺带吃掉。
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// 打印多行命令输出,每一行都缩进两个空格。
// 单条 wprintw("  %s\n", text) 只会缩进第一行——输出里夹的换行
//(ls / ll / tree / cat / help)会把光标拉回第 0 列,后续行就贴左乱掉。
// 所以按 '\n' 切开,逐行加缩进。
static void print_output(WINDOW* win, const std::string& text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        wprintw(win, "  %s\n", text.substr(start, end - start).c_str());
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// ------- 辅助:递归遍历目录树 -------

// 深度优先遍历目录,把树状结构画进 tree_win。line 是引用参数,
// 跨递归层累加当前行号;ancestors 记录每个祖先是否还有下方兄弟,
// 用来决定画竖线还是空格。深度和行数都设了上限,防止把窗口撑爆。
static void walk_tree(IFileSystem& fs, const std::string& path, int& line, int max_lines,
                      WINDOW* win, std::vector<bool>& ancestors) {
    if (ancestors.size() > 6 || line >= max_lines) return;

    std::vector<DirEntry> entries;
    if (fs.fs_ls(path.c_str(), entries) != 0) return;

    // 去掉 "." / ".." ,这样末项判定(└─ 字符)才准确。
    std::vector<DirEntry> kids;
    for (auto& e : entries) {
        if (std::strcmp(e.name, ".") == 0) continue;
        if (std::strcmp(e.name, "..") == 0) continue;
        kids.push_back(e);
    }
    // 排序:目录在前,同类按字母序。
    std::sort(kids.begin(), kids.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.type != b.type) return a.type > b.type;
        return std::string(a.name) < std::string(b.name);
    });

    for (size_t i = 0; i < kids.size(); ++i) {
        if (line >= max_lines) return;
        const DirEntry& e = kids[i];
        bool last = (i + 1 == kids.size());

        // 分支前缀:每个还有下方兄弟的祖先画一根竖线,
        // 然后给本项画 ├─ / └─ 连接符。
        wmove(win, line, 1);
        wattron(win, COLOR_PAIR(4));
        for (bool ancestor_more : ancestors) {
            waddch(win, ancestor_more ? ACS_VLINE : ' ');  // ACS_VLINE:竖线 │
            waddstr(win, "  ");
        }
        waddch(win, last ? ACS_LLCORNER : ACS_LTEE);  // 末项 └ ,否则 ├
        waddch(win, ACS_HLINE);                       // 横线 ─
        waddch(win, ' ');
        wattroff(win, COLOR_PAIR(4));

        if (e.type == TYPE_DIR) {
            wattron(win, COLOR_PAIR(1) | A_BOLD);
            wprintw(win, "%s/", e.name);
            wattroff(win, COLOR_PAIR(1) | A_BOLD);
        } else if (e.type == TYPE_SYMLINK) {
            wattron(win, COLOR_PAIR(4));  // 青色,带 @ 标记
            wprintw(win, "%s@", e.name);
            wattroff(win, COLOR_PAIR(4));
        } else {
            wattron(win, COLOR_PAIR(3));
            wprintw(win, "%s", e.name);
            wattroff(win, COLOR_PAIR(3));
        }
        ++line;

        if (e.type == TYPE_DIR && ancestors.size() < 5) {
            std::string child =
                (path == "/") ? "/" + std::string(e.name) : path + "/" + std::string(e.name);
            ancestors.push_back(!last);
            walk_tree(fs, child, line, max_lines, win, ancestors);
            ancestors.pop_back();
        }
    }
}

// ------- 构造 / 析构 -------

// 保存两个引擎和用户/命令管理器的指针。win_ 推迟到 run() 里再建。
Tui::Tui(IFileSystem& fs, IFileSystem& alt_fs, UserManager& um, CommandRegistry& reg)
    : fs_(&fs), alt_fs_(&alt_fs), um_(&um), reg_(&reg), running_(false), win_(nullptr) {
}

// 释放窗口集合体(里面的 WINDOW* 由 run() 退出时 delwin 善后)。
Tui::~Tui() {
    delete win_;
}

// ------- 界面绘制辅助 -------

// 画顶部标题栏:版本号 + 引擎名 + 当前用户 + 时钟,右侧贴功能键提示。
// 蓝底白字(COLOR_PAIR 2),每次重画先 werase 清掉旧文字(如更长的用户名)。
void Tui::draw_title(Windows& w) {
    wbkgd(w.title_bar, COLOR_PAIR(2));  // 设置整窗背景为蓝底白字
    werase(w.title_bar);  // 清掉旧文字(如更长的用户名),还原成背景色
    std::string user = um_->is_logged_in() ? um_->current_username() : "nobody";
    std::string fs_type = fs_->fs_type_name();

    char clock[16] = "";
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    if (lt) std::strftime(clock, sizeof(clock), "%H:%M:%S", lt);

    wattron(w.title_bar, A_BOLD);
    mvwprintw(w.title_bar, 0, 1, " PFS v2.0 ");
    wattroff(w.title_bar, A_BOLD);
    wprintw(w.title_bar, "| %s | user: %s | %s ", fs_type.c_str(), user.c_str(), clock);
    // 右对齐的功能键提示。
    const char* keys = "F1 Help  F2 Switch  F3 Map  F10 Exit ";
    int kx = w.max_x - static_cast<int>(std::strlen(keys)) - 1;
    if (kx > getcurx(w.title_bar) + 1) mvwprintw(w.title_bar, 0, kx, "%s", keys);
    wrefresh(w.title_bar);  // 把改动刷到屏幕
}

// 画左上的目录树面板:以当前目录为根,递归 walk_tree 出整棵树。
// 空目录显示 (empty);每次重画都先 werase 清窗再补边框标题。
void Tui::draw_tree(Windows& w, int start_line, int max_lines) {
    werase(w.tree_win);
    draw_box_title(w.tree_win, "Directory Tree");

    int line = start_line;
    std::string root = fs_->fs_pwd();
    // 当前目录作为树根。
    wattron(w.tree_win, COLOR_PAIR(4) | A_BOLD);
    mvwprintw(w.tree_win, line++, 1, "%s", root.c_str());
    wattroff(w.tree_win, COLOR_PAIR(4) | A_BOLD);

    std::vector<bool> ancestors;
    walk_tree(*fs_, root, line, max_lines, w.tree_win, ancestors);

    if (line == start_line + 1) {  // 根之外一行没画出 → 目录为空
        wattron(w.tree_win, COLOR_PAIR(4));
        mvwprintw(w.tree_win, line, 3, "(empty)");
        wattroff(w.tree_win, COLOR_PAIR(4));
    }
    wrefresh(w.tree_win);
}

// 画右上的磁盘用量面板:引擎名 + 块/inode 两条进度条 + F3 提示。
// 用量数据从当前引擎的 fs_disk_usage() 取,FAT16 没 inode 时跳过那条。
void Tui::draw_disk(Windows& w, int start_line) {
    werase(w.disk_win);
    draw_box_title(w.disk_win, "Disk Usage");

    DiskUsage du = fs_->fs_disk_usage();
    int line = start_line;

    wattron(w.disk_win, A_BOLD);
    mvwprintw(w.disk_win, line++, 2, "%s engine", fs_->fs_type_name().c_str());
    wattroff(w.disk_win, A_BOLD);
    line++;

    int bar_w = disk_w(w.max_x) - 22;
    if (bar_w < 8) bar_w = 8;

    draw_meter(w.disk_win, line++, 2, "Block", bar_w, du.used_blocks, du.total_blocks);
    if (du.total_inodes > 0) {
        draw_meter(w.disk_win, line++, 2, "INode", bar_w, du.used_inodes, du.total_inodes);
    }

    line++;
    wattron(w.disk_win, COLOR_PAIR(4));
    mvwprintw(w.disk_win, line, 2, "Press F3 for the full block map");
    wattroff(w.disk_win, COLOR_PAIR(4));

    wrefresh(w.disk_win);
}

// 画底部状态栏:一排 [功能键 描述]。蓝底白字,键名加粗。
void Tui::draw_status(Windows& w) {
    wbkgd(w.status_bar, COLOR_PAIR(2));
    werase(w.status_bar);
    struct {
        const char* key;
        const char* desc;
    } items[] = {
        {"F1", "Help"}, {"F2", "Switch"}, {"F3", "DiskMap"}, {"F5", "Refresh"}, {"F10", "Exit"}};
    wmove(w.status_bar, 0, 1);
    for (const auto& it : items) {
        wattron(w.status_bar, A_BOLD);
        wprintw(w.status_bar, " %s", it.key);
        wattroff(w.status_bar, A_BOLD);
        wprintw(w.status_bar, " %s ", it.desc);
    }
    wrefresh(w.status_bar);
}

// 在终端窗口当前光标处打印一行提示符 "user@PFS:path $ "。
// 用户名青色、路径绿色、$ 加粗,直接续在内层 term_win 的光标后面。
void Tui::draw_prompt(Windows& w) {
    std::string user = um_->is_logged_in() ? um_->current_username() : "?";
    std::string path = fs_ ? fs_->fs_pwd() : "/";
    wattron(w.term_win, COLOR_PAIR(4) | A_BOLD);  // 用户名青色
    wprintw(w.term_win, "%s", user.c_str());
    wattroff(w.term_win, COLOR_PAIR(4) | A_BOLD);
    wprintw(w.term_win, "@PFS:");
    wattron(w.term_win, COLOR_PAIR(1));  // 路径绿色
    wprintw(w.term_win, "%s", path.c_str());
    wattroff(w.term_win, COLOR_PAIR(1));
    wattron(w.term_win, A_BOLD);
    wprintw(w.term_win, " $ ");
    wattroff(w.term_win, A_BOLD);
}

// 全量重绘:标题/树/磁盘/状态四个面板 + 终端框 + 提示符和当前输入。
// 弹窗(F1/F3)关掉后用它把被盖住的整屏恢复回来。
void Tui::redraw_all(Windows& w, const std::string& input_buf) {
    draw_title(w);
    int inner_h = tree_h(w.max_y) - 1;
    draw_tree(w, 1, inner_h);
    draw_disk(w, 1);
    draw_status(w);

    // 弹窗(F1 帮助 / F3 磁盘图)会盖住终端框和它的滚动历史。上面的树/磁盘
    // 面板靠 werase 自己重画;但终端区只有强制刷新才会恢复:重画边框标题,
    // 再 touchwin 把整块标脏,让 ncurses 从缓冲区把整个区域(含 term_win
    // 共享的内层格子,它是 term_box 的 derwin)重画一遍,而不是假定屏幕还对得上。
    draw_box_title(w.term_box, " Terminal ");
    touchwin(w.term_box);   // 标记整窗为脏,强制下次 refresh 全画
    wrefresh(w.term_box);

    // 终端提示符
    int y, x;
    getyx(w.term_win, y, x);
    wmove(w.term_win, y, 0);
    wclrtoeol(w.term_win);
    draw_prompt(w);
    wprintw(w.term_win, "%s", input_buf.c_str());
    touchwin(w.term_win);
    wrefresh(w.term_win);
}

// ------- 切换文件系统引擎 -------

// F2 切引擎的本质:换 fs_ 这个 IFileSystem* 指针。两个引擎各有独立的
// BlockDevice 和磁盘镜像,互不干扰。先把当前引擎 unmount(落盘),
// 再把备用引擎 mount(读盘),最后 swap 指针让备用变当前。
void Tui::switch_fs() {
    if (!alt_fs_) return;
    fs_->fs_unmount();      // 当前引擎落盘卸载
    alt_fs_->fs_mount();    // 备用引擎挂载
    std::swap(fs_, alt_fs_);
}

// ------- 分页器(`more`)-------

// `more` 的全屏分页:把 content 切成逻辑行,新开一个满屏窗口逐屏显示,
// 底部状态行提示按键。退出后由调用方 redraw_all 还原主界面。
void Tui::page_content(Windows& w, const std::string& content) {
    // 切成逻辑行(末行即便为空也保留)。
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
    // 文件通常以 '\n' 结尾,会多出一行空行,去掉它。
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    WINDOW* pw = newwin(w.max_y, w.max_x, 0, 0);  // 新开一个覆盖全屏的窗口
    keypad(pw, TRUE);                             // 让方向键/PgUp 等被识别为单个键码

    const int page_h = (w.max_y > 2) ? w.max_y - 1 : 1;  // 留最后一行做状态条
    const int total = static_cast<int>(lines.size());
    const int max_top = (total > page_h) ? total - page_h : 0;
    int top = 0;  // 当前页顶部对应的行号

    bool quit = false;
    while (!quit) {
        werase(pw);
        for (int i = 0; i < page_h && top + i < total; ++i) {
            std::string ln = lines[top + i];
            if (static_cast<int>(ln.size()) > w.max_x - 1)
                ln = ln.substr(0, w.max_x - 1);  // 截断,避免折行
            mvwprintw(pw, i, 0, "%s", ln.c_str());
        }
        int last = std::min(top + page_h, total);
        wattron(pw, A_REVERSE);
        mvwprintw(pw, w.max_y - 1, 0,
                  " more: %d-%d/%d   [Space/PgDn page  up/down line  b/PgUp back"
                  "  q quit] ",
                  total ? top + 1 : 0, last, total);
        wattroff(pw, A_REVERSE);
        wrefresh(pw);

        switch (wgetch(pw)) {
            case ' ':
            case KEY_NPAGE:
                top = std::min(top + page_h, max_top);
                break;
            case KEY_DOWN:
            case '\n':
            case KEY_ENTER:
                top = std::min(top + 1, max_top);
                break;
            case 'b':
            case 'B':
            case KEY_PPAGE:
                top = std::max(top - page_h, 0);
                break;
            case KEY_UP:
                top = std::max(top - 1, 0);
                break;
            case 'q':
            case 'Q':
                quit = true;
                break;
            default:
                break;
        }
    }
    delwin(pw);  // 销毁分页窗口,主界面交回调用方恢复
}

// ------- 主循环 -------

// TUI 的全部生命周期:初始化 ncurses + 调色板、按布局新建六个窗口、
// 首屏绘制,然后进事件循环分发按键(命令回车/历史/Tab/F1-F10/输入),
// 退出时逐个 delwin 并 endwin 收尾。
void Tui::run() {
    setlocale(LC_ALL, "");  // 启用本地化,宽字符(中文)才能正确显示
    initscr();              // 初始化 ncurses,进入全屏字符模式
    cbreak();               // 关行缓冲,按键立即可读
    noecho();               // 不回显,字符由我们自己画出来
    curs_set(1);            // 终端输入行显示硬件光标
    keypad(stdscr, TRUE);   // 让方向键/功能键解析成 KEY_* 键码
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);  // 开启鼠标事件
    start_color();          // 启用颜色

    // 5 组前景/背景配色,后面用 COLOR_PAIR(n) 引用。
    init_pair(1, COLOR_GREEN, COLOR_BLACK);   // 1=绿(文件/路径/空闲)
    init_pair(2, COLOR_WHITE, COLOR_BLUE);    // 2=蓝底白字(标题/状态栏)
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);  // 3=黄(普通文件/中等占用)
    init_pair(4, COLOR_CYAN, COLOR_BLACK);    // 4=青(元数据/连接线/提示)
    init_pair(5, COLOR_RED, COLOR_BLACK);     // 5=红(占用/出错/高占用)

    delete win_;
    win_ = new Windows;
    auto& w = *win_;
    getmaxyx(stdscr, w.max_y, w.max_x);  // 取当前屏幕行列数
    // 按布局函数算出的尺寸位置新建六个窗口:newwin(高, 宽, 起始y, 起始x)。
    w.title_bar = newwin(1, w.max_x, 0, 0);
    w.tree_win = newwin(tree_h(w.max_y), tree_w(w.max_x), 1, 0);
    w.disk_win = newwin(tree_h(w.max_y), disk_w(w.max_x), 1, tree_w(w.max_x));
    w.term_box = newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
    // term_win 是 term_box 内缩一圈的子窗口(derwin),滚动时不会动到外框。
    w.term_win = derwin(w.term_box, term_h(w.max_y) - 2, w.max_x - 2, 1, 1);
    w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);

    keypad(w.term_win, TRUE);
    scrollok(w.term_win, TRUE);   // 终端内容写满后自动上滚
    wtimeout(w.term_win, 1000);  // wgetch 最多阻塞 1 秒,好让标题时钟每秒走字

    std::string input_buf;
    std::vector<std::string> cmd_history;
    int history_idx = -1;
    std::string saved_input;

    // 重画当前输入行:回到行首、清到行尾、重打提示符 + input_buf。
    // 退格/历史翻阅改了 input_buf 后都靠它把整行刷新成最新内容。
    auto redraw_input = [&]() {
        int y, x;
        getyx(w.term_win, y, x);
        wmove(w.term_win, y, 0);
        wclrtoeol(w.term_win);
        draw_prompt(w);
        wprintw(w.term_win, "%s", input_buf.c_str());
    };

    // 先把空白的 stdscr 刷一次。stdscr 首次 refresh 会把每一行都标脏,
    // 若放在面板绘制之后会把它们全抹掉(只剩走时钟的标题栏)。所以先刷它,
    // 下面各面板的 wrefresh() 才压得住,之后的 refresh() 就成了空操作。
    refresh();

    // 首屏绘制
    draw_title(w);
    draw_box_title(w.tree_win, "Directory Tree");
    draw_box_title(w.disk_win, "Disk Usage");
    draw_box_title(w.term_box, " Terminal ");
    wrefresh(w.term_box);
    draw_prompt(w);
    wrefresh(w.term_win);
    draw_status(w);
    draw_tree(w, 1, tree_h(w.max_y) - 1);
    draw_disk(w, 1);
    refresh();

    running_ = true;
    while (running_) {
        // 阻塞等键前,先把硬件光标停到终端输入点。面板/标题刷新(含时钟走字)
        // 会把光标带到别处,这里再 wrefresh 一下把它拉回终端窗口,
        // curs_set(1) 的光标才显示在该出现的地方。
        wrefresh(w.term_win);
        int ch = wgetch(w.term_win);  // 取一个键(最多阻塞 1 秒)

        if (ch == ERR) {  // 1 秒超时没按键:刷新时钟后继续等
            draw_title(w);
            continue;
        }

        switch (ch) {
            // 回车:把整行命令交注册表执行,处理结果并刷新各面板。
            case '\n':
            case KEY_ENTER: {
                if (input_buf.empty()) break;
                cmd_history.push_back(input_buf);
                history_idx = -1;

                // 命令文字在输入时已经逐字回显过了,这里只需换到下一行
                //(再打一遍会重复)。
                wprintw(w.term_win, "\n");

                std::string output;
                int ret = reg_->execute(input_buf, *fs_, *um_, output);  // 交命令注册表执行

                if (output == "__EXIT__") {  // exit/quit 命令的约定返回值
                    running_ = false;
                    break;
                }

                // `more` 的内容带 PAGER_PREFIX 前缀返回——交给全屏分页器显示,
                // 看完再恢复主界面。
                const std::string pager(PAGER_PREFIX);
                if (output.rfind(pager, 0) == 0) {
                    page_content(w, strip_ansi(output.substr(pager.size())));
                    input_buf.clear();
                    redraw_all(w, input_buf);
                    refresh();
                    break;
                }

                std::string shown = strip_ansi(output);                       // 去掉 ANSI 色码
                while (!shown.empty() && shown.back() == '\n') shown.pop_back();  // 去尾部空行
                if (!shown.empty()) {
                    if (ret != 0) wattron(w.term_win, COLOR_PAIR(5));   // 出错时整段标红
                    print_output(w.term_win, shown);
                    if (ret != 0) wattroff(w.term_win, COLOR_PAIR(5));
                }

                // 刷新侧边面板,让树/磁盘/标题反映命令刚做的事(mkdir、cd、rm…)。
                // 先把引擎同步到当前用户,这样 login/su 后树立即显示该用户的视图。
                fs_->set_user(um_->current_uid(), um_->current_gid());
                draw_title(w);
                draw_tree(w, 1, tree_h(w.max_y) - 1);
                draw_disk(w, 1);

                input_buf.clear();
                draw_prompt(w);
                wrefresh(w.term_win);
                break;
            }
            // 退格:删掉输入缓冲最后一个字节,重画输入行。
            case KEY_BACKSPACE:
            case 127:
            case '\b':
                if (!input_buf.empty()) {
                    input_buf.pop_back();
                    redraw_input();
                }
                break;
            // ↑:往回翻历史命令(首次按下先存住当前未提交的输入)。
            case KEY_UP:
                if (!cmd_history.empty()) {
                    if (history_idx == -1) {
                        saved_input = input_buf;
                        history_idx = static_cast<int>(cmd_history.size()) - 1;
                    } else if (history_idx > 0) {
                        --history_idx;
                    }
                    input_buf = cmd_history[history_idx];
                    redraw_input();
                }
                break;
            // ↓:往新翻历史命令,翻到底则回到先前存住的未提交输入。
            case KEY_DOWN:
                if (history_idx != -1) {
                    if (history_idx < static_cast<int>(cmd_history.size()) - 1) {
                        ++history_idx;
                        input_buf = cmd_history[history_idx];
                    } else {
                        history_idx = -1;
                        input_buf = saved_input;
                    }
                    redraw_input();
                }
                break;
            // F1:屏幕居中弹出帮助窗口,列出所有命令及说明,按任意键关闭。
            case KEY_F(1): {
                int popup_h = 18, popup_w = 55;
                int py = (w.max_y - popup_h) / 2;
                int px = (w.max_x - popup_w) / 2;
                WINDOW* hw = newwin(popup_h, popup_w, py, px);  // 居中新开弹窗
                // 开 keypad,功能键(如关闭用的 F1)才会被读成单个 KEY_F(1) 键码。
                // 不开的话 F1 的 "\033OP" 转义序列会漏出去:wgetch 只返回裸 ESC,
                // 剩下的 "OP" 两字节落回主循环,被当成输入打进命令行。
                keypad(hw, TRUE);
                draw_box_title(hw, " Help ");
                auto cmds = reg_->list_commands();
                int l = 1;
                for (auto& c : cmds) {
                    if (l >= popup_h - 1) break;
                    mvwprintw(hw, l++, 2, "%-12s %s", c.first.c_str(), c.second.c_str());
                }
                mvwprintw(hw, popup_h - 2, 2, "Press any key to close");
                wrefresh(hw);
                wgetch(hw);
                delwin(hw);
                redraw_all(w, input_buf);
                refresh();
                break;
            }
            // F2:切换文件系统引擎(UNIX ↔ FAT16),终端打一行提示后整屏重画。
            case KEY_F(2):
                switch_fs();
                wprintw(w.term_win, "\n[Switched to %s]\n", fs_->fs_type_name().c_str());
                draw_prompt(w);
                wprintw(w.term_win, "%s", input_buf.c_str());
                wrefresh(w.term_win);
                redraw_all(w, input_buf);
                refresh();
                break;
            // F3:全屏磁盘块位图。逐块取状态上色——红=占用、青=元数据、绿=空闲。
            case KEY_F(3): {
                fs_->set_user(um_->current_uid(), um_->current_gid());
                std::vector<uint8_t> bmap;
                fs_->fs_block_map(bmap);  // 向引擎要每块的状态数组

                // 先统计三类块数,用于底部图例汇总。
                int used = 0, freeb = 0, meta = 0;
                for (uint8_t s : bmap) {
                    if (s == BLK_USED)
                        ++used;
                    else if (s == BLK_META)
                        ++meta;
                    else
                        ++freeb;
                }

                WINDOW* dw = newwin(w.max_y, w.max_x, 0, 0);  // 满屏窗口
                keypad(dw, TRUE);
                werase(dw);
                box(dw, 0, 0);  // 画外边框
                mvwprintw(dw, 0, 2, " Disk Block Map - %s - %d blocks ",
                          fs_->fs_type_name().c_str(), static_cast<int>(bmap.size()));

                int cols = w.max_x - 4;  // 每行能塞多少个块格子
                if (cols < 16) cols = 16;
                const int top = 2;
                int max_rows = w.max_y - 5;  // 给图例 + 脚注留出空间
                int drawn = 0;
                // 把每个块画成一个彩色字符,按行优先铺满网格;超出可视行数就停。
                for (size_t i = 0; i < bmap.size(); ++i) {
                    int row = top + static_cast<int>(i) / cols;
                    if (row - top >= max_rows) break;
                    int col = 2 + static_cast<int>(i) % cols;
                    int pair;
                    chtype glyph;
                    if (bmap[i] == BLK_USED) {
                        pair = 5;  // 红色实心块:已占用
                        glyph = ACS_BLOCK;
                    } else if (bmap[i] == BLK_META) {
                        pair = 4;  // 青色实心块:元数据(超级块/inode 区等)
                        glyph = ACS_BLOCK;
                    } else {
                        pair = 1;  // 绿色棋盘格:空闲
                        glyph = ACS_CKBOARD;
                    }
                    wattron(dw, COLOR_PAIR(pair));
                    mvwaddch(dw, row, col, glyph);  // 在 (row,col) 画一个带色字符
                    wattroff(dw, COLOR_PAIR(pair));
                    ++drawn;
                }

                // 底部图例:红块=used、绿棋盘=free、青块=meta,各画一个样例。
                int ly = w.max_y - 2;
                mvwprintw(dw, ly - 1, 2, "Legend:  ");
                wattron(dw, COLOR_PAIR(5));
                waddch(dw, ACS_BLOCK);
                wattroff(dw, COLOR_PAIR(5));
                wprintw(dw, " used    ");
                wattron(dw, COLOR_PAIR(1));
                waddch(dw, ACS_CKBOARD);
                wattroff(dw, COLOR_PAIR(1));
                wprintw(dw, " free    ");
                wattron(dw, COLOR_PAIR(4));
                waddch(dw, ACS_BLOCK);
                wattroff(dw, COLOR_PAIR(4));
                wprintw(dw, " meta");
                mvwprintw(dw, ly, 2, "used=%d  free=%d  meta=%d%s   Press any key to close", used,
                          freeb, meta,
                          drawn < static_cast<int>(bmap.size()) ? "  (truncated)" : "");
                wrefresh(dw);
                wgetch(dw);
                delwin(dw);
                redraw_all(w, input_buf);
                refresh();
                break;
            }
            // F5:手动整屏重画刷新。
            case KEY_F(5):
                redraw_all(w, input_buf);
                refresh();
                break;
            // F10:退出主循环,run() 随即清理收尾。
            case KEY_F(10):
                running_ = false;
                break;
            // 终端窗口尺寸变化:销毁旧窗口、重读屏幕尺寸、按新布局重建全部窗口。
            case KEY_RESIZE: {
                delwin(w.title_bar);
                delwin(w.tree_win);
                delwin(w.disk_win);
                delwin(w.term_win);  // 内层 derwin 要先于它的父窗口销毁
                delwin(w.term_box);
                delwin(w.status_bar);
                endwin();
                refresh();
                getmaxyx(stdscr, w.max_y, w.max_x);
                w.title_bar = newwin(1, w.max_x, 0, 0);
                w.tree_win = newwin(tree_h(w.max_y), tree_w(w.max_x), 1, 0);
                w.disk_win = newwin(tree_h(w.max_y), disk_w(w.max_x), 1, tree_w(w.max_x));
                w.term_box = newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
                draw_box_title(w.term_box, " Terminal ");
                wrefresh(w.term_box);
                w.term_win = derwin(w.term_box, term_h(w.max_y) - 2, w.max_x - 2, 1, 1);
                w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);
                keypad(w.term_win, TRUE);
                scrollok(w.term_win, TRUE);
                wtimeout(w.term_win, 1000);
                redraw_all(w, input_buf);
                refresh();
                break;
            }
            // Tab:补全命令名(第一个词)或文件名(后续参数)。
            case '\t': {
                size_t last_space = input_buf.rfind(' ');
                std::string prefix = (last_space == std::string::npos)
                                         ? input_buf
                                         : input_buf.substr(last_space + 1);
                if (prefix.empty()) break;

                std::vector<std::string> matches;
                if (last_space == std::string::npos) {
                    // 第一个词 → 拿已注册的命令名来匹配。
                    for (auto& c : reg_->list_commands()) {
                        if (c.first.size() >= prefix.size() &&
                            c.first.compare(0, prefix.size(), prefix) == 0) {
                            matches.push_back(c.first);
                        }
                    }
                } else {
                    // 参数 → 拿当前目录下的条目来匹配。
                    std::vector<DirEntry> entries;
                    if (fs_->fs_ls("", entries) == 0) {
                        for (auto& e : entries) {
                            std::string name(e.name);
                            if (name.size() >= prefix.size() &&
                                name.compare(0, prefix.size(), prefix) == 0) {
                                matches.push_back(name);
                            }
                        }
                    }
                }
                if (matches.empty()) break;

                // 把输入补到所有候选的最长公共前缀:唯一候选直接补完整个词,
                // 多个候选则补到还能确定的那一段为止。
                std::string common = matches[0];
                for (size_t i = 1; i < matches.size(); ++i) {
                    size_t k = 0;
                    while (k < common.size() && k < matches[i].size() &&
                           common[k] == matches[i][k]) {
                        ++k;
                    }
                    common.resize(k);
                }
                if (common.size() > prefix.size()) {
                    std::string suffix = common.substr(prefix.size());
                    input_buf += suffix;
                    wprintw(w.term_win, "%s", suffix.c_str());
                    wrefresh(w.term_win);
                }

                // 仍有歧义 → 列出全部候选,再重画提示符 + 当前输入。
                if (matches.size() > 1) {
                    wprintw(w.term_win, "\n");
                    for (auto& m : matches) wprintw(w.term_win, "  %s", m.c_str());
                    wprintw(w.term_win, "\n");
                    draw_prompt(w);
                    wprintw(w.term_win, "%s", input_buf.c_str());
                    wrefresh(w.term_win);
                }
                break;
            }
            default:
                // 接受可打印 ASCII,以及原始 UTF-8 字节(0x80-0xFF),
                // 这样多字节输入(如中文)也能进命令行。
                // 上界限到 255 以排除 ncurses 的 KEY_* 键码(>=256);用 unsigned
                // char 转,高位字节才不会在 waddch 里被符号扩展
                //(ncursesw 会把这串字节重新拼回字形)。
                if (ch >= 32 && ch <= 255 && ch != 127) {
                    unsigned char byte = static_cast<unsigned char>(ch);
                    input_buf.push_back(static_cast<char>(byte));
                    waddch(w.term_win, byte);
                    wrefresh(w.term_win);
                }
                break;
        }
    }

    // 退出收尾:逐个销毁窗口,释放 Windows,endwin 退出 ncurses 还原终端。
    delwin(w.title_bar);
    delwin(w.tree_win);
    delwin(w.disk_win);
    delwin(w.term_win);  // 内层 derwin 要先于它的父窗口销毁
    delwin(w.term_box);
    delwin(w.status_bar);
    delete win_;
    win_ = nullptr;
    endwin();
}

}  // namespace pfs
