#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inpkg {
using Text  = std::string;
using Bytes = std::vector<unsigned char>;
template <class T>
using List = std::vector<T>;
Text join(const Text& a, const Text& b);
Text number(uint64_t value);
Text digest(const Bytes& bytes);
bool fail(const char* message, const char* detail = "");
bool identifier(const char* value);
bool relative(const char* value);
bool protectedPath(const char* value);
Text text(const Bytes& bytes);
Bytes bytes(const Text& value);
bool hostRead(const char* path, Bytes& output);
bool hostWrite(const char* path, const Bytes& input);
struct Mapping {
    Text source, target;
    bool executable = false;
};
struct Manifest {
    Text name, version, original;
    List<Mapping> files;
    bool parse(const Bytes& input);
};
struct Member {
    Text name;
    Bytes data;
};
struct Package {
    Manifest manifest;
    List<Member> members;
    const Bytes* data(const Text& name) const;
    bool load(const char* path);
    bool loadDirectory(const char* path, const char* manifestPath);
    bool save(const char* path) const;
};
class Root {
    Text path_;
    int fd_ = -1;
    int parent(const char* relative, Text& leaf, bool create) const;

   public:
    explicit Root(const char* path);
    ~Root();
    Root(const Root&)            = delete;
    Root& operator=(const Root&) = delete;
    bool valid() const { return fd_ >= 0; }
    const char* path() const { return path_.c_str(); }
    bool read(const Text& path, Bytes& data) const;
    bool write(const Text& path, const Bytes& data, unsigned mode,
               bool exclusive = true) const;
    bool exists(const Text& path, bool& present) const;
    bool directories(const Text& path, unsigned mode = 0755) const;
    bool rename(const Text& source, const Text& destination) const;
    bool remove(const Text& path) const;
    bool list(const Text& path, List<Text>& names) const;
    bool tree(const Text& path) const;
    bool permissions(const Text& path, unsigned& mode) const;
};
class Lock {
    Text path_;
    int fd_ = -1;

   public:
    ~Lock();
    bool acquire(const Root& root, const Text& database);
};
struct Options {
    Text root, prefix, database = "var/lib/inpkg";
    bool yes = false, missing = false, local = false;
};
class Installer {
    Root& root_;
    Options options_;
    Text transaction_;
    Text effective(const Text& target) const;
    bool recover();
    bool transaction(const List<Text>& paths, const List<Bytes>& contents,
                     const List<unsigned>& modes, const List<bool>& present);
    bool records(List<Manifest>& output) const;

   public:
    Installer(Root& root, const Options& options);
    bool apply(const Package& package, bool upgrade,
               bool preflightOnly = false);
    bool remove(const char* name);
    bool adopt(const char* name);
    bool list() const;
    bool info(const char* name) const;
    bool prepare();
};
int platformOptions(int argc, char** argv, Options& options);
int platformMain(int argc, char** argv);
int command(int argc, char** argv);
}  // namespace inpkg
