#include "tui/tui.h"

#include <ncurses.h>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

static int tree_h(int max_y) {
    return max_y * 0.55;
}
static int term_h(int max_y) {
    return max_y - tree_h(max_y) - 2;
}
static int tree_w(int max_x) {
    return max_x * 0.4;
}
static int disk_w(int max_x) {
    return max_x - tree_w(max_x);
}

static std::string make_prompt(IFileSystem* fs) {
    std::string path = fs ? fs->fs_pwd() : "/";
    return "root@PFS:" + path + " $ ";
}

Tui::Tui(IFileSystem& fs, UserManager& um, CommandRegistry& reg)
    : fs_(&fs), um_(&um), reg_(&reg), running_(false), win_(nullptr) {
}

Tui::~Tui() {
    delete win_;
}

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

    auto draw_prompt = [&]() {
        std::string p = make_prompt(fs_);
        wattron(w.term_win, COLOR_PAIR(1));
        wprintw(w.term_win, "%s", p.c_str());
        wattroff(w.term_win, COLOR_PAIR(1));
    };

    auto redraw_input_line = [&]() {
        std::string p = make_prompt(fs_);
        int y, x;
        getyx(w.term_win, y, x);
        wmove(w.term_win, y, 0);
        wclrtoeol(w.term_win);
        wprintw(w.term_win, "%s%s", p.c_str(), input_buf.c_str());
    };

    wbkgd(w.title_bar, COLOR_PAIR(2));
    mvwprintw(w.title_bar, 0, 1, " PFS v2.0 | user: root | F1 Help  F2 SwitchFS  F10 Exit ");
    wrefresh(w.title_bar);

    draw_box_title(w.tree_win, "Directory Tree");
    mvwprintw(w.tree_win, 1, 1, "(no FS mounted)");
    wrefresh(w.tree_win);

    draw_box_title(w.disk_win, "Disk Usage");
    mvwprintw(w.disk_win, 1, 1, "(no FS mounted)");
    wrefresh(w.disk_win);

    draw_box_title(w.term_win, " Terminal ");
    wmove(w.term_win, 1, 1);
    draw_prompt();
    wrefresh(w.term_win);

    wattron(w.status_bar, COLOR_PAIR(2));
    mvwprintw(w.status_bar, 0, 0, " F1 Help | F2 SwitchFS | F3 DiskView | F5 Refresh | F10 Exit ");
    wattroff(w.status_bar, COLOR_PAIR(2));
    wrefresh(w.status_bar);

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

                if (ret != 0 && !output.empty()) {
                    wattron(w.term_win, COLOR_PAIR(5));
                    wprintw(w.term_win, "  %s\n", output.c_str());
                    wattroff(w.term_win, COLOR_PAIR(5));
                } else if (!output.empty()) {
                    wprintw(w.term_win, "  %s\n", output.c_str());
                }

                input_buf.clear();
                draw_prompt();
                wrefresh(w.term_win);
                break;
            }
            case KEY_BACKSPACE:
            case 127:
            case '\b': {
                if (!input_buf.empty()) {
                    input_buf.pop_back();
                    redraw_input_line();
                }
                break;
            }
            case KEY_UP: {
                if (!cmd_history.empty()) {
                    if (history_idx == -1) {
                        saved_input = input_buf;
                        history_idx = static_cast<int>(cmd_history.size()) - 1;
                    } else if (history_idx > 0) {
                        --history_idx;
                    }
                    input_buf = cmd_history[history_idx];
                    redraw_input_line();
                }
                break;
            }
            case KEY_DOWN: {
                if (history_idx != -1) {
                    if (history_idx < static_cast<int>(cmd_history.size()) - 1) {
                        ++history_idx;
                        input_buf = cmd_history[history_idx];
                    } else {
                        history_idx = -1;
                        input_buf = saved_input;
                    }
                    redraw_input_line();
                }
                break;
            }
            case KEY_F(1): {
                int popup_h = 16, popup_w = 50;
                int popup_y = (w.max_y - popup_h) / 2;
                int popup_x = (w.max_x - popup_w) / 2;
                WINDOW* help_win = newwin(popup_h, popup_w, popup_y, popup_x);
                draw_box_title(help_win, " Help ");
                auto cmds = reg_->list_commands();
                int line = 1;
                for (auto& c : cmds) {
                    if (line >= popup_h - 1) break;
                    mvwprintw(help_win, line++, 2, "%-12s %s", c.first.c_str(), c.second.c_str());
                }
                mvwprintw(help_win, popup_h - 2, 2, "Press any key to close");
                wrefresh(help_win);
                wgetch(help_win);
                delwin(help_win);
                touchwin(w.title_bar);
                wrefresh(w.title_bar);
                touchwin(w.tree_win);
                wrefresh(w.tree_win);
                touchwin(w.disk_win);
                wrefresh(w.disk_win);
                touchwin(w.term_win);
                wrefresh(w.term_win);
                touchwin(w.status_bar);
                wrefresh(w.status_bar);
                refresh();
                break;
            }
            case KEY_F(2): {
                int y, x;
                getyx(w.term_win, y, x);
                wattron(w.term_win, COLOR_PAIR(3));
                wprintw(w.term_win, "\n[F2] FS switch — not yet available\n");
                wattroff(w.term_win, COLOR_PAIR(3));
                draw_prompt();
                wprintw(w.term_win, "%s", input_buf.c_str());
                wrefresh(w.term_win);
                break;
            }
            case KEY_F(5): {
                touchwin(w.title_bar);
                wrefresh(w.title_bar);
                touchwin(w.tree_win);
                wrefresh(w.tree_win);
                touchwin(w.disk_win);
                wrefresh(w.disk_win);
                touchwin(w.term_win);
                wrefresh(w.term_win);
                touchwin(w.status_bar);
                wrefresh(w.status_bar);
                refresh();
                break;
            }
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
                w.tree_win = newwin(tree_h(w.max_y), tree_w(w.max_x), 1, 0);
                w.disk_win = newwin(tree_h(w.max_y), disk_w(w.max_x), 1, tree_w(w.max_x));
                w.term_win = newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
                w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);
                keypad(w.term_win, TRUE);
                scrollok(w.term_win, TRUE);
                wbkgd(w.title_bar, COLOR_PAIR(2));
                mvwprintw(w.title_bar, 0, 1, " PFS v2.0 | F1 Help  F2 SwitchFS  F10 Exit ");
                wrefresh(w.title_bar);
                draw_box_title(w.tree_win, "Directory Tree");
                wrefresh(w.tree_win);
                draw_box_title(w.disk_win, "Disk Usage");
                wrefresh(w.disk_win);
                draw_box_title(w.term_win, " Terminal ");
                draw_prompt();
                wprintw(w.term_win, "%s", input_buf.c_str());
                wrefresh(w.term_win);
                wattron(w.status_bar, COLOR_PAIR(2));
                mvwprintw(w.status_bar, 0, 0,
                          " F1 Help | F2 SwitchFS | F3 DiskView | F5 Refresh | "
                          "F10 Exit ");
                wattroff(w.status_bar, COLOR_PAIR(2));
                wrefresh(w.status_bar);
                refresh();
                break;
            }
            default: {
                if (ch >= 32 && ch < 127) {
                    input_buf.push_back(static_cast<char>(ch));
                    waddch(w.term_win, static_cast<char>(ch));
                    wrefresh(w.term_win);
                }
                break;
            }
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

void Tui::switch_fs(IFileSystem& new_fs) {
    fs_ = &new_fs;
}

}  // namespace pfs
