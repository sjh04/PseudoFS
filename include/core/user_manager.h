#pragma once

#include <cstdint>
#include <string>

#include "core/constants.h"

namespace pfs {

// check_access() 的权限种类
constexpr uint8_t ACCESS_READ = 1;   // 读
constexpr uint8_t ACCESS_WRITE = 2;  // 写
constexpr uint8_t ACCESS_EXEC = 4;   // 执行(目录 = 可进入)

constexpr uint32_t PWDSIZ = 12;  // 用户名/密码的最大长度

// 一条用户记录:uid/gid + 用户名 + 密码(明文,够课设演示用)。
struct UserRecord {
    uint16_t uid;           // 用户号
    uint16_t gid;           // 组号
    char username[PWDSIZ];  // 用户名
    char password[PWDSIZ];  // 密码(明文)
};

// 用户管理器:登录/登出、useradd/passwd/su,以及 rwx 权限检查。
// 始终至少含一个 root,用户表可独立持久化到宿主机文件。
class UserManager {
   public:
    UserManager();

    // 成功返回 user_id,失败返回 -1,并设置当前用户。
    int login(const char* username, const char* password);

    // 免密以 root(uid 0)登录。启动时用,让会话一开始就是 root——和引擎
    // 默认 uid=0 的上下文对齐——而非"已登出却仍按 root 处理"的不一致状态。
    // 成功返回 0,无 root 账号返回 -1。即使 root 密码被改过也照样能进。
    int login_root();

    // 登出当前用户。成功返回 0,未登录返回 -1。
    int logout();

    // 新建用户(仅 root 可用)。成功返回 0,否则返回 -1。
    int add_user(const char* username, const char* password, uint16_t uid, uint16_t gid);

    // 修改指定用户的密码(root 可改任何用户的密码)。
    int change_password(uint16_t user_id, const char* old_pw, const char* new_pw);

    // 检查 user_id 对给定 owner/group/mode 的文件是否具备 `access`
    // 权限(ACCESS_READ/WRITE/EXEC)。
    bool check_access(uint16_t user_id, uint16_t file_uid, uint16_t file_gid, uint16_t file_mode,
                      uint8_t access) const;

    // 当前会话信息
    uint16_t current_uid() const;
    uint16_t current_gid() const;
    std::string current_username() const;
    bool is_logged_in() const;

    // 切换用户。root 免密码,其他人需提供目标用户的密码。
    int su(uint16_t uid, const char* password);

    // 查找
    const UserRecord* find_user(uint16_t uid) const;
    const UserRecord* find_user(const char* username) const;

    // --- 持久化 ---
    // 把用户表写成宿主机二进制文件。成功返回 0。
    int save_to_file(const char* path) const;
    // 从宿主机文件恢复用户表。成功返回 0;失败则保持现有表不变。
    // 恢复后会话处于登出态(需重新登录)。
    int load_from_file(const char* path);
    // 开启自动保存:设置后,add_user/change_password 成功即把表落盘到
    // `path`,与 FS 引擎的自动同步保持一致。
    void set_persist_path(const std::string& path);

   private:
    UserRecord users_[MAX_USER];  // 用户表
    int user_count_;              // 已注册用户数
    int current_index_;         // 当前登录用户在 users_ 中的下标,-1 表示无人登录
    std::string persist_path_;  // 为空表示关闭自动保存
};

}  // namespace pfs
