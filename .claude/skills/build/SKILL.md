---
name: build
description: Configure and build PseudoFS with CMake. Run when you need to compile the project or after adding new source files.
---

Build PseudoFS:

1. Create the build directory if it doesn't exist: `mkdir -p build`
2. Run CMake from the build directory: `cd build && cmake ..`
3. Run make: `make -j$(nproc)`
4. Report any compilation errors with the file and line number.

If the build fails:
- Read the failing source file at the reported line
- Fix the error
- Rebuild (just `make -j$(nproc)` in the build directory, no need to re-run cmake unless CMakeLists.txt changed)

After a successful build, report the executable location: `build/pfs`
