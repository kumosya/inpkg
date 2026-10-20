#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "../common/package.h"
#include <sys/file.h>
#include <sys/stat.h>
namespace inpkg {
Root::Root(const char* path) {
    char* canonical = realpath(path, nullptr);
    if (!canonical) {
        fail("invalid root", path);
        return;
    }
    path_ = canonical;
    free(canonical);
    fd_ = open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd_ < 0) fail("cannot open root", path);
}
Root::~Root() {
    if (fd_ >= 0) close(fd_);
}
int Root::parent(const char* path, Text& leaf, bool create) const {
    if (fd_ < 0 || !relative(path)) {
        fail("invalid relative path", path);
        return -1;
    }
    Text copy(path);
    char* cursor = copy.data();
    int current  = dup(fd_);
    while (char* slash = strchr(cursor, '/')) {
        *slash = 0;
        if (create && mkdirat(current, cursor, 0755) != 0 && errno != EEXIST) {
            close(current);
            return -1;
        }
        int next = openat(current, cursor,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(current);
        if (next < 0) return -1;
        current = next;
        cursor  = slash + 1;
    }
    leaf = cursor;
    return current;
}
bool Root::exists(const Text& path, bool& present) const {
    present = false;
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return errno == ENOENT;
    struct stat status{};
    int result = fstatat(directory, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW);
    int saved  = errno;
    close(directory);
    if (!result) {
        if (S_ISLNK(status.st_mode))
            return fail("symlink refused", path.c_str());
        present = true;
        return true;
    }
    return saved == ENOENT || fail("cannot inspect", path.c_str());
}
bool Root::read(const Text& path, Bytes& data) const {
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return fail("unsafe or missing parent", path.c_str());
    int input = openat(directory, leaf.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(directory);
    struct stat status{};
    if (input < 0 || fstat(input, &status) || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || status.st_size > 128 * 1024 * 1024) {
        if (input >= 0) close(input);
        return fail("not a readable regular file", path.c_str());
    }
    data.clear();
    unsigned char buffer[8192];
    ssize_t size;
    while ((size = ::read(input, buffer, sizeof(buffer))) > 0) {
        if (data.size() + size > 128 * 1024 * 1024) {
            close(input);
            return false;
        }
        for (ssize_t i = 0; i < size; ++i) data.push_back(buffer[i]);
    }
    close(input);
    return size == 0;
}
bool Root::write(const Text& path, const Bytes& data, unsigned mode,
                 bool exclusive) const {
    Text leaf;
    int directory = parent(path.c_str(), leaf, true);
    if (directory < 0) return fail("unsafe write parent", path.c_str());
    int output = openat(directory, leaf.c_str(),
                        O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                            (exclusive ? O_EXCL : O_TRUNC),
                        mode);
    if (output < 0) {
        close(directory);
        return fail("cannot create file", path.c_str());
    }
    size_t used = 0;
    bool ok     = true;
    while (used < data.size()) {
        ssize_t count = ::write(output, data.data() + used, data.size() - used);
        if (count <= 0) {
            ok = false;
            break;
        }
        used += count;
    }
    ok = fchmod(output, mode) == 0 && fsync(output) == 0 && ok;
    close(output);
    ok = fsync(directory) == 0 && ok;
    close(directory);
    return ok;
}
bool Root::directories(const Text& path, unsigned mode) const {
    Text leaf;
    int directory = parent(path.c_str(), leaf, true);
    if (directory < 0) return false;
    bool ok    = mkdirat(directory, leaf.c_str(), mode) == 0 || errno == EEXIST;
    int target = openat(directory, leaf.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (target < 0) return false;
    close(target);
    return ok;
}
bool Root::rename(const Text& source, const Text& destination) const {
    Text a, b;
    int from = parent(source.c_str(), a, false),
        to   = parent(destination.c_str(), b, true);
    if (from < 0 || to < 0) {
        if (from >= 0) close(from);
        if (to >= 0) close(to);
        return false;
    }
    struct stat status{};
    bool ok = fstatat(to, b.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0
                  ? errno == ENOENT
                  : !S_ISLNK(status.st_mode);
    ok      = ok && renameat(from, a.c_str(), to, b.c_str()) == 0;
    if (ok) ok = fsync(from) == 0 && fsync(to) == 0;
    close(from);
    close(to);
    return ok;
}
bool Root::remove(const Text& path) const {
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return errno == ENOENT;
    struct stat status{};
    bool ok;
    if (fstatat(directory, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW))
        ok = errno == ENOENT;
    else
        ok = !S_ISLNK(status.st_mode) &&
             unlinkat(directory, leaf.c_str(),
                      S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0) == 0;
    if (ok) ok = fsync(directory) == 0;
    close(directory);
    return ok;
}
bool Root::list(const Text& path, List<Text>& names) const {
    names.clear();
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return false;
    int target = openat(directory, leaf.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (target < 0) return false;
    DIR* stream = fdopendir(target);
    if (!stream) {
        close(target);
        return false;
    }
    while (dirent* entry = readdir(stream))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            names.push_back(Text(entry->d_name));
    closedir(stream);
    return true;
}
bool Root::tree(const Text& path) const {
    bool present = false;
    if (!exists(path, present)) return false;
    if (!present) return true;
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return false;
    struct stat status{};
    bool ok =
        fstatat(directory, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0;
    close(directory);
    if (!ok || S_ISLNK(status.st_mode)) return false;
    if (S_ISDIR(status.st_mode)) {
        List<Text> names;
        if (!list(path, names)) return false;
        for (const auto& name : names)
            if (!tree(join(join(path, Text("/")), name))) return false;
    }
    return remove(path);
}
bool Root::permissions(const Text& path, unsigned& mode) const {
    Text leaf;
    int directory = parent(path.c_str(), leaf, false);
    if (directory < 0) return false;
    struct stat status{};
    bool ok =
        fstatat(directory, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(status.st_mode);
    close(directory);
    mode = status.st_mode & 0777;
    return ok;
}
bool Lock::acquire(const Root& root, const Text& database) {
    path_ = join(join(join(Text(root.path()), Text("/")), database),
                 Text("/.lock"));
    // Open the database through an anchored root before opening its lock.
    int rootfd =
        open(root.path(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootfd < 0) return false;
    Text copy(database);
    char* cursor = copy.data();
    int current  = rootfd;
    while (*cursor) {
        char* end = strchr(cursor, '/');
        if (end) *end = 0;
        int next = openat(current, cursor,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(current);
        if (next < 0) return false;
        current = next;
        if (!end) break;
        cursor = end + 1;
    }
    fd_ = openat(current, ".lock.linux",
                 O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(current);
    return (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0) ||
           fail("package database is locked");
}
Lock::~Lock() {
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        close(fd_);
    }
}
}  // namespace inpkg
