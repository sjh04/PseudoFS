---
name: add-command
description: Add a new shell command to PseudoFS (e.g., "add-command mkdir"). Scaffolds the handler and registers it in the command dispatcher.
disable-model-invocation: true
---

Add a new command to the PseudoFS shell layer. Arguments: `$ARGUMENTS` should be the command name (e.g., `mkdir`, `ls`, `cp`).

Steps:

1. Parse `$ARGUMENTS` as the command name. If missing, ask the user.

2. Check docs/项目需求.md to understand what this command should do (section 1 for core commands, section 2 for extras). Summarize the expected behavior to the user.

3. Check if a command dispatcher/registry exists in `src/shell/`. If it does, read it to understand the registration pattern.

4. If no dispatcher exists yet, create one:
   - `include/shell/command.h` — base `Command` class or function signature typedef
   - `include/shell/command_registry.h` — registry that maps command names to handlers
   - `src/shell/command_registry.cpp` — implementation

5. Create the command handler:
   - `src/shell/cmd_<name>.cpp` — implements the command logic
   - The handler should:
     - Accept a reference to the VFS interface and a vector of string arguments
     - Validate arguments (print usage on error)
     - Call the appropriate VFS methods
     - Print output to the TUI/terminal

6. Register the new command in the dispatcher.

7. Run clang-format on all modified/created files.

8. Verify it builds: `cd build && cmake .. && make -j$(nproc)`

9. Suggest a test case for `tests/test_cmd_<name>.cpp`.

Refer to the VFS interface in docs/项目需求.md (section 3.2) for available filesystem operations.
