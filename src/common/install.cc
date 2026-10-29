#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "package.h"
namespace inpkg {
namespace {
struct State {
    Text path, hash;
    unsigned mode = 0644;
};
bool stateParse(const Bytes& data, List<State>& states) {
    states.clear();
    for (auto value : data)
        if (!value) return fail("NUL in state record");
    Text value   = text(data);
    char* cursor = value.data();
    while (*cursor) {
        char* line = cursor;
        char* end  = strchr(cursor, '\n');
        if (end) {
            *end   = 0;
            cursor = end + 1;
        } else
            cursor += strlen(cursor);
        char* hash = strchr(line, '\t');
        if (!hash) return fail("corrupt state record");
        *hash++    = 0;
        char* mode = strchr(hash, '\t');
        if (!mode) return false;
        *mode++               = 0;
        char* tail            = nullptr;
        unsigned long numeric = strtoul(mode, &tail, 8);
        if (!relative(line) || strlen(hash) != 64 ||
            strspn(hash, "0123456789abcdef") != 64 || !tail || *tail ||
            numeric > 0777)
            return fail("corrupt state entry");
        for (const auto& old : states)
            if (!strcmp(old.path.c_str(), line)) return fail("duplicate state path");
        State state;
        state.path = line;
        state.hash = hash;
        state.mode = numeric;
        states.push_back(state);
    }
    return true;
}
Text stateLine(const Text& path, const Bytes& data, unsigned mode) {
    char octal[16];
    snprintf(octal, sizeof(octal), "%03o", mode);
    return join(join(join(join(path, Text("\t")), digest(data)), Text("\t")),
                join(Text(octal), Text("\n")));
}
bool ends(const Text& value, const char* suffix) {
    size_t length = strlen(suffix);
    return value.size() >= length &&
           !strcmp(value.c_str() + value.size() - length, suffix);
}
bool confirm(const Options& options, const char* operation, const char* name) {
    printf("%s %s in %s\n", operation, name, options.root.c_str());
    if (options.yes) return true;
    char reply[16];
    fputs("Continue? [y/N]: ", stdout);
    return fgets(reply, sizeof(reply), stdin) &&
           (reply[0] == 'y' || reply[0] == 'Y');
}
}  // namespace
Installer::Installer(Root& root, const Options& options)
    : root_(root),
      options_(options),
      transaction_(join(options.database, Text("/.transaction"))) {}
Text Installer::effective(const Text& target) const {
    const char* path = target.c_str() + 1;
    if (options_.local) {
        if (!strncmp(path, "usr/", 4)) path += 4;
        if (strncmp(path, "bin/", 4) && strncmp(path, "sbin/", 5) &&
            strncmp(path, "lib/", 4) && strncmp(path, "include/", 8))
            return Text();
    }
    return options_.prefix.empty()
               ? Text(path)
               : join(join(options_.prefix, Text("/")), Text(path));
}
bool Installer::prepare() {
    return root_.valid() && root_.directories(options_.database) && recover();
}
bool Installer::records(List<Manifest>& output) const {
    output.clear();
    List<Text> names;
    if (!root_.list(options_.database, names)) return false;
    for (const auto& name : names)
        if (ends(name, ".conf")) {
            Bytes data;
            Manifest manifest;
            if (!root_.read(join(join(options_.database, Text("/")), name),
                            data) ||
                !manifest.parse(data))
                return fail("invalid installed database", name.c_str());
            if (join(manifest.name, Text(".conf")) != name)
                return fail("database identity mismatch", name.c_str());
            output.push_back(std::move(manifest));
        }
    return true;
}
bool Installer::recover() {
    bool exists = false;
    if (!root_.exists(transaction_, exists)) return false;
    if (!exists) return true;
    bool committed = false, journal = false;
    if (!root_.exists(join(transaction_, Text("/committed")), committed) ||
        !root_.exists(join(transaction_, Text("/journal")), journal))
        return false;
    if (!committed && journal) {
        Bytes data, checksum;
        if (!root_.read(join(transaction_, Text("/journal")), data) ||
            !root_.read(join(transaction_, Text("/journal.sha256")), checksum))
            return false;
        if (text(checksum) != digest(data))
            return fail(
                "corrupt transaction journal; manual recovery required");
        Text input   = text(data);
        char* cursor = input.data();
        List<Text> paths;
        List<Bytes> backups;
        List<unsigned> modes;
        List<bool> originals;
        while (*cursor) {
            char* line = cursor;
            char* end  = strchr(cursor, '\n');
            if (!end) return fail("incomplete transaction journal");
            *end          = 0;
            cursor        = end + 1;
            char* present = strchr(line, '\t');
            if (!present) return false;
            *present++ = 0;
            char* mode = strchr(present, '\t');
            if (!mode) return false;
            *mode++    = 0;
            char* hash = strchr(mode, '\t');
            if (!hash) return false;
            *hash++                  = 0;
            char* tail               = nullptr;
            unsigned long permission = strtoul(mode, &tail, 8);
            if (!relative(line) || (*present != '0' && *present != '1') ||
                present[1] || !tail || *tail || permission > 0777 ||
                paths.size() > 8194)
                return fail("invalid recovery path");
            for (const auto& previous : paths)
                if (!strcmp(previous.c_str(), line))
                    return fail("duplicate recovery path");
            Text path(line), backup = join(join(transaction_, Text("/old/")),
                                           number(paths.size()));
            Bytes previous;
            if (*present == '1') {
                if (!root_.read(backup, previous) ||
                    digest(previous) != Text(hash))
                    return fail(
                        "corrupt transaction backup; manual recovery required",
                        path.c_str());
            } else if (strcmp(hash, "-"))
                return fail("invalid absent-file backup");
            paths.push_back(path);
            backups.push_back(std::move(previous));
            modes.push_back(permission);
            originals.push_back(*present == '1');
        }
        // Validate the entire journal and every backup before restoring any
        // file.
        for (size_t index = 0; index < paths.size(); ++index) {
            const auto& path = paths[index];
            if (originals[index]) {
                Text restore =
                    join(join(transaction_, Text("/restore-")), number(index));
                if (!root_.remove(restore) ||
                    !root_.write(restore, backups[index], modes[index]) ||
                    !root_.rename(restore, path))
                    return fail(
                        "recovery incomplete; keep transaction for retry",
                        path.c_str());
            } else if (!root_.remove(path))
                return fail("cannot remove uncommitted file", path.c_str());
        }
        fprintf(stderr, "inpkg: recovered interrupted package transaction\n");
    }
    return root_.tree(transaction_) || fail("cannot clean package transaction");
}
bool Installer::transaction(const List<Text>& paths,
                            const List<Bytes>& contents,
                            const List<unsigned>& modes,
                            const List<bool>& present) {
    if (!root_.directories(transaction_, 0700) ||
        !root_.directories(join(transaction_, Text("/new")), 0700) ||
        !root_.directories(join(transaction_, Text("/old")), 0700))
        return false;
    Text journal;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool old      = false;
        unsigned mode = 0644;
        Text hash("-");
        if (!root_.exists(paths[i], old)) return recover() && false;
        if (old) {
            Bytes data;
            if (!root_.permissions(paths[i], mode) ||
                !root_.read(paths[i], data) ||
                !root_.write(join(join(transaction_, Text("/old/")), number(i)),
                             data, mode))
                return recover() && false;
            hash = digest(data);
        }
        char permission[16];
        snprintf(permission, sizeof(permission), "%03o", mode);
        journal += join(
            join(join(join(paths[i], Text("\t")), Text(old ? "1" : "0")),
                 Text("\t")),
            join(join(join(Text(permission), Text("\t")), hash), Text("\n")));
        if (present[i] &&
            !root_.write(join(join(transaction_, Text("/new/")), number(i)),
                         contents[i], modes[i]))
            return recover() && false;
    }
    if (!root_.write(join(transaction_, Text("/journal.sha256")),
                     bytes(digest(bytes(journal))), 0600) ||
        !root_.write(join(transaction_, Text("/journal.pending")),
                     bytes(journal), 0600) ||
        !root_.rename(join(transaction_, Text("/journal.pending")),
                      join(transaction_, Text("/journal"))))
        return recover() && false;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool ok = present[i]
                      ? root_.rename(
                            join(join(transaction_, Text("/new/")), number(i)),
                            paths[i])
                      : root_.remove(paths[i]);
        if (!ok) {
            recover();
            return fail("transaction failed", paths[i].c_str());
        }
#ifdef INPKG_TESTING
        const char* injected = getenv("INPKG_TEST_CRASH_AFTER");
        if (injected && strtoul(injected, nullptr, 10) == i + 1) _exit(86);
#endif
    }
    if (!root_.write(join(transaction_, Text("/committed")), Bytes(), 0600)) {
        recover();
        return false;
    }
    return root_.tree(transaction_);
}
bool Installer::apply(const Package& package, bool upgrade,
                      bool preflightOnly) {
    List<Manifest> installed;
    if (!records(installed)) return false;
    const Manifest* old = nullptr;
    for (const auto& entry : installed)
        if (entry.name == package.manifest.name) old = &entry;
    if (!upgrade && old)
        return fail("package already installed; use upgrade",
                    old->name.c_str());
    if (upgrade && !old && !options_.missing)
        return fail(
            "package is not installed; use install or --install-missing",
            package.manifest.name.c_str());
    List<State> states;
    Text record =
             join(join(options_.database, Text("/")), package.manifest.name),
         statePath = join(record, Text(".state"));
    if (old) {
        bool present = false;
        if (!root_.exists(statePath, present)) return false;
        if (!present)
            return fail("legacy database needs explicit adopt before upgrade",
                        old->name.c_str());
        Bytes state;
        if (!root_.read(statePath, state) || !stateParse(state, states))
            return false;
    }
    List<Text> paths;
    List<Bytes> contents;
    List<unsigned> modes;
    List<bool> present;
    Text nextState;
    for (const auto& mapping : package.manifest.files) {
        Text path = effective(mapping.target);
        if (path.empty())
            return fail("unsupported local mapping", mapping.target.c_str());
        for (const auto& other : installed)
            if (other.name != package.manifest.name)
                for (const auto& file : other.files)
                    if (effective(file.target) == path)
                        return fail("destination owned by another package",
                                    path.c_str());
        bool exists = false;
        if (!root_.exists(path, exists)) return false;
        bool owned = false;
        if (old)
            for (const auto& file : old->files)
                if (effective(file.target) == path) owned = true;
        if (exists && !owned)
            return fail("unowned destination exists", path.c_str());
        if (exists) {
            Bytes current;
            unsigned mode;
            bool tracked = false;
            if (!root_.read(path, current) || !root_.permissions(path, mode))
                return false;
            for (const auto& state : states)
                if (state.path == path) {
                    tracked = true;
                    if (state.hash != digest(current) || state.mode != mode)
                        return fail(
                            "locally modified file; preserve and resolve "
                            "conflict",
                            path.c_str());
                }
            if (!tracked) return fail("missing file state", path.c_str());
        }
        const Bytes* data = package.data(mapping.source);
        if (!data) return fail("missing payload", mapping.source.c_str());
        unsigned mode = mapping.executable ? 0755 : 0644;
        paths.push_back(path);
        contents.push_back(*data);
        modes.push_back(mode);
        present.push_back(true);
        nextState += stateLine(path, *data, mode);
    }
    if (old)
        for (const auto& mapping : old->files) {
            Text path     = effective(mapping.target);
            bool retained = false;
            for (const auto& item : paths)
                if (item == path) retained = true;
            if (retained) continue;
            bool exists = false;
            if (!root_.exists(path, exists)) return false;
            if (exists) {
                Bytes current;
                unsigned mode;
                bool tracked = false;
                if (!root_.read(path, current) ||
                    !root_.permissions(path, mode))
                    return false;
                for (const auto& state : states)
                    if (state.path == path) {
                        tracked = true;
                        if (state.hash != digest(current) || state.mode != mode)
                            return fail("removed file was locally modified",
                                        path.c_str());
                    }
                if (!tracked)
                    return fail("missing old file state", path.c_str());
            }
            paths.push_back(path);
            contents.push_back(Bytes());
            modes.push_back(0);
            present.push_back(false);
        }
    paths.push_back(join(record, Text(".conf")));
    contents.push_back(bytes(package.manifest.original));
    modes.push_back(0644);
    present.push_back(true);
    paths.push_back(statePath);
    contents.push_back(bytes(nextState));
    modes.push_back(0644);
    present.push_back(true);
    if (preflightOnly) return true;
    if (!confirm(options_, old ? "Upgrade" : "Install",
                 package.manifest.name.c_str()))
        return false;
    return transaction(paths, contents, modes, present);
}
bool Installer::adopt(const char* name) {
    if (!identifier(name)) return false;
    Text record = join(join(options_.database, Text("/")), Text(name));
    Bytes data;
    Manifest manifest;
    if (!root_.read(join(record, Text(".conf")), data) ||
        !manifest.parse(data) || strcmp(manifest.name.c_str(), name))
        return false;
    bool exists = false;
    if (!root_.exists(join(record, Text(".state")), exists)) return false;
    if (exists) return fail("package already has tracked state", name);
    Text state;
    for (const auto& mapping : manifest.files) {
        Text path = effective(mapping.target);
        Bytes content;
        unsigned mode;
        if (path.empty() || !root_.read(path, content) ||
            !root_.permissions(path, mode))
            return false;
        state += stateLine(path, content, mode);
    }
    if (!confirm(options_, "Adopt current files as legacy baseline", name))
        return false;
    return root_.write(join(record, Text(".state")), bytes(state), 0644);
}
bool Installer::remove(const char* name) {
    if (!identifier(name)) return false;
    List<Manifest> records;
    if (!this->records(records)) return false;
    const Manifest* selected = nullptr;
    for (const auto& record : records)
        if (!strcmp(record.name.c_str(), name)) selected = &record;
    if (!selected) return fail("package is not installed", name);
    Text prefix = join(join(options_.database, Text("/")), Text(name));
    Bytes state;
    List<State> states;
    if (!root_.read(join(prefix, Text(".state")), state) ||
        !stateParse(state, states))
        return fail("legacy removal needs adopt first", name);
    List<Text> paths;
    List<Bytes> contents;
    List<unsigned> modes;
    List<bool> present;
    for (const auto& mapping : selected->files) {
        Text path   = effective(mapping.target);
        bool exists = false;
        if (path.empty() || !root_.exists(path, exists)) return false;
        for (const auto& other : records)
            if (other.name != selected->name)
                for (const auto& file : other.files)
                    if (effective(file.target) == path)
                        return fail("ambiguous file ownership", path.c_str());
        if (exists) {
            Bytes current;
            unsigned mode;
            bool tracked = false;
            if (!root_.read(path, current) || !root_.permissions(path, mode))
                return false;
            for (const auto& entry : states)
                if (entry.path == path) {
                    tracked = true;
                    if (entry.hash != digest(current) || entry.mode != mode)
                        return fail("locally modified file; removal refused",
                                    path.c_str());
                }
            if (!tracked) return false;
        }
        paths.push_back(path);
    }
    paths.push_back(join(prefix, Text(".conf")));
    paths.push_back(join(prefix, Text(".state")));
    for (size_t i = 0; i < paths.size(); ++i) {
        contents.push_back(Bytes());
        modes.push_back(0);
        present.push_back(false);
    }
    return confirm(options_, "Remove", name) &&
           transaction(paths, contents, modes, present);
}
bool Installer::list() const {
    List<Manifest> entries;
    if (!records(entries)) return false;
    for (const auto& entry : entries)
        printf("%s %s\n", entry.name.c_str(), entry.version.c_str());
    return true;
}
bool Installer::apps() const {
    List<Manifest> entries;if(!records(entries))return false;
    for(const auto& package:entries)for(const auto& file:package.files)if(file.graphical) {
        const auto path=effective(file.target);if(path.empty())continue;
        printf("%s\t%s\t/%s\n",package.name.c_str(),package.version.c_str(),path.c_str());
    }
    return true;
}
bool Installer::info(const char* name) const {
    if (!identifier(name)) return false;
    Bytes data;
    if (!root_.read(join(join(join(options_.database, Text("/")), Text(name)),
                         Text(".conf")),
                    data))
        return false;
    fwrite(data.data(), 1, data.size(), stdout);
    return true;
}
}  // namespace inpkg
