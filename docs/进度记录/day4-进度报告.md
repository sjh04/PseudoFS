# 第 4 天进度报告

---

## 一、实验组整体进度

- **今日任务目标：** 完成文件读写全链路与系统集成。打开文件表联动 `open`/`read`/`write`/`close`；main.cpp 集成 20 条命令并支持 `rm -r` 递归删除；FAT16 `fs_write` 重构为增量 I/O；UNIX 访问权限接入读写路径；写后 auto-sync 防丢数据；TUI 接入真实 FS 数据并支持 F2 双引擎切换。
- **已完成模块：**
  - `OpenFileTable`：系统打开文件表（40 项，记录 inode/offset/引用计数）+ 每用户打开文件表（20 个 fd）；`fs_open` 返回 fd，`fs_read`/`fs_write` 推进 offset，`fs_close` 递减引用计数
  - main.cpp：集成 20 条命令（format/ls/ll/cd/pwd/mkdir/rmdir/touch/open/read/write/close/rm/cp/ln/chmod/stat/cat/help/exit）；`rm -r` 递归删除目录
  - FAT16：`fs_write` 重构为增量 I/O，边写边按需分配簇并更新 `file_size`（修复原一次性整文件写）
  - UNIX：`open`/`read`/`write`/`delete` 前接入 rwx 权限检查
  - auto-sync：写类操作后自动落盘脏的超级块/inode/数据块，防崩溃丢数据
  - TUI：目录树/磁盘占用读取实时数据；F2 切换 UNIX↔FAT16；新增 `--tui` 启动 flag
- **进行中模块：**
  - `login`/`logout` 与权限层由登录用户驱动（当前权限检查仍用占位 uid）
  - `cp`/`ln` 语义细化、`chmod` 完整化
  - 测试用例编写、磁盘块全屏可视化
- **遇到的问题：**
  - F2 切换时两引擎若共用一个 `BlockDevice` 缓冲，面板数据互相串扰（引出"每引擎独立 BlockDevice"待办）
  - `rm -r` 递归删除时 inode 引用计数与硬链接的关系（`nlink>1` 时只减计数不真删）
  - FAT16 `file_size` 与已分配簇数的一致性（尾簇部分使用）
  - root 是否绕过权限检查（决定 root 全通过）

---

## 二、个人进度

### 组员一

- **姓名：** ______
- **整体任务：** 文件系统核心引擎
- **今日计划完成工作：** 打开文件表联动读写 + auto-sync + 配合 FAT16 写重构
- **实际完成工作：** `OpenFileTable` 系统表/用户表实现，`fs_open`/`read`/`write`/`close` 与 offset、引用计数联动；auto-sync 在写后落盘脏块；协助定位 FAT16 写问题、确认增量分配边界
- **遇到的困难及解决过程：** 系统打开文件表与用户 fd 的两级映射（用户 fd → 系统表索引 → inode）中间一层易混，画两级表关系图理清；同一文件被多次 `open` 应共享系统表项（引用计数），按 inode 号查重命中已有项后只递增 `f_count`

### 组员二

- **姓名：** ______
- **整体任务：** 用户接口与系统控制
- **今日计划完成工作：** main 集成 20 命令 + `rm -r` 递归 + `open`/`read`/`write`/`close` 命令
- **实际完成工作：** 20 条命令接入 dispatch；`rm -r` 后序递归删除子目录与文件；`open` 返回 fd 并提示、`read` 打印内容、`write` 支持追加/覆盖、`close` 释放 fd；命令错误码统一映射为可读提示
- **遇到的困难及解决过程：** `rm -r` 递归顺序——须先删子项再删父目录（后序遍历），否则父目录 inode 先释放会导致子项找不到入口；`write` 覆盖 vs 追加用 flag 区分，默认追加，覆盖时先把 inode 大小截断再写

### 组员三

- **姓名：** ______
- **整体任务：** 用户管理与访问控制
- **今日计划完成工作：** 权限检查接入读写 + root 策略 + `chmod`
- **实际完成工作：** rwx 权限校验函数（按 owner/group/other 取对应 3 位与请求操作比对）接入 `open`/`read`/`write`/`delete`；确定 root（uid=0）绕过全部检查；`chmod` 以八进制改 `i_mode` 低 9 位
- **遇到的困难及解决过程：** 当前无登录态，权限用谁的 uid 待定——先用占位 uid，约定 Day 5 接 `login` 后由 `UserManager` 提供当前用户；组权限 gid 匹配规则简化为"同 gid 即套用 group 位"

### 组员四

- **姓名：** ______
- **整体任务：** 测试文档与界面设计
- **今日计划完成工作：** TUI 接真实数据 + F2 双引擎切换 + `--tui` flag
- **实际完成工作：** 目录树面板递归读真实目录、磁盘面板读真实 inode/block 占用；F2 在两引擎间切换并重绘；`--tui` 命令行 flag 启动图形界面（默认 CLI）
- **遇到的困难及解决过程：** F2 切换后面板显示的仍是旧引擎数据——排查发现两引擎共享了底层缓冲，记入"每引擎独立 BlockDevice"待 Day 7 修；宽字符目录树缩进对齐困难，改用 ACS 线框字符绘制树枝
