# PseudoFS

操作系统课程设计 — 多用户多级目录文件系统模拟器（2023 级）

## Features

- **双文件系统引擎**：UNIX inode 体系 + FAT16，通过 VFS 抽象层统一，F2 一键切换
- **成组块链接法**：UNIX FS 空闲块管理（50 块/组）
- **混合索引**：直接索引 + 一次间址 + 二次间址
- **ncurses TUI**：目录树面板 / 磁盘可视化 / 终端交互
- **多用户权限**：login/logout、rwx 三级访问控制（owner/group/other）

## Quick Start

```bash
# 安装依赖
sudo apt install build-essential cmake libncurses5-dev libncursesw5-dev

# 构建
mkdir build && cd build
cmake .. && make -j$(nproc)

# 运行
./pfs                # 默认 UNIX FS
./pfs --fat16        # 使用 FAT16
./pfs --format       # 强制格式化

# 测试
./pfs_tests
```

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

| 命令 | 说明 |
|------|------|
| `login` / `logout` | 用户登录/注销 |
| `useradd` / `passwd` | 创建用户/修改密码 |
| `mkdir` / `rmdir` | 创建/删除目录 |
| `cd` / `pwd` / `ls` / `ll` | 目录导航 |
| `touch` / `open` / `close` | 创建/打开/关闭文件 |
| `read` / `write` | 读写文件 |
| `rm` / `cp` / `mv` | 删除/复制/移动 |
| `ln` / `chmod` / `stat` | 链接/权限/详情 |
| `tree` | 目录树 |
| `format` | 格式化文件系统 |
| `help` | 帮助 |

## Documentation

- [协作手册](docs/协作手册.md) — 分工、模块接口、时间线、集成规范
- [项目需求](docs/项目需求.md) — 完整需求规格、架构设计、数据结构
- [课设要求](docs/课设要求.md) — 课程 PPT 要求整理

## License

MIT
