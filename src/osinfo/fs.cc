#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "../common/package.h"
#include "os/task.h"
#include <sys/stat.h>
namespace inpkg {
Root::Root(const char* path) {
    struct stat status{};
    if (stat(path, &status) || !S_ISDIR(status.st_mode)) {
        fail("invalid root", path);
        return;
    }
    path_ = path;
    fd_   = 1;
}
Root::~Root() {}
int Root::parent(const char* path, Text& leaf, bool create) const {
    if (!valid() || !relative(path)) return -1;
    leaf = join(Text(path_.c_str()),
                Text(path_.c_str()[path_.size() - 1] == '/' ? "" : "/"));
    leaf += path;
    Text copy(leaf);
    char* cursor = copy.data();
    for (char* slash = strchr(cursor + 1, '/'); slash;
         slash       = strchr(slash + 1, '/')) {
        *slash = 0;
        struct stat status{};
        if (lstat(cursor, &status)) {
            if (!create || mkdir(cursor, 0755)) return -1;
        } else if (!S_ISDIR(status.st_mode))
            return -1;
        *slash = '/';
    }
    return 0;
}
bool Root::exists(const Text& path, bool& present) const {
    present = false;
    Text full;
    if (parent(path.c_str(), full, false) < 0) return errno == ENOENT;
    struct stat status{};
    if (lstat(full.c_str(), &status)) return errno == ENOENT;
    if (S_ISLNK(status.st_mode)) return false;
    present = true;
    return true;
}
bool Root::read(const Text& path, Bytes& data) const {
    Text full;
    if (parent(path.c_str(), full, false) < 0) return false;
    struct stat status{};
    if (lstat(full.c_str(), &status) || !S_ISREG(status.st_mode)) return false;
    return hostRead(full.c_str(), data);
}
bool Root::write(const Text& path, const Bytes& data, unsigned mode,
                 bool exclusive) const {
    Text full;
    if (parent(path.c_str(), full, true) < 0) return false;
    int file = open(full.c_str(),
                    O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), mode);
    if (file < 0) return false;
    size_t used = 0;
    bool ok     = true;
    while (used < data.size()) {
        ssize_t count = ::write(file, data.data() + used, data.size() - used);
        if (count <= 0) {
            ok = false;
            break;
        }
        used += count;
    }
    // OSInfo acknowledges filesystem writes synchronously but has no fsync
    // operation. The journal supports process interruption, not power loss.
    ok = fchmod(file, mode) == 0 && ok;
    return (close(file) == 0 && ok) || fail("cannot write", full.c_str());
}
bool Root::directories(const Text& path, unsigned mode) const {
    Text full;
    if (parent(path.c_str(), full, true) < 0) return false;
    struct stat status{};
    if (!lstat(full.c_str(), &status)) return S_ISDIR(status.st_mode);
    return mkdir(full.c_str(), mode) == 0;
}
bool Root::rename(const Text& source, const Text& destination) const {
    Text a, b;
    return parent(source.c_str(), a, false) >= 0 &&
           parent(destination.c_str(), b, true) >= 0 &&
           ::rename(a.c_str(), b.c_str()) == 0;
}
bool Root::remove(const Text& path) const {
    Text full;
    if (parent(path.c_str(), full, false) < 0) return errno == ENOENT;
    struct stat status{};
    if (lstat(full.c_str(), &status)) return errno == ENOENT;
    return S_ISDIR(status.st_mode)
               ? rmdir(full.c_str()) == 0
               : !S_ISLNK(status.st_mode) && unlink(full.c_str()) == 0;
}
bool Root::list(const Text& path, List<Text>& names) const {
    names.clear();
    Text full;
    if (parent(path.c_str(), full, false) < 0) return false;
    DIR* directory = opendir(full.c_str());
    if (!directory) return false;
    while (dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            names.push_back(Text(entry->d_name));
    }
    closedir(directory);
    return true;
}
bool Root::tree(const Text& path) const {
    bool present = false;
    if (!exists(path, present)) return false;
    if (!present) return true;
    Text full;
    if (parent(path.c_str(), full, false) < 0) return false;
    struct stat status{};
    if (lstat(full.c_str(), &status)) return false;
    if (S_ISDIR(status.st_mode)) {
        List<Text> names;
        if (!list(path, names)) return false;
        for (const auto& name : names)
            if (!tree(join(join(path, Text("/")), name))) return false;
    }
    return remove(path);
}
bool Root::permissions(const Text& path, unsigned& mode) const {
    Text full;
    if (parent(path.c_str(), full, false) < 0) return false;
    struct stat status{};
    if (lstat(full.c_str(), &status) || !S_ISREG(status.st_mode)) return false;
    mode = status.st_mode & 0777;
    return true;
}
bool Lock::acquire(const Root& root, const Text& database) {
    path_ = join(join(join(Text(root.path()),
                           Text(!strcmp(root.path(), "/") ? "" : "/")),
                      database),
                 Text("/.lock"));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (mkdir(path_.c_str(), 0700) == 0) {
            Text owner = join(path_, Text("/owner"));
            FILE* file = fopen(owner.c_str(), "w");
            if (!file) {
                rmdir(path_.c_str());
                return false;
            }
            fprintf(file, "%d\n", getpid());
            fclose(file);
            fd_ = 1;
            return true;
        }
        Bytes owner;
        if (!hostRead(join(path_, Text("/owner")).c_str(), owner))
            return fail("package database locked");
        uint64_t pid = strtoull(text(owner).c_str(), nullptr, 10);
        os::TaskInfo info{};
        if (!pid || (!os::Task::query(pid, info) && info.state != 2))
            return fail("package database locked");
        if (unlink(join(path_, Text("/owner")).c_str()) || rmdir(path_.c_str()))
            return false;
    }
    return false;
}
Lock::~Lock() {
    if (fd_ >= 0) {
        unlink(join(path_, Text("/owner")).c_str());
        rmdir(path_.c_str());
    }
}
}  // namespace inpkg
