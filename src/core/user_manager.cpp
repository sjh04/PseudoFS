#include "core/user_manager.h"

#include <cstdio>
#include <cstring>

#include "core/constants.h"

namespace pfs {

namespace {
// "PUSR" little-endian — identifies a PseudoFS user-table file.
constexpr uint32_t USER_MAGIC = 0x52535550;
}  // namespace

UserManager::UserManager() : user_count_(0), current_index_(-1) {
    // Pre-create root user
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

int UserManager::logout() {
    if (current_index_ < 0) {
        return -1;
    }
    current_index_ = -1;
    return 0;
}

int UserManager::add_user(const char* username, const char* password, uint16_t uid, uint16_t gid) {
    // Only root (uid 0) can add users
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

int UserManager::change_password(uint16_t user_id, const char* old_pw, const char* new_pw) {
    if (!is_logged_in()) return -1;

    const UserRecord* user = find_user(user_id);
    if (user == nullptr) return -1;

    bool is_root = (users_[current_index_].uid == 0);
    bool is_self = (current_uid() == user_id);

    if (!is_root && !is_self) return -1;

    // Root changing someone else's password: skip old_pw check.
    // Everyone else (including root changing own pw): verify old_pw.
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

bool UserManager::check_access(uint16_t user_id, uint16_t file_uid, uint16_t file_gid,
                               uint16_t file_mode, uint8_t access) const {
    // Root bypasses all permission checks
    if (user_id == 0) return true;

    uint16_t shift;
    if (user_id == file_uid) {
        shift = 6;  // owner
    } else {
        const UserRecord* user = find_user(user_id);
        if (user != nullptr && user->gid == file_gid) {
            shift = 3;  // group
        } else {
            shift = 0;  // other
        }
    }

    return ((file_mode >> shift) & access) == access;
}

uint16_t UserManager::current_uid() const {
    if (current_index_ < 0) return 0;
    return users_[current_index_].uid;
}

uint16_t UserManager::current_gid() const {
    if (current_index_ < 0) return 0;
    return users_[current_index_].gid;
}

std::string UserManager::current_username() const {
    if (current_index_ < 0) return "";
    return std::string(users_[current_index_].username);
}

bool UserManager::is_logged_in() const {
    return current_index_ >= 0;
}

const UserRecord* UserManager::find_user(uint16_t uid) const {
    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == uid) {
            return &users_[i];
        }
    }
    return nullptr;
}

const UserRecord* UserManager::find_user(const char* username) const {
    for (int i = 0; i < user_count_; ++i) {
        if (std::strncmp(users_[i].username, username, PWDSIZ) == 0) {
            return &users_[i];
        }
    }
    return nullptr;
}

int UserManager::su(uint16_t uid, const char* password) {
    const UserRecord* target = find_user(uid);
    if (target == nullptr) return -1;

    bool is_root = (current_index_ >= 0 && users_[current_index_].uid == 0);

    // Root can su without password
    if (!is_root) {
        if (password == nullptr || std::strncmp(target->password, password, PWDSIZ) != 0) {
            return -1;
        }
    }

    // Find target index
    for (int i = 0; i < user_count_; ++i) {
        if (users_[i].uid == uid) {
            current_index_ = i;
            return 0;
        }
    }
    return -1;
}

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

    // Read into a temp buffer first so a truncated/corrupt file cannot
    // clobber the live table (which always has at least root).
    UserRecord tmp[MAX_USER];
    size_t n = std::fread(tmp, sizeof(UserRecord), count, fp);
    std::fclose(fp);
    if (n != count) return -1;

    for (uint32_t i = 0; i < count; ++i) users_[i] = tmp[i];
    user_count_ = static_cast<int>(count);
    current_index_ = -1;  // restored table starts logged out
    return 0;
}

void UserManager::set_persist_path(const std::string& path) {
    persist_path_ = path;
}

}  // namespace pfs
