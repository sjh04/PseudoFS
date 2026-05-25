# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

PseudoFS — OS course design project. A multi-user, multi-level directory file system simulator with dual engines (UNIX inode + FAT16) behind a VFS abstraction layer. C++17, ncurses TUI, 4-person team, 2-week sprint (weeks 13-14).

## Build

```bash
sudo apt install build-essential cmake libncurses5-dev libncursesw5-dev
mkdir build && cd build && cmake .. && make
./pfs
```

## Architecture

Five layers, top to bottom:

1. **TUI** (`src/tui/`) — ncurses panels: directory tree, disk usage, terminal, function keys
2. **Shell** (`src/shell/`) — command parsing, tokenization, dispatch
3. **VFS** (`src/core/`) — `IFileSystem` abstract interface, open file table, user/permission management
4. **FS Engines** — `src/fs/unix/` (inode + group-linked free blocks + mixed indexing) and `src/fs/fat16/` (FAT table + cluster chain). Both implement `IFileSystem`.
5. **Block Device** (`src/core/`) — virtual disk backed by a host file (`pfs.img`), block-level read/write/seek

Headers (`.h`) go in `include/<layer>/`, sources (`.cpp`) go in `src/<layer>/`.

## Key constants

@docs/项目需求.md (section 3.5) has the full table. Critical ones:

- Block size: 512 bytes
- UNIX FS: 1 boot + 1 superblock + 32 inode blocks (256 inodes × 64B) + 512 data blocks = 273KB
- inode addressing: addr[0..5] direct, addr[6..7] single-indirect (128 ptrs each), addr[8] double-indirect
- Group-linked free blocks: 50 per group
- Max users: 8, max open files per user: 20, system open file table: 40 entries

## Code style

Google C++ Style with these specifics:
- `snake_case` for functions and variables, `CamelCase` for classes/structs
- 4-space indentation (tabs → spaces)
- Braces on same line as control statements
- `.h` for headers, `.cpp` for implementation
- All code and comments in English; docs may be in Chinese

## Git workflow

- Feature branches off `main`, merge via PR
- Branch naming: `feat/<name>`, `fix/<name>`, `refactor/<name>`
- Keep PRs focused — one feature or fix per PR

## Gotchas

- CMakeLists.txt uses `GLOB_RECURSE` — after adding new `.cpp` files, re-run `cmake ..` in the build directory
- ncurses is required (no headless fallback) — install `libncursesw5-dev` for wide-char support
- Virtual disk is a flat binary file; all "disk" operations are offsets into this buffer — no real filesystem calls
- The project simulates a file system in memory; it never touches real files on the host beyond reading/writing `pfs.img`
