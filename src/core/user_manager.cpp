#include "core/user_manager.h"

#include <cstring>

#include "core/constants.h"

namespace pfs {

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

int UserManager::add_user(const char* username, const char* password,
                          uint16_t uid, uint16_t gid) {
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
    return 0;
}

int UserManager::change_password(uint16_t user_id, const char* old_pw,
                                 const char* new_pw) {
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
            return 0;
        }
    }
    return -1;
}

bool UserManager::check_access(uint16_t user_id, uint16_t file_uid,
                               uint16_t file_gid, uint16_t file_mode,
                               uint8_t access) const {
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

}  // namespace pfs
