#include "core/user_manager.h"

#include <cstdio>
#include <cstring>

#include "core/constants.h"

namespace pfs {

namespace {
// "PUSR" 的小端字节序——用作用户表文件的标识(magic)。
constexpr uint32_t USER_MAGIC = 0x52535550;
}  // namespace

// 用户管理器初始化:预置一个 root(uid/gid=0,默认密码 root)。
// 保证表里永远至少有 root,空盘首次启动也能登录、建其他用户。
UserManager::UserManager() : user_count_(0), current_index_(-1) {
    // 预置 root 用户
    UserRecord root;
    root.uid = 0;
    root.gid = 0;
    std::strncpy(root.username, "root", PWDSIZ - 1);
    root.username[PWDSIZ - 1] = '\0';
    std::strncpy(root.password, "root", PWDSIZ - 1);
    root.password[PWDSIZ - 1] = '\0';
    users_[0] = root;
    user_count_ = 1;
}

// 登录:用户名 + 密码逐条比对,命中就记下当前用户下标并返回其 uid。
// 明文比对、密码直接存表,够课设演示用,不追求真实安全性。
int UserManager::login(const char* username, const char* password) {
    for (int i = 0; i < user_count_; ++i) {
        if (std::strncmp(users_[i].username, username, PWDSIZ) == 0 &&
            std::strncmp(users_[i].password, password, PWDSIZ) == 0) {
            current_index_ = i;
            return users_[i].uid;
        }
    }
    return -1;
}

// 免密以 root 登录,启动时用。让会话一开始就是 root,和引擎默认 uid=0
// 的上下文对齐,避免"已登出却仍按 root 处理"的不一致状态。
int UserManager::login_root() {
    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == 0) {
            current_index_ = i;
            return 0;
        }
    }
    return -1;
}

// 登出:把当前用户下标清成 -1。打开文件表的清理由上层在登出流程里另做。
int UserManager::logout() {
    if (current_index_ < 0) {
        return -1;
    }
    current_index_ = -1;
    return 0;
}

// useradd:仅 root 可建用户,人数封顶 MAX_USER,且用户名不能重名。
// 建好后若开了持久化路径就立刻落盘,和 FS 引擎的自动同步保持一致。
int UserManager::add_user(const char* username, const char* password, uint16_t uid, uint16_t gid) {
    // 只有 root(uid 0)能添加用户
    if (current_index_ < 0 || users_[current_index_].uid != 0) {
        return -1;
    }
    if (user_count_ >= MAX_USER) {
        return -1;
    }
    for (int i = 0; i < user_count_; ++i) {
        if (std::strncmp(users_[i].username, username, PWDSIZ) == 0) {
            return -1;
        }
    }
    UserRecord rec;
    rec.uid = uid;
    rec.gid = gid;
    std::strncpy(rec.username, username, PWDSIZ - 1);
    rec.username[PWDSIZ - 1] = '\0';
    std::strncpy(rec.password, password, PWDSIZ - 1);
    rec.password[PWDSIZ - 1] = '\0';
    users_[user_count_++] = rec;
    if (!persist_path_.empty()) save_to_file(persist_path_.c_str());
    return 0;
}

// passwd:root 可改任何人密码且免验旧密码,其余人只能改自己且要验旧密码。
// 改完同样视持久化设置落盘。
int UserManager::change_password(uint16_t user_id, const char* old_pw, const char* new_pw) {
    if (!is_logged_in()) return -1;

    const UserRecord* user = find_user(user_id);
    if (user == nullptr) return -1;

    bool is_root = (users_[current_index_].uid == 0);
    bool is_self = (current_uid() == user_id);

    if (!is_root && !is_self) return -1;

    // root 改别人密码:跳过旧密码校验。
    // 其余情况(含 root 改自己密码):必须核对旧密码。
    if (!(is_root && !is_self)) {
        if (std::strncmp(user->password, old_pw, PWDSIZ) != 0) return -1;
    }

    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == user_id) {
            std::strncpy(users_[i].password, new_pw, PWDSIZ - 1);
            users_[i].password[PWDSIZ - 1] = '\0';
            if (!persist_path_.empty()) save_to_file(persist_path_.c_str());
            return 0;
        }
    }
    return -1;
}

// rwx 权限检查:按 owner/group/other 三类选 mode 里对应那 3 位,
// 看 access(读/写/执行)是否被许可。root 直接放行,符合 UNIX 惯例。
bool UserManager::check_access(uint16_t user_id, uint16_t file_uid, uint16_t file_gid,
                               uint16_t file_mode, uint8_t access) const {
    // root 绕过一切权限检查
    if (user_id == 0) return true;

    uint16_t shift;
    if (user_id == file_uid) {
        shift = 6;  // 属主
    } else {
        const UserRecord* user = find_user(user_id);
        if (user != nullptr && user->gid == file_gid) {
            shift = 3;  // 同组
        } else {
            shift = 0;  // 其他
        }
    }

    return ((file_mode >> shift) & access) == access;
}

// 当前会话的 uid;未登录时回退到 0(root),配合引擎默认上下文。
uint16_t UserManager::current_uid() const {
    if (current_index_ < 0) return 0;
    return users_[current_index_].uid;
}

// 当前会话的 gid,未登录同样回退到 0。
uint16_t UserManager::current_gid() const {
    if (current_index_ < 0) return 0;
    return users_[current_index_].gid;
}

// 当前登录用户名,供 shell 提示符显示;未登录返回空串。
std::string UserManager::current_username() const {
    if (current_index_ < 0) return "";
    return std::string(users_[current_index_].username);
}

// 是否有用户登录,以 current_index_ 是否有效为准。
bool UserManager::is_logged_in() const {
    return current_index_ >= 0;
}

// 按 uid 线性查用户记录,找不到返回 nullptr。用户数最多 8 个,无需建索引。
const UserRecord* UserManager::find_user(uint16_t uid) const {
    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == uid) {
            return &users_[i];
        }
    }
    return nullptr;
}

// 按用户名查用户记录,find_user 的重载版本,供按名引用的命令用。
const UserRecord* UserManager::find_user(const char* username) const {
    for (int i = 0; i < user_count_; ++i) {
        if (std::strncmp(users_[i].username, username, PWDSIZ) == 0) {
            return &users_[i];
        }
    }
    return nullptr;
}

// su:切换当前会话到目标用户。root 切换免密码,其他人要给出目标用户密码。
int UserManager::su(uint16_t uid, const char* password) {
    const UserRecord* target = find_user(uid);
    if (target == nullptr) return -1;

    bool is_root = (current_index_ >= 0 && users_[current_index_].uid == 0);

    // root 无需密码即可 su
    if (!is_root) {
        if (password == nullptr || std::strncmp(target->password, password, PWDSIZ) != 0) {
            return -1;
        }
    }

    // 定位目标用户下标
    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == uid) {
            current_index_ = i;
            return 0;
        }
    }
    return -1;
}

// 把整张用户表写成宿主机二进制文件:magic + 用户数 + 全部记录。
// 和 pfs.img 分开存,用户信息独立于磁盘镜像持久化。
int UserManager::save_to_file(const char* path) const {
    FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) return -1;

    uint32_t magic = USER_MAGIC;
    uint32_t count = static_cast<uint32_t>(user_count_);
    bool ok = std::fwrite(&magic, sizeof(magic), 1, fp) == 1 &&
              std::fwrite(&count, sizeof(count), 1, fp) == 1 &&
              std::fwrite(users_, sizeof(UserRecord), user_count_, fp) ==
                  static_cast<size_t>(user_count_);
    std::fclose(fp);
    return ok ? 0 : -1;
}

// 从文件恢复用户表:先校验 magic 和数量,再读到临时缓冲,确认完整才覆盖。
// 这样损坏或截断的文件不会冲掉内存里至少含 root 的现有表。
int UserManager::load_from_file(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) return -1;

    uint32_t magic = 0;
    uint32_t count = 0;
    if (std::fread(&magic, sizeof(magic), 1, fp) != 1 || magic != USER_MAGIC ||
        std::fread(&count, sizeof(count), 1, fp) != 1 || count == 0 || count > MAX_USER) {
        std::fclose(fp);
        return -1;
    }

    // 先读进临时缓冲区,这样截断/损坏的文件就不会冲掉
    // 内存里的现有表(它始终至少含 root)。
    UserRecord tmp[MAX_USER];
    size_t n = std::fread(tmp, sizeof(UserRecord), count, fp);
    std::fclose(fp);
    if (n != count) return -1;

    for (uint32_t i = 0; i < count; ++i) users_[i] = tmp[i];
    user_count_ = static_cast<int>(count);
    current_index_ = -1;  // 恢复后的表初始为登出态
    return 0;
}

// 设置持久化路径,开启自动落盘。路径非空后 add_user/passwd 成功即自动保存。
void UserManager::set_persist_path(const std::string& path) {
    persist_path_ = path;
}

}  // namespace pfs
