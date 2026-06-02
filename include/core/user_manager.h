#pragma once

#include <cstdint>
#include <string>

#include "core/constants.h"

namespace pfs {

constexpr uint8_t ACCESS_READ = 1;
constexpr uint8_t ACCESS_WRITE = 2;
constexpr uint8_t ACCESS_EXEC = 4;

constexpr uint32_t PWDSIZ = 12;

struct UserRecord {
    uint16_t uid;
    uint16_t gid;
    char username[PWDSIZ];
    char password[PWDSIZ];
};

class UserManager {
   public:
    UserManager();

    // Returns user_id on success, -1 on failure. Sets current user.
    int login(const char* username, const char* password);

    // Log in as root (uid 0) without a password. Used at startup so a session
    // begins as root — matching the engine's default uid=0 context — instead of
    // the inconsistent "logged out but still treated as root" state. Returns 0
    // on success, -1 if no root account exists. Survives a changed root pw.
    int login_root();

    // Logs out current user. Returns 0 on success, -1 if not logged in.
    int logout();

    // Create a new user (root only). Returns 0 on success, -1 otherwise.
    int add_user(const char* username, const char* password, uint16_t uid, uint16_t gid);

    // Change password for the given user (root may change any user's pw).
    int change_password(uint16_t user_id, const char* old_pw, const char* new_pw);

    // Check whether user_id has `access` permission (ACCESS_READ/WRITE/EXEC)
    // for a file with the given owner, group, and mode bits.
    bool check_access(uint16_t user_id, uint16_t file_uid, uint16_t file_gid, uint16_t file_mode,
                      uint8_t access) const;

    // Current session
    uint16_t current_uid() const;
    uint16_t current_gid() const;
    std::string current_username() const;
    bool is_logged_in() const;

    // Switch user. Root can su without password; others need target's pw.
    int su(uint16_t uid, const char* password);

    // Lookup
    const UserRecord* find_user(uint16_t uid) const;
    const UserRecord* find_user(const char* username) const;

    // --- Persistence ---
    // Dump the user table to a host file (binary). Returns 0 on success.
    int save_to_file(const char* path) const;
    // Restore the user table from a host file. Returns 0 on success; on
    // failure the current table is left unchanged. Leaves the session
    // logged out (current user must log in again).
    int load_from_file(const char* path);
    // Enable auto-save: once set, a successful add_user/change_password
    // flushes the table to `path`, mirroring the FS engines' auto-sync.
    void set_persist_path(const std::string& path);

   private:
    UserRecord users_[MAX_USER];
    int user_count_;
    int current_index_;         // -1 when no user logged in
    std::string persist_path_;  // empty = auto-save disabled
};

}  // namespace pfs
