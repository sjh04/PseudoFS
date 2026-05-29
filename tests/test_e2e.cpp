#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/block_device.h"
#include "core/constants.h"
#include "core/user_manager.h"
#include "core/vfs.h"
#include "fs/fat16/fat16_fs.h"
#include "fs/unix/unix_fs.h"
#include "shell/command_registry.h"

namespace pfs {
namespace {

// ============================================================
// Full验收流程 tests — black-box via CommandRegistry
// ============================================================

struct TestHarness {
    BlockDevice dev;
    UnixFs unix_fs;
    Fat16Fs fat16_fs;
    UserManager um;
    CommandRegistry reg;
    std::string output;
    int ret = 0;

    TestHarness()
        : dev(TOTAL_BLK_NUM, BLOCK_SIZE),
          unix_fs(dev),
          fat16_fs(dev) {
        unix_fs.set_disk_path("test_e2e.img");
        fat16_fs.set_disk_path("test_e2e_fat16.img");
    }

    int exec(IFileSystem& fs, const std::string& cmd) {
        output.clear();
        ret = reg.execute(cmd, fs, um, output);
        return ret;
    }

    int exec_unix(const std::string& cmd) { return exec(unix_fs, cmd); }
    int exec_fat(const std::string& cmd) { return exec(fat16_fs, cmd); }
};

// --- Registration (mirrors main.cpp) ---

void register_all(CommandRegistry& reg) {
    reg.register_cmd(
        "login",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 2) return -1;
            int uid = um.login(args[0].c_str(), args[1].c_str());
            if (uid < 0) { out = "Login failed."; return -1; }
            out = "Welcome, " + um.current_username();
            return 0;
        }, "");
    reg.register_cmd("logout",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>&,
           std::string&) -> int { return um.logout(); }, "");
    reg.register_cmd("useradd",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size() < 4) return -1;
            uint16_t uid = static_cast<uint16_t>(std::stoi(args[2]));
            uint16_t gid = static_cast<uint16_t>(std::stoi(args[3]));
            if (um.add_user(args[0].c_str(), args[1].c_str(), uid, gid) != 0)
                return -1;
            fs.fs_mkdir("/home");
            fs.fs_mkdir(("/home/" + args[0]).c_str());
            return 0;
        }, "");
    reg.register_cmd("passwd",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.size() < 2) return -1;
            return um.change_password(um.current_uid(), args[0].c_str(),
                                      args[1].c_str());
        }, "");
    reg.register_cmd("mkdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            bool p=false; std::string path;
            for(auto& a:args){if(a=="-p")p=true;else path=a;}
            if(path.empty())return -1;
            if(!p)return fs.fs_mkdir(path.c_str());
            std::string cur; size_t pos=0;
            if(path[0]=='/'){cur="/";pos=1;}
            while(pos<path.size()){
                size_t s=path.find('/',pos);
                if(s==std::string::npos)s=path.size();
                if(s>pos){cur+=path.substr(pos,s-pos);fs.fs_mkdir(cur.c_str());cur+="/";}
                pos=s+1;
            }
            FileStat st; return fs.fs_stat(path.c_str(),st)==0?0:-1;
        }, "");
    reg.register_cmd("su",
        [](IFileSystem&, UserManager& um, const std::vector<std::string>& args,
           std::string&) -> int {
            if(args.empty())return -1;
            const UserRecord* u=um.find_user(args[0].c_str());
            if(!u){u=um.find_user(static_cast<uint16_t>(std::stoi(args[0])));}
            if(!u)return -1;
            return um.su(u->uid,args.size()>1?args[1].c_str():nullptr);
        }, "");
    reg.register_cmd("more",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if(args.empty())return -1;
            int fd=fs.fs_open(args[0].c_str(),O_READ); if(fd<0)return -1;
            std::vector<char> b(65536,0); ssize_t n=fs.fs_read(fd,b.data(),b.size()-1);
            fs.fs_close(fd); if(n<0)return -1;
            out.assign(b.data(),n); return 0;
        }, "");
    reg.register_cmd("find",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if(args.size()<2)return -1;
            std::string pat=args[1]; out.clear();
            std::function<void(const std::string&)> s=[&](const std::string& d){
                std::vector<DirEntry> es;
                if(fs.fs_ls(d.c_str(),es)!=0)return;
                for(auto& e:es){
                    if(std::strcmp(e.name,".")==0||std::strcmp(e.name,"..")==0)continue;
                    std::string f=(d=="/")?"/"+std::string(e.name):d+"/"+e.name;
                    if(std::string(e.name).find(pat)!=std::string::npos)out+=f+"\n";
                    if(e.type==TYPE_DIR)s(f);
                }
            };
            s(args[0]); if(out.empty())out="(no matches)"; return 0;
        }, "");
    reg.register_cmd("rmdir",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.empty()) return -1;
            return fs.fs_rmdir(args[0].c_str());
        }, "");
    reg.register_cmd("cd",
        [](IFileSystem& fs, UserManager& um, const std::vector<std::string>& args,
           std::string&) -> int {
            std::string p = args.empty() ? "/" : args[0];
            if (!p.empty() && p[0] == '~')
                p = "/home/" + um.current_username() + p.substr(1);
            return fs.fs_chdir(p.c_str());
        }, "");
    reg.register_cmd("pwd",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int { out = fs.fs_pwd(); return 0; }, "");
    reg.register_cmd("ls",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            std::vector<DirEntry> e;
            fs.fs_ls(args.empty() ? "" : args[0].c_str(), e);
            for (auto& d : e) { out += d.name; out += " "; }
            return 0;
        }, "");
    reg.register_cmd("touch",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.empty()) return -1;
            return fs.fs_create(args[0].c_str(), DEFAULT_MODE);
        }, "");
    reg.register_cmd("rm",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.empty()) return -1;
            bool rec = false; std::string t;
            for (auto& a : args) {
                if (a == "-r") rec = true; else t = a;
            }
            return rec ? fs.fs_delete_recursive(t.c_str())
                       : fs.fs_delete(t.c_str());
        }, "");
    reg.register_cmd("open",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) return -1;
            int fl = O_READ;
            if (args.size() > 1) {
                if (args[1]=="w") fl=O_WRITE;
                else if (args[1]=="rw") fl=O_READ|O_WRITE;
                else if (args[1]=="a") fl=O_WRITE|O_APPEND;
            }
            int fd = fs.fs_open(args[0].c_str(), fl);
            if (fd<0) return -1;
            out = std::to_string(fd); return 0;
        }, "");
    reg.register_cmd("close",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.empty()) return -1;
            return fs.fs_close(std::stoi(args[0]));
        }, "");
    reg.register_cmd("read",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.empty()) return -1;
            int fd = std::stoi(args[0]);
            size_t len = args.size()>1 ? std::stoul(args[1]) : 4096;
            std::vector<char> b(len+1,0);
            ssize_t n = fs.fs_read(fd, b.data(), len);
            if (n<0) return -1;
            out.assign(b.data(), n); return 0;
        }, "");
    reg.register_cmd("write",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size()<2) return -1;
            int fd = std::stoi(args[0]);
            std::string t; for(size_t i=1;i<args.size();++i){if(i>1)t+=" ";t+=args[i];}
            ssize_t n=fs.fs_write(fd,t.c_str(),t.size());
            if(n<0)return -1;
            out=std::to_string(n);return 0;
        }, "");
    reg.register_cmd("cp",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if (args.size()<2) return -1;
            int s=fs.fs_open(args[0].c_str(),O_READ);
            if(s<0)return -1;
            fs.fs_create(args[1].c_str(),DEFAULT_MODE);
            int d=fs.fs_open(args[1].c_str(),O_WRITE);
            if(d<0){fs.fs_close(s);return -1;}
            char b[4096]; ssize_t n;
            while((n=fs.fs_read(s,b,sizeof(b)))>0) fs.fs_write(d,b,n);
            fs.fs_close(s); fs.fs_close(d); return 0;
        }, "");
    reg.register_cmd("mv",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if (args.size()<2) return -1;
            FileStat st;
            if(fs.fs_stat(args[0].c_str(),st)!=0) return -1;
            int s=fs.fs_open(args[0].c_str(),O_READ);
            if(s<0)return -1;
            fs.fs_create(args[1].c_str(),DEFAULT_MODE);
            int d=fs.fs_open(args[1].c_str(),O_WRITE);
            if(d<0){fs.fs_close(s);return -1;}
            char b[4096]; ssize_t n;
            while((n=fs.fs_read(s,b,sizeof(b)))>0) fs.fs_write(d,b,n);
            fs.fs_close(s); fs.fs_close(d);
            fs.fs_delete(args[0].c_str()); return 0;
        }, "");
    reg.register_cmd("stat",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            if(args.empty())return -1;
            FileStat st{};
            if(fs.fs_stat(args[0].c_str(),st)!=0)return -1;
            char b[256];
            std::snprintf(b,sizeof(b),"size=%u type=%d",st.size,st.type);
            out=b;return 0;
        }, "");
    reg.register_cmd("chmod",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string&) -> int {
            if(args.size()<2)return -1;
            return fs.fs_chmod(args[1].c_str(),
                static_cast<uint16_t>(std::stoi(args[0],nullptr,8)));
        }, "");
    reg.register_cmd("disk",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>&,
           std::string& out) -> int {
            DiskUsage du=fs.fs_disk_usage();
            char b[128];
            std::snprintf(b,sizeof(b),"used=%u/%u",du.used_blocks,du.total_blocks);
            out=b;return 0;
        }, "");
    reg.register_cmd("tree",
        [](IFileSystem& fs, UserManager&, const std::vector<std::string>& args,
           std::string& out) -> int {
            int maxd=6; const char* path="";
            for(size_t i=0;i<args.size();++i){
                if(args[i]=="-d"&&i+1<args.size()){maxd=std::stoi(args[++i]);if(maxd<1)maxd=1;}
                else path=args[i].c_str();
            }
            std::vector<DirEntry> top;
            if(fs.fs_ls(path,top)!=0)return -1;
            std::function<void(const std::vector<DirEntry>&,const std::string&,int,const std::string&)>
                walk=[&](const std::vector<DirEntry>& es,const std::string& b,int d,const std::string& p){
                if(d>maxd)return;
                for(size_t j=0;j<es.size();++j){
                    auto& e=es[j];
                    if(std::strcmp(e.name,".")==0||std::strcmp(e.name,"..")==0)continue;
                    bool last=(j==es.size()-1);
                    out+=p+(last?"\\-- ":"|-- ")+e.name+(e.type==TYPE_DIR?"/":"")+"\n";
                    if(e.type==TYPE_DIR&&d<maxd){
                        std::string c=b+"/"+e.name;
                        std::vector<DirEntry> kids;
                        if(fs.fs_ls(c.c_str(),kids)==0)
                            walk(kids,c,d+1,p+(last?"    ":"|   "));
                    }
                }
            };
            walk(top,".",1,""); if(out.empty())out="(empty)"; return 0;
        }, "");
}

// ============================================================
// Test fixture
// ============================================================

class E2ETest : public ::testing::Test {
protected:
    TestHarness h;

    void SetUp() override { register_all(h.reg); }
};

// ==================== UNIX FS ====================

TEST_F(E2ETest, UnixFs_FormatAndMount) {
    EXPECT_EQ(h.unix_fs.fs_format(), 0);
    EXPECT_EQ(h.unix_fs.fs_type_name(), "UNIX");
    EXPECT_EQ(h.unix_fs.fs_pwd(), "/");
}

TEST_F(E2ETest, UnixFs_LoginLogout) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("login root root"), 0);
    EXPECT_TRUE(h.um.is_logged_in());
    EXPECT_EQ(h.exec_unix("logout"), 0);
    EXPECT_FALSE(h.um.is_logged_in());
}

TEST_F(E2ETest, UnixFs_LoginWrongPassword) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("login root wrong"), -1);
}

TEST_F(E2ETest, UnixFs_UserAddAndLogin) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("login root root"), 0);
    EXPECT_EQ(h.exec_unix("useradd alice pass1 1 1"), 0);
    EXPECT_EQ(h.exec_unix("logout"), 0);
    EXPECT_EQ(h.exec_unix("login alice pass1"), 0);
    EXPECT_EQ(h.um.current_username(), "alice");
}

TEST_F(E2ETest, UnixFs_PasswdChange) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    EXPECT_EQ(h.exec_unix("passwd root newroot"), 0);
    h.exec_unix("logout");
    EXPECT_EQ(h.exec_unix("login root root"), -1);
    EXPECT_EQ(h.exec_unix("login root newroot"), 0);
}

TEST_F(E2ETest, UnixFs_MkdirAndNavigation) {
    h.unix_fs.fs_format();
    // /home already exists after format
    EXPECT_EQ(h.exec_unix("mkdir /home/alice"), 0);
    EXPECT_EQ(h.exec_unix("cd /home/alice"), 0);
    EXPECT_EQ(h.unix_fs.fs_pwd(), "/home/alice");
    EXPECT_EQ(h.exec_unix("cd /"), 0);
    EXPECT_EQ(h.unix_fs.fs_pwd(), "/");
}

TEST_F(E2ETest, UnixFs_MkdirDuplicateFails) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("mkdir /foo"), 0);
    EXPECT_EQ(h.exec_unix("mkdir /foo"), -1);
}

TEST_F(E2ETest, UnixFs_RmdirEmpty) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /empty");
    EXPECT_EQ(h.exec_unix("rmdir /empty"), 0);
}

TEST_F(E2ETest, UnixFs_RmdirNonEmptyFails) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /dir");
    h.exec_unix("touch /dir/file.txt");
    EXPECT_EQ(h.exec_unix("rmdir /dir"), -1);
}

TEST_F(E2ETest, UnixFs_TouchAndLs) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /hello.txt");
    h.exec_unix("ls /");
    EXPECT_NE(h.output.find("hello.txt"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_WriteAndRead) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /data.bin");
    ASSERT_EQ(h.exec_unix("open /data.bin w"), 0);
    std::string fd = h.output;
    EXPECT_EQ(h.exec_unix("write " + fd + " HelloWorld"), 0);
    EXPECT_EQ(h.exec_unix("close " + fd), 0);
    ASSERT_EQ(h.exec_unix("open /data.bin"), 0);
    fd = h.output;
    EXPECT_EQ(h.exec_unix("read " + fd), 0);
    EXPECT_EQ(h.output, "HelloWorld");
}

TEST_F(E2ETest, UnixFs_WriteLargeFile) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /big.bin");
    h.exec_unix("open /big.bin w");
    std::string fd = h.output;
    // Write 2000 chars
    std::string data(2000, 'X');
    // Use direct API for large write
    ssize_t n = h.unix_fs.fs_write(std::stoi(fd), data.c_str(), 2000);
    EXPECT_EQ(n, 2000);
    h.exec_unix("close " + fd);
    h.exec_unix("open /big.bin");
    fd = h.output;
    EXPECT_EQ(h.exec_unix("read " + fd + " 5000"), 0);
    EXPECT_EQ(h.output.size(), 2000u);
}

TEST_F(E2ETest, UnixFs_DeleteFile) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /rm.txt");
    EXPECT_EQ(h.exec_unix("rm /rm.txt"), 0);
    h.exec_unix("ls /");
    EXPECT_EQ(h.output.find("rm.txt"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_DeleteRecursive) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /a");
    h.exec_unix("mkdir /a/b");
    h.exec_unix("touch /a/b/c.txt");
    EXPECT_EQ(h.exec_unix("rm -r /a"), 0);
    h.exec_unix("ls /");
    EXPECT_EQ(h.output.find("a"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_CpFile) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /src.txt");
    ASSERT_EQ(h.exec_unix("open /src.txt w"), 0);
    std::string fd = h.output;
    h.exec_unix("write " + fd + " copy_me");
    h.exec_unix("close " + fd);
    EXPECT_EQ(h.exec_unix("cp /src.txt /dst.txt"), 0);
    ASSERT_EQ(h.exec_unix("open /dst.txt"), 0);
    fd = h.output;
    h.exec_unix("read " + fd);
    EXPECT_EQ(h.output, "copy_me");
}

TEST_F(E2ETest, UnixFs_MvFile) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /old.txt");
    ASSERT_EQ(h.exec_unix("open /old.txt w"), 0);
    std::string fd = h.output;
    h.exec_unix("write " + fd + " move_me");
    h.exec_unix("close " + fd);
    EXPECT_EQ(h.exec_unix("mv /old.txt /new.txt"), 0);
    // Old should not exist
    EXPECT_EQ(h.exec_unix("stat /old.txt"), -1);
    // New should have content
    ASSERT_EQ(h.exec_unix("open /new.txt"), 0);
    fd = h.output;
    h.exec_unix("read " + fd);
    EXPECT_EQ(h.output, "move_me");
}

TEST_F(E2ETest, UnixFs_Stat) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /info.txt");
    ASSERT_EQ(h.exec_unix("open /info.txt w"), 0);
    std::string fd = h.output;
    h.exec_unix("write " + fd + " ABCD");
    h.exec_unix("close " + fd);
    EXPECT_EQ(h.exec_unix("stat /info.txt"), 0);
    EXPECT_NE(h.output.find("size=4"), std::string::npos);
    EXPECT_NE(h.output.find("type=0"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_StatDir) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /mydir");
    EXPECT_EQ(h.exec_unix("stat /mydir"), 0);
    EXPECT_NE(h.output.find("type=1"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_Chmod) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /perm.txt");
    EXPECT_EQ(h.exec_unix("chmod 0755 /perm.txt"), 0);
}

TEST_F(E2ETest, UnixFs_DiskUsage) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("disk"), 0);
    EXPECT_NE(h.output.find("used="), std::string::npos);
}

TEST_F(E2ETest, UnixFs_Tree) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /a");
    h.exec_unix("touch /a/x.txt");
    h.exec_unix("mkdir /a/b");
    EXPECT_EQ(h.exec_unix("tree"), 0);
    EXPECT_NE(h.output.find("a/"), std::string::npos);
    EXPECT_NE(h.output.find("x.txt"), std::string::npos);
    EXPECT_NE(h.output.find("b/"), std::string::npos);
}

TEST_F(E2ETest, UnixFs_Persistence) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /keep.txt");
    ASSERT_EQ(h.exec_unix("open /keep.txt w"), 0);
    std::string fd = h.output;
    h.exec_unix("write " + fd + " persist");
    h.exec_unix("close " + fd);
    h.unix_fs.fs_unmount();
    h.dev.save_to_file("test_e2e.img");

    // Re-mount
    BlockDevice dev2(TOTAL_BLK_NUM, BLOCK_SIZE);
    UnixFs u2(dev2);
    u2.set_disk_path("test_e2e.img");
    dev2.load_from_file("test_e2e.img");
    EXPECT_EQ(u2.fs_mount(), 0);
    EXPECT_EQ(u2.fs_pwd(), "/");
    // Check file exists
    FileStat st;
    EXPECT_EQ(u2.fs_stat("/keep.txt", st), 0);
    u2.fs_unmount();
}

// ==================== FAT16 FS ====================

TEST_F(E2ETest, Fat16Fs_FormatAndMount) {
    EXPECT_EQ(h.fat16_fs.fs_format(), 0);
    EXPECT_EQ(h.fat16_fs.fs_type_name(), "FAT16");
}

TEST_F(E2ETest, Fat16Fs_MkdirAndTouch) {
    h.fat16_fs.fs_format();
    EXPECT_EQ(h.exec_fat("mkdir /subdir"), 0);
    EXPECT_EQ(h.exec_fat("touch /subdir/file.txt"), 0);
    h.exec_fat("ls /subdir");
    EXPECT_NE(h.output.find("FILE.TXT"), std::string::npos);
}

TEST_F(E2ETest, Fat16Fs_WriteAndRead) {
    h.fat16_fs.fs_format();
    h.exec_fat("touch /data.bin");
    ASSERT_EQ(h.exec_fat("open /data.bin w"), 0);
    std::string fd = h.output;
    EXPECT_EQ(h.exec_fat("write " + fd + " FAT16rocks"), 0);
    h.exec_fat("close " + fd);
    ASSERT_EQ(h.exec_fat("open /data.bin"), 0);
    fd = h.output;
    EXPECT_EQ(h.exec_fat("read " + fd), 0);
    EXPECT_EQ(h.output, "FAT16rocks");
}

TEST_F(E2ETest, Fat16Fs_CdAndPwd) {
    h.fat16_fs.fs_format();
    h.exec_fat("mkdir /home");
    EXPECT_EQ(h.exec_fat("cd /home"), 0);
    EXPECT_EQ(h.fat16_fs.fs_pwd(), "/home");
}

TEST_F(E2ETest, Fat16Fs_DeleteAndRmR) {
    h.fat16_fs.fs_format();
    h.exec_fat("touch /bye.txt");
    EXPECT_EQ(h.exec_fat("rm /bye.txt"), 0);
    h.exec_fat("mkdir /d");
    h.exec_fat("touch /d/f.txt");
    EXPECT_EQ(h.exec_fat("rm -r /d"), 0);
}

TEST_F(E2ETest, Fat16Fs_Stat) {
    h.fat16_fs.fs_format();
    h.exec_fat("touch /statme.txt");
    ASSERT_EQ(h.exec_fat("open /statme.txt w"), 0);
    std::string fd = h.output;
    h.exec_fat("write " + fd + " HI");
    h.exec_fat("close " + fd);
    EXPECT_EQ(h.exec_fat("stat /statme.txt"), 0);
    EXPECT_NE(h.output.find("size=2"), std::string::npos);
}

// ==================== Error cases ====================

TEST_F(E2ETest, OpenNonExistentFile) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("open /nope.txt"), -1);
    EXPECT_EQ(h.exec_unix("open /nope.txt w"), -1);
}

TEST_F(E2ETest, CloseInvalidFd) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("close 99"), -1);
}

TEST_F(E2ETest, ReadInvalidFd) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("read 99"), -1);
}

TEST_F(E2ETest, WriteInvalidFd) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("write 99 hello"), -1);
}

TEST_F(E2ETest, MkdirMissingParent) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("mkdir /a/b/c"), -1);
}

TEST_F(E2ETest, DeleteNonExistent) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("rm /ghost.txt"), -1);
}

TEST_F(E2ETest, StatNonExistent) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("stat /ghost"), -1);
}

TEST_F(E2ETest, UseraddByNonRoot) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd bob bobpw 2 2");
    h.exec_unix("logout");
    h.exec_unix("login bob bobpw");
    EXPECT_EQ(h.exec_unix("useradd carl carlpw 3 3"), -1);
}

TEST_F(E2ETest, CommandNotFound) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("foobar"), -1);
}

// ==================== Stress test ====================

TEST_F(E2ETest, CreateManyFiles) {
    h.unix_fs.fs_format();
    for (int i = 0; i < 20; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "/f%d", i);
        EXPECT_EQ(h.exec_unix(std::string("touch ") + name), 0);
    }
    h.exec_unix("ls /");
    // Verify each file appears in listing
    for (int i = 0; i < 20; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "f%d", i);
        EXPECT_NE(h.output.find(name), std::string::npos);
    }
}

TEST_F(E2ETest, NestedDirs) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("mkdir /l1"), 0);
    EXPECT_EQ(h.exec_unix("mkdir /l1/l2"), 0);
    EXPECT_EQ(h.exec_unix("mkdir /l1/l2/l3"), 0);
    EXPECT_EQ(h.exec_unix("mkdir /l1/l2/l3/l4"), 0);
    EXPECT_EQ(h.exec_unix("touch /l1/l2/l3/l4/deep.txt"), 0);
    EXPECT_EQ(h.exec_unix("stat /l1/l2/l3/l4/deep.txt"), 0);
}

TEST_F(E2ETest, OpenWriteReadCloseCycle) {
    h.unix_fs.fs_format();
    for (int i = 0; i < 10; ++i) {
        h.exec_unix("touch /cycle.txt");
        ASSERT_EQ(h.exec_unix("open /cycle.txt w"), 0);
        std::string fd = h.output;
        h.exec_unix("write " + fd + " round" + std::to_string(i));
        h.exec_unix("close " + fd);
        ASSERT_EQ(h.exec_unix("open /cycle.txt"), 0);
        fd = h.output;
        h.exec_unix("read " + fd);
        EXPECT_EQ(h.output, "round" + std::to_string(i));
        h.exec_unix("close " + fd);
        h.exec_unix("rm /cycle.txt");
    }
}

// --- mkdir -p ---

TEST_F(E2ETest, MkdirP) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("mkdir -p /a/b/c"), 0);
    EXPECT_EQ(h.exec_unix("stat /a/b/c"), 0);
}

TEST_F(E2ETest, MkdirPExistingParents) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir /a");
    EXPECT_EQ(h.exec_unix("mkdir -p /a/b/c"), 0);
}

TEST_F(E2ETest, MkdirPNoFlagFails) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("mkdir /x/y/z"), -1);
}

// --- su ---

TEST_F(E2ETest, SuByRoot) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd alice pw 1 1");
    EXPECT_EQ(h.exec_unix("su 1"), 0);
    EXPECT_EQ(h.um.current_username(), "alice");
}

TEST_F(E2ETest, SuByNonRootWithPassword) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd alice apw 1 1");
    h.exec_unix("useradd bob bpw 2 2");
    h.exec_unix("logout");
    h.exec_unix("login alice apw");
    EXPECT_EQ(h.exec_unix("su 2 bpw"), 0);
    EXPECT_EQ(h.um.current_username(), "bob");
}

TEST_F(E2ETest, SuByNonRootWrongPassword) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd alice apw 1 1");
    h.exec_unix("useradd bob bpw 2 2");
    h.exec_unix("logout");
    h.exec_unix("login alice apw");
    EXPECT_EQ(h.exec_unix("su 2 wrong"), -1);
    EXPECT_EQ(h.um.current_username(), "alice");
}

TEST_F(E2ETest, SuByName) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd carol cpw 3 3");
    EXPECT_EQ(h.exec_unix("su carol"), 0);
    EXPECT_EQ(h.um.current_username(), "carol");
}

// --- more ---

TEST_F(E2ETest, MoreDisplaysContent) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /readme.txt");
    ASSERT_EQ(h.exec_unix("open /readme.txt w"), 0);
    std::string fd = h.output;
    h.exec_unix("write " + fd + " line1\nline2\nline3");
    h.exec_unix("close " + fd);
    EXPECT_EQ(h.exec_unix("more /readme.txt"), 0);
    EXPECT_NE(h.output.find("line1"), std::string::npos);
    EXPECT_NE(h.output.find("line3"), std::string::npos);
}

TEST_F(E2ETest, MoreNonExistent) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("more /ghost.txt"), -1);
}

// --- find ---

TEST_F(E2ETest, FindMatchesFiles) {
    h.unix_fs.fs_format();
    h.exec_unix("touch /alpha.txt");
    h.exec_unix("touch /beta.txt");
    h.exec_unix("mkdir /sub");
    h.exec_unix("touch /sub/alphab.txt");
    EXPECT_EQ(h.exec_unix("find / alpha"), 0);
    EXPECT_NE(h.output.find("alpha.txt"), std::string::npos);
    EXPECT_NE(h.output.find("alphab.txt"), std::string::npos);
    EXPECT_EQ(h.output.find("beta.txt"), std::string::npos);
}

TEST_F(E2ETest, FindNoMatches) {
    h.unix_fs.fs_format();
    EXPECT_EQ(h.exec_unix("find / zzz_nonexistent"), 0);
    EXPECT_EQ(h.output, "(no matches)");
}

TEST_F(E2ETest, FindInSubdirectory) {
    h.unix_fs.fs_format();
    h.exec_unix("mkdir -p /a/b");
    h.exec_unix("touch /a/b/target.txt");
    EXPECT_EQ(h.exec_unix("find /a target"), 0);
    EXPECT_NE(h.output.find("target.txt"), std::string::npos);
}

// --- cd ~ ---

TEST_F(E2ETest, CdTilde) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd dave dpw 5 5");
    h.exec_unix("logout");
    h.exec_unix("login dave dpw");
    EXPECT_EQ(h.exec_unix("cd ~"), 0);
    EXPECT_EQ(h.unix_fs.fs_pwd(), "/home/dave");
}

TEST_F(E2ETest, CdTildeSubdir) {
    h.unix_fs.fs_format();
    h.exec_unix("login root root");
    h.exec_unix("useradd eve epw 6 6");
    h.exec_unix("logout");
    h.exec_unix("login eve epw");
    h.exec_unix("mkdir -p /home/eve/docs");
    EXPECT_EQ(h.exec_unix("cd ~/docs"), 0);
    EXPECT_EQ(h.unix_fs.fs_pwd(), "/home/eve/docs");
}

}  // namespace
}  // namespace pfs
