#pragma once

#include <string>

namespace pfs {

class IFileSystem;
class UserManager;
class CommandRegistry;

class Tui {
   public:
    Tui(IFileSystem& fs, UserManager& um, CommandRegistry& reg);
    ~Tui();

    // Main loop: init ncurses, event loop, cleanup. Blocks until F10.
    void run();

    // Switch the underlying filesystem engine at runtime (F2).
    void switch_fs(IFileSystem& new_fs);

   private:
    IFileSystem* fs_;
    UserManager* um_;
    CommandRegistry* reg_;
    bool running_;

    // ncurses window pointers (null until run() is called)
    struct Windows;
    Windows* win_;
};

}  // namespace pfs
