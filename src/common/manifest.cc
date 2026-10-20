#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "package.h"

namespace inpkg {
Text join(const Text& a, const Text& b) {
    Text result(a);
    result += b;
    return result;
}
Text number(uint64_t value) {
    char output[32];
    snprintf(output, sizeof(output), "%llu",
             static_cast<unsigned long long>(value));
    return Text(output);
}
bool fail(const char* message, const char* detail) {
    fprintf(stderr, "inpkg: %s%s%s\n", message, *detail ? ": " : "", detail);
    return false;
}
Text text(const Bytes& input) {
    Text output;
    for (unsigned char value : input) {
        char one[] = {static_cast<char>(value), 0};
        output += one;
    }
    return output;
}
Bytes bytes(const Text& input) {
    Bytes output;
    for (size_t i = 0; i < input.size(); ++i) output.push_back(input[i]);
    return output;
}
bool identifier(const char* value) {
    if (!*value || strlen(value) > 63 || !strcmp(value, ".") ||
        !strcmp(value, ".."))
        return false;
    for (; *value; ++value)
        if (!((*value >= 'a' && *value <= 'z') ||
              (*value >= 'A' && *value <= 'Z') ||
              (*value >= '0' && *value <= '9') || strchr("_+.-", *value)))
            return false;
    return true;
}
bool relative(const char* value) {
    if (!*value || *value == '/' || strlen(value) > 1023) return false;
    const char* start = value;
    for (const char* p = value;; ++p) {
        if (*p && *p != '/') {
            if (static_cast<unsigned char>(*p) < 32 || *p == '\\' || *p == 127)
                return false;
            continue;
        }
        size_t length = p - start;
        if (!length || (length == 1 && *start == '.') ||
            (length == 2 && start[0] == '.' && start[1] == '.'))
            return false;
        if (!*p) return true;
        start = p + 1;
    }
}
bool protectedPath(const char* value) {
    return !strcmp(value, "/lib/ld-osinfo-x86-64.so") ||
           !strcmp(value, "/lib/version.1") ||
           !strncmp(value, "/var/lib/inpkg/", 15);
}
static char* trim(char* value) {
    while (*value == ' ' || *value == '\t') ++value;
    size_t size = strlen(value);
    while (size && strchr(" \t\r", value[size - 1])) value[--size] = 0;
    return value;
}
bool Manifest::parse(const Bytes& input) {
    name.clear();
    version = "1";
    files.clear();
    if (input.empty() || input.size() > 1024 * 1024)
        return fail("missing or oversized manifest");
    for (auto c : input)
        if (!c) return fail("NUL in manifest");
    original = text(input);
    Text copy(original);
    List<Text> binaries, declarations;
    bool named = false, versioned = false;
    char* cursor = copy.data();
    while (*cursor) {
        char* line = cursor;
        char* end  = strchr(cursor, '\n');
        if (end) {
            *end   = 0;
            cursor = end + 1;
        } else
            cursor += strlen(cursor);
        line = trim(line);
        if (!*line || *line == '#') continue;
        char* equals = strchr(line, '=');
        if (!equals) return fail("expected key=value", line);
        *equals     = 0;
        char* key   = trim(line);
        char* value = trim(equals + 1);
        if (!*value) return fail("empty manifest value", key);
        if (!strcmp(key, "name") || !strcmp(key, "version")) {
            bool& seen = !strcmp(key, "name") ? named : versioned;
            if (seen || !identifier(value))
                return fail("invalid or duplicate identifier", key);
            seen = true;
            if (!strcmp(key, "name"))
                name = value;
            else
                version = value;
        } else if (!strcmp(key, "bin") || !strcmp(key, "file") ||
                   !strcmp(key, "include")) {
            if (*value != '/' || !relative(value + 1))
                return fail("invalid declaration", value);
            declarations.push_back(Text(value));
            if (!strcmp(key, "bin")) binaries.push_back(Text(value));
        } else if (!strcmp(key, "install")) {
            char* arrow = strstr(value, "->");
            if (!arrow) return fail("invalid install mapping");
            *arrow       = 0;
            char* source = trim(value);
            char* target = trim(arrow + 2);
            if (!relative(source) || !strcmp(source, "package.conf") ||
                *target != '/' || !relative(target + 1) ||
                protectedPath(target))
                return fail("invalid or system-managed mapping", target);
            for (const auto& old : files)
                if (!strcmp(old.source.c_str(), source) ||
                    !strcmp(old.target.c_str(), target))
                    return fail("duplicate mapping", target);
            if (files.size() >= 8192) return fail("too many package files");
            Mapping entry;
            entry.source = source;
            entry.target = target;
            files.push_back(entry);
        } else
            return fail("unknown manifest key", key);
    }
    if (!named || files.empty())
        return fail("name and install mappings are required");
    for (const auto& declared : declarations) {
        bool found = false;
        for (auto& mapping : files)
            if (mapping.target == declared) {
                found = true;
                for (const auto& executable : binaries)
                    if (executable == declared) mapping.executable = true;
            }
        if (!found)
            return fail("declaration missing install mapping",
                        declared.c_str());
    }
    return true;
}
bool hostRead(const char* path, Bytes& output) {
    FILE* file = fopen(path, "rb");
    if (!file) return fail("cannot read", path);
    output.clear();
    unsigned char buffer[8192];
    size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), file))) {
        if (output.size() + size > 128 * 1024 * 1024) {
            fclose(file);
            return fail("file exceeds limit", path);
        }
        for (size_t i = 0; i < size; ++i) output.push_back(buffer[i]);
    }
    bool ok = !ferror(file);
    fclose(file);
    return ok || fail("read failed", path);
}
bool hostWrite(const char* path, const Bytes& input) {
    FILE* file = fopen(path, "wb");
    if (!file) return fail("cannot write", path);
    bool ok = fwrite(input.data(), 1, input.size(), file) == input.size();
    return (fclose(file) == 0 && ok) || fail("write failed", path);
}
}  // namespace inpkg
