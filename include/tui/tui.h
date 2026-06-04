#pragma once

#include <string>

namespace pfs {

class IFileSystem;
class UserManager;
class CommandRegistry;

// ncurses TUI 主控:六窗口布局(标题栏/目录树/磁盘/终端框+内层/状态栏)、
// 事件循环、F2 切引擎、F3 全屏磁盘块位图。只持有指针,不拥有这些对象。
class Tui {
   public:
    Tui(IFileSystem& fs, IFileSystem& alt_fs, UserManager& um, CommandRegistry& reg);
    ~Tui();

    // 主循环:初始化 ncurses → 事件循环 → 清理。阻塞直到按 F10 退出。
    void run();

   private:
    IFileSystem* fs_;       // 当前引擎(F2 与 alt_fs_ 互换)
    IFileSystem* alt_fs_;   // 后台的另一个引擎,切换前先 unmount/mount
    UserManager* um_;
    CommandRegistry* reg_;
    bool running_;

    // ncurses 窗口指针打包(run() 调用前为 null)
    struct Windows;
    Windows* win_;

    void draw_title(Windows& w);
    void draw_tree(Windows& w, int start_line, int max_lines);
    void draw_disk(Windows& w, int start_line);
    void draw_status(Windows& w);
    void draw_prompt(Windows& w);
    void redraw_all(Windows& w, const std::string& input_buf);
    void switch_fs();

    // `more` 用的全屏分页器:一屏一屏地显示 content。
    // 空格/PgDn = 翻页,↓/Enter = 下一行,b/PgUp = 上翻页,
    // ↑ = 上一行,q = 退出。
    void page_content(Windows& w, const std::string& content);
};

}  // namespace pfs
