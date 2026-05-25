# 开发规范

## 环境准备

```bash
sudo apt install build-essential cmake libncurses5-dev libncursesw5-dev
git clone https://github.com/sjh04/PseudoFS.git && cd PseudoFS
mkdir build && cd build && cmake .. && make -j$(nproc)
./pfs_tests   # 验证构建
```

## Git 工作流

**Feature branch → PR → merge to main。** 不要直接 push main。

```bash
git checkout main && git pull
git checkout -b feat/unix-balloc
# ... 开发 ...
git add <files> && git commit -m "feat(unix-fs): implement balloc/bfree"
git push -u origin feat/unix-balloc
gh pr create
```

### 分支命名

```
feat/<scope>-<name>       feat/unix-balloc, feat/shell-mkdir
fix/<scope>-<name>        fix/inode-refcount
test/<name>               test/superblock-alloc
docs/<name>               docs/report-q1
```

### Commit 格式

```
<type>(<scope>): <description>

type:   feat / fix / refactor / test / docs / chore
scope:  core / unix-fs / fat16 / shell / tui
```

## 代码规范

- Google C++ Style，4 空格缩进，100 字符列宽
- `snake_case` 函数/变量，`CamelCase` 类/结构体
- `.h` 头文件放 `include/<layer>/`，`.cpp` 放 `src/<layer>/`
- 代码和注释用英文，文档可用中文
- clang-format hook 已配置，编辑后自动格式化

## 添加新模块

1. 创建 `include/<layer>/<module>.h` 和 `src/<layer>/<module>.cpp`
2. 创建 `tests/test_<module>.cpp`
3. 重新构建：`cd build && cmake .. && make -j$(nproc)`

## 构建命令

```bash
cd build && cmake .. && make -j$(nproc)    # 全量构建
cd build && make -j$(nproc)                 # 增量构建
cd build && ./pfs_tests                     # 全部测试
cd build && ./pfs_tests --gtest_filter=XX.* # 指定测试
```
