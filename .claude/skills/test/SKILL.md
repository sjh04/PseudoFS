---
name: test
description: Build and run the GoogleTest test suite. Use after code changes to verify correctness.
---

Run the PseudoFS test suite:

1. Build the project first — run `cd build && cmake .. && make -j$(nproc)` (create `build/` if needed).
2. Run tests: `cd build && ./pfs_tests --gtest_color=yes`
3. If specific tests are requested, use `--gtest_filter=<pattern>`:
   - Single test: `./pfs_tests --gtest_filter=BlockDevice.ReadWrite`
   - All tests in a suite: `./pfs_tests --gtest_filter=BlockDevice.*`
   - Multiple patterns: `./pfs_tests --gtest_filter=BlockDevice.*:SuperBlock.*`
4. Report results: how many passed, failed, and skipped.
5. For any failures, read the failing test source and the tested source file, then explain what went wrong and suggest a fix.

You can also use `cd build && ctest --output-on-failure` for a summary view.
