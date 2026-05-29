#include "tui/tui.h"

#include <ncurses.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "core/user_manager.h"
#include "core/vfs.h"
#include "shell/command_registry.h"

namespace pfs {

struct Tui::Windows {
    WINDOW* title_bar;
    WINDOW* tree_win;
    WINDOW* disk_win;
    WINDOW* term_win;
    WINDOW* status_bar;
    int max_y;
    int max_x;
};

static void draw_box_title(WINDOW* win, const char* title) {
    box(win, 0, 0);
    mvwprintw(win, 0, 1, " %s ", title);
}

static int tree_h(int max_y) { return max_y * 0.55; }
static int term_h(int max_y) { return max_y - tree_h(max_y) - 2; }
static int tree_w(int max_x) { return max_x * 0.4; }
static int disk_w(int max_x) { return max_x - tree_w(max_x); }

// ------- Helper: recursive tree walk -------

static void walk_tree(IFileSystem& fs, const std::string& path,
                      int depth, int& line, int max_lines,
                      WINDOW* win) {
    if (depth > 6 || line >= max_lines) return;

    std::vector<DirEntry> entries;
    if (fs.fs_ls(path.c_str(), entries) != 0) return;
    if (entries.empty()) return;

    // Sort: dirs first, then alphabetically
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  if (a.type != b.type) return a.type > b.type;
                  return std::string(a.name) < std::string(b.name);
              });

    std::string indent(depth * 2, ' ');

    for (auto& e : entries) {
        if (line >= max_lines) return;
        if (std::strcmp(e.name, ".") == 0) continue;
        if (std::strcmp(e.name, "..") == 0) continue;

        if (e.type == TYPE_DIR) {
            wattron(win, COLOR_PAIR(1));
            mvwprintw(win, line++, 1, "%s%s/", indent.c_str(), e.name);
            wattroff(win, COLOR_PAIR(1));
        } else {
            wattron(win, COLOR_PAIR(3));
            mvwprintw(win, line++, 1, "%s%s", indent.c_str(), e.name);
            wattroff(win, COLOR_PAIR(3));
        }

        if (e.type == TYPE_DIR && depth < 5) {
            std::string child = (path == "/") ? "/" + std::string(e.name)
                                              : path + "/" + std::string(e.name);
            walk_tree(fs, child, depth + 1, line, max_lines, win);
        }
    }
}

// ------- Constructor / Destructor -------

Tui::Tui(IFileSystem& fs, IFileSystem& alt_fs, UserManager& um,
         CommandRegistry& reg)
    : fs_(&fs), alt_fs_(&alt_fs), um_(&um), reg_(&reg),
      running_(false), win_(nullptr) {}

Tui::~Tui() { delete win_; }

// ------- UI draw helpers -------

void Tui::draw_title(Windows& w) {
    wbkgd(w.title_bar, COLOR_PAIR(2));
    std::string user = um_->is_logged_in() ? um_->current_username() : "nobody";
    std::string fs_type = fs_->fs_type_name();
    mvwprintw(w.title_bar, 0, 1,
              " PFS v2.0 | %s | user: %s | F1 Help  F2 SwitchFS  F10 Exit ",
              fs_type.c_str(), user.c_str());
    wrefresh(w.title_bar);
}

void Tui::draw_tree(Windows& w, int start_line, int max_lines) {
    werase(w.tree_win);
    draw_box_title(w.tree_win, "Directory Tree");

    int line = start_line;
    std::string root = fs_->fs_pwd();
    walk_tree(*fs_, root, 0, line, max_lines, w.tree_win);

    if (line == start_line) {
        mvwprintw(w.tree_win, line, 1, "(empty)");
    }
    wrefresh(w.tree_win);
}

void Tui::draw_disk(Windows& w, int start_line) {
    werase(w.disk_win);
    draw_box_title(w.disk_win, "Disk Usage");

    DiskUsage du = fs_->fs_disk_usage();
    int line = start_line;

    mvwprintw(w.disk_win, line++, 2, "FS: %s", fs_->fs_type_name().c_str());
    mvwprintw(w.disk_win, line++, 2, "Blocks: %u / %u", du.used_blocks,
              du.total_blocks);

    // Draw block usage bar (scaled to fit)
    int bar_w = disk_w(w.max_x) - 4;
    if (bar_w < 10) bar_w = 10;
    int used_w = (du.total_blocks > 0)
                     ? (du.used_blocks * bar_w / du.total_blocks)
                     : 0;
    mvwprintw(w.disk_win, line++, 2, "[");
    for (int i = 0; i < bar_w; ++i) {
        if (i < used_w)
            wattron(w.disk_win, COLOR_PAIR(5));
        else
            wattron(w.disk_win, COLOR_PAIR(4));
        wprintw(w.disk_win, (i < used_w) ? "#" : "-");
        if (i < used_w)
            wattroff(w.disk_win, COLOR_PAIR(5));
        else
            wattroff(w.disk_win, COLOR_PAIR(4));
    }
    wprintw(w.disk_win, "]");

    if (du.total_inodes > 0) {
        line++;
        mvwprintw(w.disk_win, line++, 2, "INodes: %u / %u", du.used_inodes,
                  du.total_inodes);
    }

    wrefresh(w.disk_win);
}

void Tui::draw_status(Windows& w) {
    wattron(w.status_bar, COLOR_PAIR(2));
    mvwprintw(w.status_bar, 0, 0,
              " F1 Help | F2 SwitchFS | F5 Refresh | F10 Exit ");
    wattroff(w.status_bar, COLOR_PAIR(2));
    wrefresh(w.status_bar);
}

void Tui::draw_prompt(Windows& w) {
    std::string user = um_->is_logged_in() ? um_->current_username() : "?";
    std::string path = fs_ ? fs_->fs_pwd() : "/";
    wattron(w.term_win, COLOR_PAIR(1));
    wprintw(w.term_win, "%s@PFS:%s $ ", user.c_str(), path.c_str());
    wattroff(w.term_win, COLOR_PAIR(1));
}

void Tui::redraw_all(Windows& w, const std::string& input_buf) {
    draw_title(w);
    int inner_h = tree_h(w.max_y) - 1;
    draw_tree(w, 1, inner_h);
    draw_disk(w, 1);
    draw_status(w);
    // Term prompt
    int y, x;
    getyx(w.term_win, y, x);
    wmove(w.term_win, y, 0);
    wclrtoeol(w.term_win);
    draw_prompt(w);
    wprintw(w.term_win, "%s", input_buf.c_str());
    wrefresh(w.term_win);
}

// ------- FS switch -------

void Tui::switch_fs() {
    if (!alt_fs_) return;
    fs_->fs_unmount();
    alt_fs_->fs_mount();
    std::swap(fs_, alt_fs_);
}

// ------- Main loop -------

void Tui::run() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    start_color();

    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_WHITE, COLOR_BLUE);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLACK);
    init_pair(5, COLOR_RED, COLOR_BLACK);

    delete win_;
    win_ = new Windows;
    auto& w = *win_;
    getmaxyx(stdscr, w.max_y, w.max_x);
    w.title_bar = newwin(1, w.max_x, 0, 0);
    w.tree_win = newwin(tree_h(w.max_y), tree_w(w.max_x), 1, 0);
    w.disk_win = newwin(tree_h(w.max_y), disk_w(w.max_x), 1, tree_w(w.max_x));
    w.term_win = newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
    w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);

    keypad(w.term_win, TRUE);
    scrollok(w.term_win, TRUE);

    std::string input_buf;
    std::vector<std::string> cmd_history;
    int history_idx = -1;
    std::string saved_input;

    auto redraw_input = [&]() {
        int y, x;
        getyx(w.term_win, y, x);
        wmove(w.term_win, y, 0);
        wclrtoeol(w.term_win);
        draw_prompt(w);
        wprintw(w.term_win, "%s", input_buf.c_str());
    };

    // Initial draw
    draw_title(w);
    draw_box_title(w.tree_win, "Directory Tree");
    draw_box_title(w.disk_win, "Disk Usage");
    draw_box_title(w.term_win, " Terminal ");
    draw_prompt(w);
    wrefresh(w.term_win);
    draw_status(w);
    draw_tree(w, 1, tree_h(w.max_y) - 1);
    draw_disk(w, 1);
    refresh();

    running_ = true;
    while (running_) {
        int ch = wgetch(w.term_win);

        switch (ch) {
        case '\n':
        case KEY_ENTER: {
            if (input_buf.empty()) break;
            cmd_history.push_back(input_buf);
            history_idx = -1;

            wattron(w.term_win, COLOR_PAIR(3));
            wprintw(w.term_win, "%s\n", input_buf.c_str());
            wattroff(w.term_win, COLOR_PAIR(3));

            std::string output;
            int ret = reg_->execute(input_buf, *fs_, *um_, output);

            if (output == "__EXIT__") {
                running_ = false;
                break;
            }

            if (ret != 0 && !output.empty()) {
                wattron(w.term_win, COLOR_PAIR(5));
                wprintw(w.term_win, "  %s\n", output.c_str());
                wattroff(w.term_win, COLOR_PAIR(5));
            } else if (!output.empty()) {
                wprintw(w.term_win, "  %s\n", output.c_str());
            }

            // Refresh the side panels so the tree / disk / title reflect what
            // the command just did (mkdir, cd, rm, ...). Sync the engine to the
            // current user first so the tree shows the logged-in user's view
            // immediately after login/su.
            fs_->set_user(um_->current_uid(), um_->current_gid());
            draw_title(w);
            draw_tree(w, 1, tree_h(w.max_y) - 1);
            draw_disk(w, 1);

            input_buf.clear();
            draw_prompt(w);
            wrefresh(w.term_win);
            break;
        }
        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (!input_buf.empty()) {
                input_buf.pop_back();
                redraw_input();
            }
            break;
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
        case KEY_DOWN:
            if (history_idx != -1) {
                if (history_idx <
                    static_cast<int>(cmd_history.size()) - 1) {
                    ++history_idx;
                    input_buf = cmd_history[history_idx];
                } else {
                    history_idx = -1;
                    input_buf = saved_input;
                }
                redraw_input();
            }
            break;
        case KEY_F(1): {
            int popup_h = 18, popup_w = 55;
            int py = (w.max_y - popup_h) / 2;
            int px = (w.max_x - popup_w) / 2;
            WINDOW* hw = newwin(popup_h, popup_w, py, px);
            draw_box_title(hw, " Help ");
            auto cmds = reg_->list_commands();
            int l = 1;
            for (auto& c : cmds) {
                if (l >= popup_h - 1) break;
                mvwprintw(hw, l++, 2, "%-12s %s", c.first.c_str(),
                          c.second.c_str());
            }
            mvwprintw(hw, popup_h - 2, 2, "Press any key to close");
            wrefresh(hw);
            wgetch(hw);
            delwin(hw);
            redraw_all(w, input_buf);
            refresh();
            break;
        }
        case KEY_F(2):
            switch_fs();
            wprintw(w.term_win, "\n[Switched to %s]\n",
                    fs_->fs_type_name().c_str());
            draw_prompt(w);
            wprintw(w.term_win, "%s", input_buf.c_str());
            wrefresh(w.term_win);
            redraw_all(w, input_buf);
            refresh();
            break;
        case KEY_F(5):
            redraw_all(w, input_buf);
            refresh();
            break;
        case KEY_F(10):
            running_ = false;
            break;
        case KEY_RESIZE: {
            delwin(w.title_bar);
            delwin(w.tree_win);
            delwin(w.disk_win);
            delwin(w.term_win);
            delwin(w.status_bar);
            endwin();
            refresh();
            getmaxyx(stdscr, w.max_y, w.max_x);
            w.title_bar = newwin(1, w.max_x, 0, 0);
            w.tree_win =
                newwin(tree_h(w.max_y), tree_w(w.max_x), 1, 0);
            w.disk_win = newwin(tree_h(w.max_y), disk_w(w.max_x), 1,
                                tree_w(w.max_x));
            w.term_win =
                newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
            w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);
            keypad(w.term_win, TRUE);
            scrollok(w.term_win, TRUE);
            redraw_all(w, input_buf);
            refresh();
            break;
        }
        case '\t': {  // Tab — filename completion
            // Find the last word (after last space)
            size_t last_space = input_buf.rfind(' ');
            std::string prefix =
                (last_space == std::string::npos)
                    ? input_buf
                    : input_buf.substr(last_space + 1);
            if (prefix.empty()) break;

            // Get current directory listing
            std::vector<DirEntry> entries;
            if (fs_->fs_ls("", entries) == 0) {
                std::vector<std::string> matches;
                for (auto& e : entries) {
                    std::string name(e.name);
                    if (name.size() >= prefix.size() &&
                        name.compare(0, prefix.size(), prefix) == 0) {
                        matches.push_back(name);
                    }
                }
                if (matches.size() == 1) {
                    // Single match: complete
                    std::string suffix = matches[0].substr(prefix.size());
                    input_buf += suffix;
                    wprintw(w.term_win, "%s", suffix.c_str());
                    wrefresh(w.term_win);
                } else if (matches.size() > 1) {
                    // Multiple: show options
                    wprintw(w.term_win,
                            "\n");
                    for (auto& m : matches) {
                        wprintw(w.term_win, "  %s", m.c_str());
                    }
                    wprintw(w.term_win, "\n");
                    draw_prompt(w);
                    wprintw(w.term_win, "%s", input_buf.c_str());
                    wrefresh(w.term_win);
                }
            }
            break;
        }
        default:
            if (ch >= 32 && ch < 127) {
                input_buf.push_back(static_cast<char>(ch));
                waddch(w.term_win, static_cast<char>(ch));
                wrefresh(w.term_win);
            }
            break;
        }
    }

    delwin(w.title_bar);
    delwin(w.tree_win);
    delwin(w.disk_win);
    delwin(w.term_win);
    delwin(w.status_bar);
    delete win_;
    win_ = nullptr;
    endwin();
}

}  // namespace pfs
