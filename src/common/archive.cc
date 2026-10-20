#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "inpkg/gzip.h"
#include "package.h"
namespace inpkg {
namespace {
Text field(const unsigned char* data, size_t size) {
    Text result;
    for (size_t i = 0; i < size && data[i]; ++i) {
        char c[] = {static_cast<char>(data[i]), 0};
        result += c;
    }
    return result;
}
bool octal(const unsigned char* data, size_t size, uint64_t& value) {
    value      = 0;
    bool ended = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (!c || c == ' ') {
            if (value) ended = true;
            continue;
        }
        if (ended || c < '0' || c > '7' || value > (UINT64_MAX >> 3))
            return false;
        value = value * 8 + c - '0';
    }
    return true;
}
bool zero(const unsigned char* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (data[i]) return false;
    return true;
}
bool header(archive::Output& output, const Text& name, size_t size,
            unsigned mode) {
    unsigned char block[512]{};
    const char* path     = name.c_str();
    const char* basename = path;
    if (name.size() > 100) {
        const char* split = strrchr(path, '/');
        if (!split || static_cast<size_t>(split - path) > 155 ||
            strlen(split + 1) > 100)
            return fail("ustar path too long", path);
        memcpy(block + 345, path, split - path);
        basename = split + 1;
    }
    memcpy(block, basename, strlen(basename));
    snprintf(reinterpret_cast<char*>(block + 100), 8, "%07o", mode);
    snprintf(reinterpret_cast<char*>(block + 108), 8, "%07o", 0);
    snprintf(reinterpret_cast<char*>(block + 116), 8, "%07o", 0);
    snprintf(reinterpret_cast<char*>(block + 124), 12, "%011llo",
             static_cast<unsigned long long>(size));
    snprintf(reinterpret_cast<char*>(block + 136), 12, "%011o", 0);
    memset(block + 148, ' ', 8);
    block[156] = '0';
    memcpy(block + 257, "ustar", 5);
    memcpy(block + 263, "00", 2);
    unsigned checksum = 0;
    for (unsigned char c : block) checksum += c;
    snprintf(reinterpret_cast<char*>(block + 148), 7, "%06o", checksum);
    block[155] = ' ';
    return output.write(block, sizeof(block));
}
}  // namespace
const Bytes* Package::data(const Text& name) const {
    for (const auto& member : members)
        if (member.name == name) return &member.data;
    return nullptr;
}
bool Package::load(const char* path) {
    members.clear();
    FILE* file = fopen(path, "rb");
    if (!file) return fail("cannot open archive", path);
    bool ok = true, ended = false;
    size_t total = 0;
    {
        archive::Input input(file, true);
        unsigned char block[512];
        while (ok) {
            if (input.read(block, 512) != 512) {
                ok = false;
                break;
            }
            if (zero(block, 512)) {
                if (input.read(block, 512) != 512 || !zero(block, 512)) {
                    ok = false;
                    break;
                }
                size_t count;
                while ((count = input.read(block, 512)))
                    if (!zero(block, count)) {
                        ok = false;
                        break;
                    }
                ended = true;
                break;
            }
            uint64_t size = 0, checksum = 0;
            unsigned sum = 0;
            for (size_t i = 0; i < 512; ++i)
                sum += i >= 148 && i < 156 ? ' ' : block[i];
            if (!octal(block + 124, 12, size) ||
                !octal(block + 148, 8, checksum) || sum != checksum ||
                memcmp(block + 257, "ustar", 5) ||
                (block[156] && block[156] != '0') || size > 64 * 1024 * 1024 ||
                total + size > 128 * 1024 * 1024 || members.size() > 8192) {
                ok = false;
                break;
            }
            Text name = field(block, 100), prefix = field(block + 345, 155);
            if (!prefix.empty()) name = join(join(prefix, Text("/")), name);
            if (!relative(name.c_str())) {
                ok = false;
                break;
            }
            for (const auto& old : members)
                if (old.name == name) ok = false;
            if (!ok) break;
            Member member;
            member.name = name;
            member.data.resize(size);
            total += size;
            if (input.read(member.data.data(), size) != size) {
                ok = false;
                break;
            }
            const size_t padding = (512 - size % 512) % 512;
            if (input.read(block, padding) != padding) {
                ok = false;
                break;
            }
            members.push_back(std::move(member));
        }
        ok = ok && ended && input.finish();
    }
    fclose(file);
    if (!ok) return fail("invalid, corrupt or unsupported archive", path);
    const auto* configuration = data(Text("package.conf"));
    if (!configuration || !manifest.parse(*configuration)) return false;
    if (members.size() != manifest.files.size() + 1)
        return fail("archive does not match manifest", path);
    for (const auto& mapping : manifest.files)
        if (!data(mapping.source))
            return fail("missing archive member", mapping.source.c_str());
    return true;
}
bool Package::loadDirectory(const char* path, const char* manifestPath) {
    members.clear();
    Bytes configuration;
    if (!hostRead(manifestPath, configuration) ||
        !manifest.parse(configuration))
        return false;
    Root root(path);
    if (!root.valid()) return false;
    Member definition;
    definition.name = "package.conf";
    definition.data = configuration;
    members.push_back(std::move(definition));
    for (const auto& mapping : manifest.files) {
        Member member;
        member.name = mapping.source;
        if (!root.read(mapping.source, member.data)) return false;
        members.push_back(std::move(member));
    }
    return true;
}
bool Package::save(const char* path) const {
    Text temporary = join(join(Text(path), Text(".tmp.")), number(getpid()));
    FILE* file     = fopen(temporary.c_str(), "wbx");
    if (!file)
        return fail("cannot create archive temporary", temporary.c_str());
    bool ok = true;
    {
        archive::Output output(file, true);
        unsigned char padding[1024]{};
        for (const auto& member : members) {
            unsigned mode = 0644;
            for (const auto& mapping : manifest.files)
                if (mapping.source == member.name && mapping.executable)
                    mode = 0755;
            if (!header(output, member.name, member.data.size(), mode) ||
                !output.write(member.data.data(), member.data.size()) ||
                !output.write(padding,
                              (512 - member.data.size() % 512) % 512)) {
                ok = false;
                break;
            }
        }
        ok = ok && output.write(padding, 1024) && output.finish();
    }
    ok = fclose(file) == 0 && ok;
    if (ok) ok = ::rename(temporary.c_str(), path) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok || fail("archive write failed", path);
}
}  // namespace inpkg
