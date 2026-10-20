#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cstring>

#include "../common/package.h"
namespace fs = std::filesystem;
namespace inpkg {
int catalog(int argc, char** argv) {
    try {
        fs::path configuration = "build/packages.conf", directory = "packages",
                 makefile, sysroot;
        bool check = false;
        for (int i = 2; i < argc; ++i) {
            Text key(argv[i]);
            if (key == Text("--check")) {
                check = true;
                continue;
            }
            if (i + 1 == argc)
                throw std::runtime_error("missing catalog option value");
            const char* value = argv[++i];
            if (key == Text("--config"))
                configuration = value;
            else if (key == Text("--package-dir"))
                directory = value;
            else if (key == Text("--makefile"))
                makefile = value;
            else if (key == Text("--sysroot"))
                sysroot = value;
            else
                throw std::runtime_error("unknown catalog option");
        }
        directory = fs::absolute(directory);
        List<Manifest> definitions;
        std::vector<fs::path> external;
        std::ifstream file(configuration);
        if (!file) throw std::runtime_error("missing package catalog");
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            auto separator = line.find('|');
            if (separator == std::string::npos)
                throw std::runtime_error("invalid catalog row");
            auto kind  = line.substr(0, separator),
                 value = line.substr(separator + 1);
            if (kind == "manifest") {
                Bytes data;
                Manifest manifest;
                if (!hostRead(value.c_str(), data) || !manifest.parse(data))
                    return 1;
                definitions.push_back(std::move(manifest));
            } else if (kind == "external") {
                if (!identifier(value.c_str()))
                    throw std::runtime_error("invalid external name");
                external.push_back(directory / "external" / (value + ".inpk"));
            } else if (kind == "archive") {
                external.emplace_back(value);
            } else
                throw std::runtime_error("unknown catalog row");
        }
        std::set<std::string> names, destinations;
        std::ostringstream generated;
        std::vector<std::string> applications;
        for (const auto& manifest : definitions) {
            if (!names.insert(manifest.name.c_str()).second)
                throw std::runtime_error("duplicate package name");
            for (const auto& mapping : manifest.files) {
                if (!destinations.insert(mapping.target.c_str()).second)
                    throw std::runtime_error("duplicate destination");
                if (mapping.executable &&
                    strncmp(mapping.target.c_str(), "/sbin/sys/", 10)) {
                    std::string name =
                        fs::path(mapping.source.c_str()).filename();
                    if (!identifier(name.c_str()))
                        throw std::runtime_error("invalid app name");
                    applications.push_back(name);
                    generated << "APP_OUTPUT_" << name << " := "
                              << (directory / manifest.name.c_str() /
                                  mapping.source.c_str())
                                     .string()
                              << '\n';
                }
            }
        }
        if (!makefile.empty()) {
            generated << "APPLICATION_NAMES :=";
            for (const auto& name : applications) generated << ' ' << name;
            generated << "\nAPPLICATION_PROGRAMS := $(foreach "
                         "app,$(APPLICATION_NAMES),$(APP_OUTPUT_$(app)))\n";
            fs::create_directories(makefile.parent_path());
            auto temporary = makefile.string() + ".tmp";
            std::ofstream output(temporary);
            output << generated.str();
            output.close();
            if (!output)
                throw std::runtime_error("cannot write package makefile");
            fs::rename(temporary, makefile);
            return 0;
        }
        List<Package> packages;
        for (const auto& manifest : definitions) {
            fs::path stage = directory / manifest.name.c_str();
            fs::create_directories(stage);
            if (!check) {
                for (const auto& mapping : manifest.files) {
                    fs::path source      = mapping.source.c_str(),
                             destination = stage / source;
                    if (source.begin()->string() == "include") {
                        if (fs::is_symlink(source) ||
                            !fs::is_regular_file(source))
                            throw std::runtime_error(
                                "missing or linked header");
                        fs::create_directories(destination.parent_path());
                        fs::copy_file(source, destination,
                                      fs::copy_options::overwrite_existing);
                    }
                }
                if (!hostWrite((stage / "package.conf").c_str(),
                               bytes(manifest.original)))
                    return 1;
                Package package;
                if (!package.loadDirectory(stage.c_str(),
                                           (stage / "package.conf").c_str()) ||
                    !package.save(
                        (directory /
                         (std::string(manifest.name.c_str()) + ".inpk"))
                            .c_str()))
                    return 1;
            }
            Package package;
            if (!package.load(
                    (directory / (std::string(manifest.name.c_str()) + ".inpk"))
                        .c_str()))
                return 1;
            packages.push_back(std::move(package));
        }
        for (const auto& archive : external) {
            Package package;
            if (!package.load(archive.c_str()))
                return fail("missing or incorrect external package",
                            archive.c_str())
                           ? 0
                           : 1;
            if (!names.insert(package.manifest.name.c_str()).second)
                throw std::runtime_error("duplicate external name");
            for (const auto& mapping : package.manifest.files)
                if (!destinations.insert(mapping.target.c_str()).second)
                    throw std::runtime_error("external installation conflict");
            packages.push_back(std::move(package));
        }
        if (!sysroot.empty()) {
            Options options;
            Root root(sysroot.c_str());
            if (!root.valid() || !strcmp(root.path(), "/")) return 1;
            options.root    = root.path();
            options.yes     = true;
            options.missing = true;
            if (!root.directories(options.database)) return 1;
            Lock lock;
            if (!lock.acquire(root, options.database)) return 1;
            Installer installer(root, options);
            if (!installer.prepare()) return 1;
            for (const auto& package : packages)
                if (!installer.apply(package, true, true)) return 1;
            for (const auto& package : packages)
                if (!installer.apply(package, true)) return 1;
        }
        printf("inpkg: %s %zu catalog packages\n",
               check ? "validated" : "packed", packages.size());
        return 0;
    } catch (const std::exception& error) {
        return fail(error.what()) ? 0 : 1;
    }
}
}  // namespace inpkg
