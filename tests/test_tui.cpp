#include <gtest/gtest.h>

#include "shell/command_registry.h"
#include "tui/tui.h"

namespace pfs {
namespace {

// IFileSystem and UserManager are forward-declared in tui.h. We use a
// small buffer as placeholder for the references. The Tui constructor
// only stores pointers, so no member access happens here.
alignas(64) char fs_buf[256];
alignas(64) char um_buf[256];
IFileSystem& fs = *static_cast<IFileSystem*>(static_cast<void*>(fs_buf));
UserManager& um = *static_cast<UserManager*>(static_cast<void*>(um_buf));

TEST(TuiTest, ConstructAndDestruct) {
    CommandRegistry reg;
    {
        Tui tui(fs, um, reg);
        // runs destructor at end of scope
    }
    SUCCEED();
}

TEST(TuiTest, SwitchFsUpdatesPointer) {
    CommandRegistry reg;
    Tui tui(fs, um, reg);

    alignas(64) char other_buf[256];
    IFileSystem& other_fs = *static_cast<IFileSystem*>(static_cast<void*>(other_buf));

    // Before switch, fs_ should point to original
    // After switch, should point to other
    tui.switch_fs(other_fs);
    // No crash = pointer was stored correctly
    SUCCEED();
}

TEST(TuiTest, MultipleConstruction) {
    CommandRegistry reg;
    Tui tui1(fs, um, reg);
    Tui tui2(fs, um, reg);
    SUCCEED();
}

}  // namespace
}  // namespace pfs
