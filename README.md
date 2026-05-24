# PseudoFS

操作系统课程设计 — 多用户多级目录文件系统模拟器

## Features

- **双文件系统引擎**：UNIX inode 体系 + FAT16，通过 VFS 抽象层统一接口
- **成组块链接法**：UNIX FS 空闲块管理
- **混合索引**：直接索引 + 一次间址 + 二次间址
- **ncurses TUI**：目录树面板 / 磁盘可视化 / 终端交互
- **多用户权限**：login/logout、rwx 访问控制

## Build

```bash
# 安装依赖
sudo apt install build-essential cmake libncurses5-dev libncursesw5-dev

# 构建
mkdir build && cd build
cmake .. && make

# 运行
./pfs
```

## Project Structure

```
PseudoFS/
├── include/              # 头文件
│   ├── core/             #   块设备层
│   ├── fs/unix/          #   UNIX FS 引擎
│   ├── fs/fat16/         #   FAT16 FS 引擎
│   ├── shell/            #   命令解析
│   ├── tui/              #   ncurses 界面
│   └── utils/            #   工具函数
├── src/                  # 源文件 (同 include 结构)
├── tests/                # 测试
├── docs/                 # 文档
└── CMakeLists.txt
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
| `tree` / `find` | 目录树/搜索 |
| `format` | 格式化文件系统 |
| `help` | 帮助 |

## License

MIT
