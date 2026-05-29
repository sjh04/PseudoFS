#pragma once

#include <string>

namespace pfs {

class IFileSystem;
class UserManager;
class CommandRegistry;

class Tui {
public:
    Tui(IFileSystem& fs, IFileSystem& alt_fs, UserManager& um,
        CommandRegistry& reg);
    ~Tui();

    // Main loop: init ncurses, event loop, cleanup. Blocks until F10.
    void run();

private:
    IFileSystem* fs_;
    IFileSystem* alt_fs_;
    UserManager* um_;
    CommandRegistry* reg_;
    bool running_;

    // ncurses window pointers (null until run() is called)
    struct Windows;
    Windows* win_;

    void draw_title(Windows& w);
    void draw_tree(Windows& w, int start_line, int max_lines);
    void draw_disk(Windows& w, int start_line);
    void draw_status(Windows& w);
    void draw_prompt(Windows& w);
    void redraw_all(Windows& w, const std::string& input_buf);
    void switch_fs();

    // Full-screen pager for `more`: shows `content` one screenful at a time.
    // Space/PgDn = page forward, ↓/Enter = line forward, b/PgUp = page back,
    // ↑ = line back, q = quit.
    void page_content(Windows& w, const std::string& content);
};

}  // namespace pfs
