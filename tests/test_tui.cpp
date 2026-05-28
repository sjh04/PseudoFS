#include <gtest/gtest.h>

#include "shell/command_registry.h"
#include "tui/tui.h"

namespace pfs {
namespace {

alignas(64) char fs_buf[256];
alignas(64) char alt_buf[256];
alignas(64) char um_buf[256];
IFileSystem& fs = *static_cast<IFileSystem*>(static_cast<void*>(fs_buf));
IFileSystem& alt = *static_cast<IFileSystem*>(static_cast<void*>(alt_buf));
UserManager& um = *static_cast<UserManager*>(static_cast<void*>(um_buf));

TEST(TuiTest, ConstructAndDestruct) {
    CommandRegistry reg;
    { Tui tui(fs, alt, um, reg); }
    SUCCEED();
}

TEST(TuiTest, MultipleConstruction) {
    CommandRegistry reg;
    Tui tui1(fs, alt, um, reg);
    Tui tui2(fs, alt, um, reg);
    SUCCEED();
}

}  // namespace
}  // namespace pfs
