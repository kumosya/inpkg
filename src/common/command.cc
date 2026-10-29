#include <cerrno>
#include <cstdio>
#include <cstring>

#include "package.h"
#include <sys/stat.h>
namespace inpkg {
int command(int argc, char** argv) {
    if (argc < 2 || !strcmp(argv[1], "--help")) {
        puts(
            "usage: inpkg pack DIR --manifest FILE --output FILE | check "
            "FILE... | import FILE --package-dir DIR | install/upgrade FILE... "
            "[--install-missing] | remove/info/adopt NAME | list | apps | recover "
            "[--sysroot DIR] [-y]");
        return argc < 2 ? 2 : 0;
    }
    Text operation(argv[1]), manifest, output, packageDirectory;
    List<Text> arguments;
    Options options;
    for (int i = 2; i < argc; ++i) {
        const char* value = argv[i];
        if (!strcmp(value, "--manifest") || !strcmp(value, "--output") ||
            !strcmp(value, "--package-dir") || !strcmp(value, "--sysroot")) {
            if (++i == argc) return fail("missing option value", value) ? 0 : 2;
            if (!strcmp(value, "--manifest"))
                manifest = argv[i];
            else if (!strcmp(value, "--output"))
                output = argv[i];
            else if (!strcmp(value, "--package-dir"))
                packageDirectory = argv[i];
            else
                options.root = argv[i];
        } else if (!strcmp(value, "--install-missing"))
            options.missing = true;
        else if (!strcmp(value, "--yes") || !strcmp(value, "-y"))
            options.yes = true;
        else if (!strcmp(value, "--system") || !strcmp(value, "--local") ||
                 !strcmp(value, "--root")) {
        } else if (*value == '-')
            return fail("unknown option", value) ? 0 : 2;
        else
            arguments.push_back(Text(value));
    }
    if (operation == Text("digest") || operation == Text("inspect") ||
        operation == Text("inspect-manifest")) {
        if (arguments.size() != 1) return 2;
        if (operation == Text("digest")) {
            Bytes data;
            if (!hostRead(arguments[0].c_str(), data)) return 1;
            puts(digest(data).c_str());
            return 0;
        }
        Manifest manifest;
        if (operation == Text("inspect")) {
            Package package;
            if (!package.load(arguments[0].c_str())) return 1;
            manifest = std::move(package.manifest);
        } else {
            Bytes data;
            if (!hostRead(arguments[0].c_str(), data) || !manifest.parse(data)) return 1;
        }
        printf("name\t%s\n", manifest.name.c_str());
        for (const auto& file : manifest.files)
            printf("%s\t%s\n", file.executable ? "bin" : "file", file.target.c_str());
        return 0;
    }
    if (operation == Text("pack")) {
        if (arguments.size() != 1 || manifest.empty() || output.empty())
            return fail("pack requires directory, --manifest and --output") ? 0
                                                                            : 2;
        Package package;
        return package.loadDirectory(arguments[0].c_str(), manifest.c_str()) &&
                       package.save(output.c_str())
                   ? 0
                   : 1;
    }
    if (operation == Text("check") || operation == Text("import")) {
        if (arguments.empty()) return 2;
        for (const auto& path : arguments) {
            Package package;
            if (!package.load(path.c_str())) return 1;
            if (operation == Text("import")) {
                if (packageDirectory.empty())
                    return fail("import requires --package-dir") ? 0 : 2;
                if (mkdir(packageDirectory.c_str(), 0755) != 0 &&
                    errno != EEXIST)
                    return fail("cannot create import directory",
                                packageDirectory.c_str())
                               ? 0
                               : 1;
                Root target(packageDirectory.c_str());
                if (!target.valid() ||
                    !package.save(
                        join(join(packageDirectory, Text("/")),
                             join(package.manifest.name, Text(".inpk")))
                            .c_str()))
                    return 1;
            }
            printf("%s %s %s\n", operation.c_str(),
                   package.manifest.name.c_str(),
                   package.manifest.version.c_str());
        }
        return 0;
    }
    if (options.missing && operation != Text("upgrade"))
        return fail("--install-missing requires upgrade") ? 0 : 2;
    if (platformOptions(argc, argv, options) != 0) return 2;
    Root root(options.root.c_str());
    if (operation == Text("list") || operation == Text("info") || operation==Text("apps")) {
        if (!root.valid()) return 1;
        bool pending = false;
        if (!root.exists(join(options.database, Text("/.transaction")),
                         pending) ||
            pending)
            return fail("package transaction pending; recover before querying")
                       ? 0
                       : 1;
        Installer reader(root, options);
        if(operation==Text("apps"))return arguments.empty() && reader.apps()?0:1;
        if (operation == Text("list"))
            return arguments.empty() && reader.list() ? 0 : 1;
        return arguments.size() == 1 && reader.info(arguments[0].c_str()) ? 0
                                                                          : 1;
    }
    if (!root.valid() || !root.directories(options.database)) return 1;
    Lock lock;
    if (!lock.acquire(root, options.database)) return 1;
    Installer installer(root, options);
    if (!installer.prepare()) return 1;
    if (operation == Text("recover")) return arguments.empty() ? 0 : 2;
    if (operation == Text("list"))
        return arguments.empty() && installer.list() ? 0 : 1;
    if (operation == Text("info") || operation == Text("remove") ||
        operation == Text("adopt")) {
        if (arguments.size() != 1) return 2;
        return (operation == Text("info") ? installer.info(arguments[0].c_str())
                : operation == Text("adopt")
                    ? installer.adopt(arguments[0].c_str())
                    : installer.remove(arguments[0].c_str()))
                   ? 0
                   : 1;
    }
    if (operation != Text("install") && operation != Text("upgrade"))
        return fail("unknown command", operation.c_str()) ? 0 : 2;
    if (arguments.empty()) return 2;
    List<Package> packages;
    for (const auto& path : arguments) {
        Package package;
        if (!package.load(path.c_str())) return 1;
        for (const auto& previous : packages) {
            if (previous.manifest.name == package.manifest.name)
                return fail("duplicate input package") ? 0 : 1;
            for (const auto& a : previous.manifest.files)
                for (const auto& b : package.manifest.files)
                    if (a.target == b.target)
                        return fail("batch destination conflict",
                                    a.target.c_str())
                                   ? 0
                                   : 1;
        }
        packages.push_back(std::move(package));
    }
    const bool upgrade = operation == Text("upgrade");
    for (const auto& package : packages)
        if (!installer.apply(package, upgrade, true)) return 1;
    for (const auto& package : packages)
        if (!installer.apply(package, upgrade)) return 1;
    return 0;
}
}  // namespace inpkg
