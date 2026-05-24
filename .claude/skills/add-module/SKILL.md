---
name: add-module
description: Scaffold a new module with header, source, and test files. Use when adding a new component (e.g., "add-module block_device core").
disable-model-invocation: true
---

Create a new module for PseudoFS. Arguments: `$ARGUMENTS` should be `<module_name> <layer>`.

- `module_name`: snake_case name (e.g., `block_device`, `super_block`, `fat16_fs`)
- `layer`: one of `core`, `fs/unix`, `fs/fat16`, `shell`, `tui`, `utils`

Steps:

1. Parse `$ARGUMENTS` into module_name and layer. If missing, ask the user.

2. Create the header file at `include/<layer>/<module_name>.h`:
   ```cpp
   #pragma once

   // Forward declarations and includes as needed

   namespace pfs {

   class ClassName {
   public:
       ClassName();
       ~ClassName();

   private:
   };

   }  // namespace pfs
   ```
   Use CamelCase for the class name derived from module_name (e.g., `block_device` → `BlockDevice`).

3. Create the source file at `src/<layer>/<module_name>.cpp`:
   ```cpp
   #include "<layer>/<module_name>.h"

   namespace pfs {

   ClassName::ClassName() {}
   ClassName::~ClassName() {}

   }  // namespace pfs
   ```

4. Create the test file at `tests/test_<module_name>.cpp`:
   ```cpp
   #include <gtest/gtest.h>
   #include "<layer>/<module_name>.h"

   TEST(ClassName, Construction) {
       pfs::ClassName obj;
       // TODO: add meaningful tests
   }
   ```

5. Remove any `.gitkeep` in the created directories.

6. Run clang-format on all created files.

7. Verify the project still builds: `cd build && cmake .. && make -j$(nproc)`

8. Report what was created and suggest what methods/members to add based on the module's role in the architecture (refer to docs/项目需求.md).
