#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "../common/package.h"
#include "os/dir.h"
#include "os/user.h"
namespace inpkg {
int platformOptions(int argc, char** argv, Options& options) {
    if (!options.root.empty())
        return fail("--sysroot is a Linux option") ? 0 : 1;
    const char* scope = nullptr;
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], "--system") || !strcmp(argv[i], "--local") ||
            !strcmp(argv[i], "--root")) {
            if (scope) return fail("multiple package scopes") ? 0 : 1;
            scope = argv[i];
        }
    if (!scope) scope = geteuid() == 0 ? "--system" : "--local";
    options.root = "/";
    char database[1024]{};
    os::Dir directories;
    if (!strcmp(scope, "--system")) {
        if (geteuid() != 0 && strcmp(argv[1], "list") &&
            strcmp(argv[1], "info") && strcmp(argv[1],"apps"))
            return fail("system changes require root") ? 0 : 1;
        if (directories.open(os::Dir::Scope::System,"inpkg") || !directories.getState(database,sizeof(database)))
            return 1;
    } else {
        const uint32_t uid = !strcmp(scope, "--root") ? 0 : getuid();
        if (uid == 0 && geteuid() != 0) return 1;
        os::User::Profile profile{};
        if (os::User().lookup(uid, profile))
            return 1;
        options.local  = true;
        options.prefix = join(Text(profile.getHome() + 1), Text("/.local"));
        if (uid == geteuid()) {
            if (directories.open(os::Dir::Scope::User,"inpkg") || !directories.getState(database,sizeof(database)))
                return 1;
        } else
            snprintf(database, sizeof(database), "%s/.local/state/inpkg",
                     profile.getHome());
    }
    if (*database != '/' || !relative(database + 1)) return 1;
    options.database = database + 1;
    return 0;
}
}  // namespace inpkg
int main(int argc, char** argv) { return inpkg::command(argc, argv); }
