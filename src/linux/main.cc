#include <cstring>

#include "../common/package.h"
namespace inpkg {
int catalog(int argc, char** argv);
int platformOptions(int argc, char** argv, Options& options) {
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], "--local") || !strcmp(argv[i], "--root") ||
            !strcmp(argv[i], "--system"))
            return fail("OSInfo scope flag is not a Linux sysroot") ? 0 : 1;
    if (options.root.empty())
        return fail("Linux installed-package operations require --sysroot") ? 0
                                                                            : 1;
    Root root(options.root.c_str());
    if (!root.valid() || !strcmp(root.path(), "/"))
        return fail("refusing Linux host root") ? 0 : 1;
    options.root = root.path();
    return 0;
}
}  // namespace inpkg
#ifndef INPKG_EMBEDDED
int main(int argc, char** argv) {
    return argc > 1 && !strcmp(argv[1], "catalog") ? inpkg::catalog(argc, argv)
                                                   : inpkg::command(argc, argv);
}
#endif
