# PseudoFS

操作系统课程设计 — 多用户多级目录文件系统模拟器（2023 级）

## Features

- **双文件系统引擎**：UNIX inode 体系 + FAT16，通过 VFS 抽象层统一，F2 一键切换
- **成组块链接法**：UNIX FS 空闲块管理（50 块/组）
- **混合索引**：直接索引 + 一次间址 + 二次间址
- **多用户权限**：login/logout/su、rwx 三级访问控制（owner/group/other），权限随登录用户实时生效
- **持久化**：文件系统与用户表均保存到宿主机文件，重启后恢复
- **ncurses TUI**：目录树 / 磁盘用量条 / 终端面板 + F3 全屏磁盘块位图

## Quick Start

```bash
# 安装依赖（Ubuntu 22.04 / 24.04 / WSL）
# 先刷新索引，否则可能 404（请求的旧版本已被镜像替换）
sudo apt-get update
sudo apt-get install -y build-essential cmake libncurses-dev

# 构建
mkdir build && cd build
cmake .. && make -j$(nproc)

# 运行
./pfs                # CLI 模式，默认 UNIX FS
./pfs --fat16        # CLI 模式，使用 FAT16
./pfs --tui          # ncurses TUI（四面板 + 功能键）
./pfs --format       # 强制格式化（同时重置用户表）

# 测试
./pfs_tests
```

预置用户 **root / root**；要创建其他用户需先 `login root root`，再 `useradd <name> <pw> <uid> <gid>`。

## Project Structure

```
PseudoFS/
├── include/              # 头文件 (.h)
│   ├── core/             #   块设备、VFS 接口、打开文件表
│   ├── fs/unix/          #   UNIX FS 引擎
│   ├── fs/fat16/         #   FAT16 FS 引擎
│   ├── shell/            #   命令解析
│   ├── tui/              #   ncurses 界面
│   └── utils/            #   工具函数
├── src/                  # 源文件 (.cpp)，与 include/ 镜像
├── tests/                # GoogleTest 测试
├── docs/                 # 文档
│   ├── 课设要求.md        #   课程要求（来自 PPT）
│   ├── 项目需求.md        #   项目需求规格
│   └── 协作手册.md        #   分工、接口、时间线
├── CMakeLists.txt
├── CLAUDE.md             # Claude Code 项目指南
└── CONTRIBUTING.md       # 开发规范
```

## Commands

| 类别 | 命令 |
|------|------|
| 用户 | `login <user> <pw>` · `logout` · `useradd <user> <pw> <uid> <gid>` · `passwd <old> <new>` · `su <uid\|name> [pw]` |
| 目录 | `mkdir [-p] <path>` · `rmdir <path>` · `cd [path]`（支持 `~`、`..`）· `pwd` · `ls [path]` · `ll [path]` · `tree [-d N] [path]` |
| 文件 | `touch <file>` · `open <file> [r\|w\|rw\|a]` · `close <fd>` · `read <fd> [len]` · `write <fd> <text>` · `cat <file>` · `more <file>` · `rm [-r] <path>` · `cp <src> <dst>` · `mv <src> <dst>` · `ln <src> <dst>` |
| 元信息/权限 | `stat <path>` · `chmod <mode> <path>` · `find <path> <name>` · `disk` |
| 系统 | `format` · `history` · `help` · `exit` |

> 读写文件是 fd 流程：`open` 返回 `fd`，再用 `read <fd>` / `write <fd> <text>`，最后 `close <fd>`。

### TUI 功能键（`--tui`）

| 按键 | 功能 |
|------|------|
| F1 | 帮助窗口 |
| F2 | 切换 UNIX FS ↔ FAT16 |
| F3 | 全屏磁盘块位图（占用/空闲/元数据） |
| F5 | 刷新界面 |
| F10 | 退出 |
| ↑ ↓ | 命令历史 |
| Tab | 文件名补全 |

## Documentation

- [协作手册](docs/协作手册.md) — 分工、模块接口、时间线、集成规范
- [项目需求](docs/项目需求.md) — 完整需求规格、架构设计、数据结构
- [课设要求](docs/课设要求.md) — 课程 PPT 要求整理

## License

MIT
