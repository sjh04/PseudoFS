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

struct Tui::Windows {
    WINDOW* title_bar;
    WINDOW* tree_win;
    WINDOW* disk_win;
    WINDOW* term_box;  // bordered outer window for the terminal panel
    WINDOW* term_win;  // inner content window (derwin) — scrolls without touching the box
    WINDOW* status_bar;
    int max_y;
    int max_x;
};

static void draw_box_title(WINDOW* win, const char* title) {
    box(win, 0, 0);
    wattron(win, A_BOLD);
    mvwprintw(win, 0, 1, " %s ", title);
    wattroff(win, A_BOLD);
}

// Draw "Label ████▒▒▒▒ used/total (pct%)" using ACS block glyphs. The filled
// portion is colored by fullness (green < 70%, yellow < 90%, red otherwise).
static void draw_meter(WINDOW* win, int y, int x, const char* label, int width, uint32_t used,
                       uint32_t total) {
    if (width < 1) width = 1;
    int filled = (total > 0) ? static_cast<int>(static_cast<uint64_t>(used) * width / total) : 0;
    if (filled > width) filled = width;
    int pct = (total > 0) ? static_cast<int>(static_cast<uint64_t>(used) * 100 / total) : 0;
    int fill_pair = (pct >= 90) ? 5 : (pct >= 70 ? 3 : 1);  // red / yellow / green

    mvwprintw(win, y, x, "%-6s ", label);
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            wattron(win, COLOR_PAIR(fill_pair));
            waddch(win, ACS_BLOCK);
            wattroff(win, COLOR_PAIR(fill_pair));
        } else {
            wattron(win, COLOR_PAIR(4));
            waddch(win, ACS_CKBOARD);
            wattroff(win, COLOR_PAIR(4));
        }
    }
    wprintw(win, " %u/%u (%d%%)", used, total, pct);
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

// Strip ANSI CSI escape sequences (e.g. ls's "\033[32m") from a string. The
// shell colors some output with ANSI codes for CLI mode; ncurses would print
// them literally, so drop them before showing output in the TUI terminal.
static std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            // Skip parameter/intermediate bytes; stop on the final byte (0x40-0x7E).
            while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E)) ++i;
            // The final byte is consumed by the loop's ++i.
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Write multi-line command output, indenting EVERY line by two spaces. A single
// wprintw("  %s\n", text) indents only the first line — newlines embedded in the
// output (ls / ll / tree / cat / help) reset the cursor to column 0, leaving the
// remaining lines flush-left and ragged. Split on '\n' and indent each line.
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

// ------- Helper: recursive tree walk -------

static void walk_tree(IFileSystem& fs, const std::string& path, int& line, int max_lines,
                      WINDOW* win, std::vector<bool>& ancestors) {
    if (ancestors.size() > 6 || line >= max_lines) return;

    std::vector<DirEntry> entries;
    if (fs.fs_ls(path.c_str(), entries) != 0) return;

    // Drop "." / ".." so last-child detection (the └─ glyph) is correct.
    std::vector<DirEntry> kids;
    for (auto& e : entries) {
        if (std::strcmp(e.name, ".") == 0) continue;
        if (std::strcmp(e.name, "..") == 0) continue;
        kids.push_back(e);
    }
    // Sort: dirs first, then alphabetically.
    std::sort(kids.begin(), kids.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.type != b.type) return a.type > b.type;
        return std::string(a.name) < std::string(b.name);
    });

    for (size_t i = 0; i < kids.size(); ++i) {
        if (line >= max_lines) return;
        const DirEntry& e = kids[i];
        bool last = (i + 1 == kids.size());

        // Branch prefix: a vertical bar for each ancestor that has more
        // siblings below, then the ├─ / └─ connector for this entry.
        wmove(win, line, 1);
        wattron(win, COLOR_PAIR(4));
        for (bool ancestor_more : ancestors) {
            waddch(win, ancestor_more ? ACS_VLINE : ' ');
            waddstr(win, "  ");
        }
        waddch(win, last ? ACS_LLCORNER : ACS_LTEE);
        waddch(win, ACS_HLINE);
        waddch(win, ' ');
        wattroff(win, COLOR_PAIR(4));

        if (e.type == TYPE_DIR) {
            wattron(win, COLOR_PAIR(1) | A_BOLD);
            wprintw(win, "%s/", e.name);
            wattroff(win, COLOR_PAIR(1) | A_BOLD);
        } else if (e.type == TYPE_SYMLINK) {
            wattron(win, COLOR_PAIR(4));  // cyan, with @ marker
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

// ------- Constructor / Destructor -------

Tui::Tui(IFileSystem& fs, IFileSystem& alt_fs, UserManager& um, CommandRegistry& reg)
    : fs_(&fs), alt_fs_(&alt_fs), um_(&um), reg_(&reg), running_(false), win_(nullptr) {
}

Tui::~Tui() {
    delete win_;
}

// ------- UI draw helpers -------

void Tui::draw_title(Windows& w) {
    wbkgd(w.title_bar, COLOR_PAIR(2));
    werase(w.title_bar);  // clear stale text (e.g. longer username) to the bg
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
    // Right-aligned hint keys.
    const char* keys = "F1 Help  F2 Switch  F3 Map  F10 Exit ";
    int kx = w.max_x - static_cast<int>(std::strlen(keys)) - 1;
    if (kx > getcurx(w.title_bar) + 1) mvwprintw(w.title_bar, 0, kx, "%s", keys);
    wrefresh(w.title_bar);
}

void Tui::draw_tree(Windows& w, int start_line, int max_lines) {
    werase(w.tree_win);
    draw_box_title(w.tree_win, "Directory Tree");

    int line = start_line;
    std::string root = fs_->fs_pwd();
    // Current directory as the tree root.
    wattron(w.tree_win, COLOR_PAIR(4) | A_BOLD);
    mvwprintw(w.tree_win, line++, 1, "%s", root.c_str());
    wattroff(w.tree_win, COLOR_PAIR(4) | A_BOLD);

    std::vector<bool> ancestors;
    walk_tree(*fs_, root, line, max_lines, w.tree_win, ancestors);

    if (line == start_line + 1) {
        wattron(w.tree_win, COLOR_PAIR(4));
        mvwprintw(w.tree_win, line, 3, "(empty)");
        wattroff(w.tree_win, COLOR_PAIR(4));
    }
    wrefresh(w.tree_win);
}

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

void Tui::draw_prompt(Windows& w) {
    std::string user = um_->is_logged_in() ? um_->current_username() : "?";
    std::string path = fs_ ? fs_->fs_pwd() : "/";
    wattron(w.term_win, COLOR_PAIR(4) | A_BOLD);  // user in cyan
    wprintw(w.term_win, "%s", user.c_str());
    wattroff(w.term_win, COLOR_PAIR(4) | A_BOLD);
    wprintw(w.term_win, "@PFS:");
    wattron(w.term_win, COLOR_PAIR(1));  // path in green
    wprintw(w.term_win, "%s", path.c_str());
    wattroff(w.term_win, COLOR_PAIR(1));
    wattron(w.term_win, A_BOLD);
    wprintw(w.term_win, " $ ");
    wattroff(w.term_win, A_BOLD);
}

void Tui::redraw_all(Windows& w, const std::string& input_buf) {
    draw_title(w);
    int inner_h = tree_h(w.max_y) - 1;
    draw_tree(w, 1, inner_h);
    draw_disk(w, 1);
    draw_status(w);

    // A popup (F1 Help / F3 Disk Map) overpaints the terminal box and its
    // scrollback. The tree/disk panels above repaint via werase, but the
    // terminal region is only restored if we force it: redraw the box border
    // and touch the box so ncurses repaints the whole region — including the
    // inner cells, which term_win shares (it is a derwin of term_box) — from
    // the buffer rather than assuming the screen still matches.
    draw_box_title(w.term_box, " Terminal ");
    touchwin(w.term_box);
    wrefresh(w.term_box);

    // Term prompt
    int y, x;
    getyx(w.term_win, y, x);
    wmove(w.term_win, y, 0);
    wclrtoeol(w.term_win);
    draw_prompt(w);
    wprintw(w.term_win, "%s", input_buf.c_str());
    touchwin(w.term_win);
    wrefresh(w.term_win);
}

// ------- FS switch -------

void Tui::switch_fs() {
    if (!alt_fs_) return;
    fs_->fs_unmount();
    alt_fs_->fs_mount();
    std::swap(fs_, alt_fs_);
}

// ------- Pager (`more`) -------

void Tui::page_content(Windows& w, const std::string& content) {
    // Split into logical lines (the last one even if empty).
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
    // Files usually end in '\n', leaving a spurious trailing blank line.
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    WINDOW* pw = newwin(w.max_y, w.max_x, 0, 0);
    keypad(pw, TRUE);

    const int page_h = (w.max_y > 2) ? w.max_y - 1 : 1;  // bottom row = status
    const int total = static_cast<int>(lines.size());
    const int max_top = (total > page_h) ? total - page_h : 0;
    int top = 0;

    bool quit = false;
    while (!quit) {
        werase(pw);
        for (int i = 0; i < page_h && top + i < total; ++i) {
            std::string ln = lines[top + i];
            if (static_cast<int>(ln.size()) > w.max_x - 1)
                ln = ln.substr(0, w.max_x - 1);  // truncate to avoid line wrap
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
    delwin(pw);
}

// ------- Main loop -------

void Tui::run() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(1);  // visible cursor in the terminal input line
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
    w.term_box = newwin(term_h(w.max_y), w.max_x, 1 + tree_h(w.max_y), 0);
    w.term_win = derwin(w.term_box, term_h(w.max_y) - 2, w.max_x - 2, 1, 1);
    w.status_bar = newwin(1, w.max_x, w.max_y - 1, 0);

    keypad(w.term_win, TRUE);
    scrollok(w.term_win, TRUE);
    wtimeout(w.term_win, 1000);  // wake every 1s so the title clock ticks

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

    // Paint the blank stdscr once up front. The first stdscr refresh marks
    // every line dirty, so if it ran after the panels were drawn it would wipe
    // them (leaving only the clock-ticked title bar). Do it first; the panel
    // wrefresh() calls below then win, and later refresh()es are no-ops.
    refresh();

    // Initial draw
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
        // Park the hardware cursor at the input point before blocking on a key.
        // Panel/title refreshes (incl. the clock tick) leave it elsewhere; this
        // brings it back into the terminal window so curs_set(1) shows it there.
        wrefresh(w.term_win);
        int ch = wgetch(w.term_win);

        if (ch == ERR) {  // 1s timeout, no key: refresh the clock and keep waiting
            draw_title(w);
            continue;
        }

        switch (ch) {
            case '\n':
            case KEY_ENTER: {
                if (input_buf.empty()) break;
                cmd_history.push_back(input_buf);
                history_idx = -1;

                // The command text was already echoed live as it was typed, so
                // just advance to the next line (reprinting it would double it).
                wprintw(w.term_win, "\n");

                std::string output;
                int ret = reg_->execute(input_buf, *fs_, *um_, output);

                if (output == "__EXIT__") {
                    running_ = false;
                    break;
                }

                // `more` returns its content behind PAGER_PREFIX — show it in the
                // full-screen pager, then restore the main screen.
                const std::string pager(PAGER_PREFIX);
                if (output.rfind(pager, 0) == 0) {
                    page_content(w, strip_ansi(output.substr(pager.size())));
                    input_buf.clear();
                    redraw_all(w, input_buf);
                    refresh();
                    break;
                }

                std::string shown = strip_ansi(output);
                while (!shown.empty() && shown.back() == '\n') shown.pop_back();
                if (!shown.empty()) {
                    if (ret != 0) wattron(w.term_win, COLOR_PAIR(5));
                    print_output(w.term_win, shown);
                    if (ret != 0) wattroff(w.term_win, COLOR_PAIR(5));
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
            case KEY_F(1): {
                int popup_h = 18, popup_w = 55;
                int py = (w.max_y - popup_h) / 2;
                int px = (w.max_x - popup_w) / 2;
                WINDOW* hw = newwin(popup_h, popup_w, py, px);
                // Enable keypad so a function key (e.g. F1 to close) is read as one
                // KEY_F(1) token. Without it, F1's "\033OP" escape sequence leaks:
                // wgetch returns the bare ESC and the leftover "OP" bytes fall
                // through to the main loop and get typed into the command line.
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
            case KEY_F(2):
                switch_fs();
                wprintw(w.term_win, "\n[Switched to %s]\n", fs_->fs_type_name().c_str());
                draw_prompt(w);
                wprintw(w.term_win, "%s", input_buf.c_str());
                wrefresh(w.term_win);
                redraw_all(w, input_buf);
                refresh();
                break;
            case KEY_F(3): {  // Full-screen disk block map
                fs_->set_user(um_->current_uid(), um_->current_gid());
                std::vector<uint8_t> bmap;
                fs_->fs_block_map(bmap);

                int used = 0, freeb = 0, meta = 0;
                for (uint8_t s : bmap) {
                    if (s == BLK_USED)
                        ++used;
                    else if (s == BLK_META)
                        ++meta;
                    else
                        ++freeb;
                }

                WINDOW* dw = newwin(w.max_y, w.max_x, 0, 0);
                keypad(dw, TRUE);
                werase(dw);
                box(dw, 0, 0);
                mvwprintw(dw, 0, 2, " Disk Block Map - %s - %d blocks ",
                          fs_->fs_type_name().c_str(), static_cast<int>(bmap.size()));

                int cols = w.max_x - 4;
                if (cols < 16) cols = 16;
                const int top = 2;
                int max_rows = w.max_y - 5;  // leave room for legend + footer
                int drawn = 0;
                for (size_t i = 0; i < bmap.size(); ++i) {
                    int row = top + static_cast<int>(i) / cols;
                    if (row - top >= max_rows) break;
                    int col = 2 + static_cast<int>(i) % cols;
                    int pair;
                    chtype glyph;
                    if (bmap[i] == BLK_USED) {
                        pair = 5;  // red solid
                        glyph = ACS_BLOCK;
                    } else if (bmap[i] == BLK_META) {
                        pair = 4;  // cyan solid
                        glyph = ACS_BLOCK;
                    } else {
                        pair = 1;  // green stipple
                        glyph = ACS_CKBOARD;
                    }
                    wattron(dw, COLOR_PAIR(pair));
                    mvwaddch(dw, row, col, glyph);
                    wattroff(dw, COLOR_PAIR(pair));
                    ++drawn;
                }

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
                delwin(w.term_win);  // inner derwin before its parent
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
            case '\t': {  // Tab — complete the command name (first word) or a filename
                size_t last_space = input_buf.rfind(' ');
                std::string prefix = (last_space == std::string::npos)
                                         ? input_buf
                                         : input_buf.substr(last_space + 1);
                if (prefix.empty()) break;

                std::vector<std::string> matches;
                if (last_space == std::string::npos) {
                    // First word → complete against registered command names.
                    for (auto& c : reg_->list_commands()) {
                        if (c.first.size() >= prefix.size() &&
                            c.first.compare(0, prefix.size(), prefix) == 0) {
                            matches.push_back(c.first);
                        }
                    }
                } else {
                    // Argument → complete against entries in the current directory.
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

                // Extend input to the longest common prefix of all matches: a unique
                // candidate finishes the word; an ambiguous one fills in as far as it
                // unambiguously can.
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

                // Still ambiguous → list all candidates, then redraw prompt+input.
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
                // Accept printable ASCII *and* raw UTF-8 bytes (0x80-0xFF) so
                // multi-byte input (e.g. Chinese) reaches the command line.
                // Bound to <=255 to exclude ncurses KEY_* codes (>=256); cast via
                // unsigned char so high bytes are not sign-extended in waddch
                // (ncursesw reassembles the byte sequence into the glyph).
                if (ch >= 32 && ch <= 255 && ch != 127) {
                    unsigned char byte = static_cast<unsigned char>(ch);
                    input_buf.push_back(static_cast<char>(byte));
                    waddch(w.term_win, byte);
                    wrefresh(w.term_win);
                }
                break;
        }
    }

    delwin(w.title_bar);
    delwin(w.tree_win);
    delwin(w.disk_win);
    delwin(w.term_win);  // inner derwin before its parent
    delwin(w.term_box);
    delwin(w.status_bar);
    delete win_;
    win_ = nullptr;
    endwin();
}

}  // namespace pfs
