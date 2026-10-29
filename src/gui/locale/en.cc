#include <cstring>
namespace {struct Text{const char* key;const char* value;};const Text texts[]={
    {"PackageInstaller","Package installer"},
    {"ChoosePackage","Choose package"},
    {"Install","Install"},
    {"Remove","Uninstall"},
    {"ConfirmInstall","Install this package?"},
    {"ConfirmRemove","Uninstall this package?"},
    {"Completed","Operation completed."},
    {"SystemScope","Install for all users"},
    {"UserScope","Install for this user"},
    {"AuthFailed","Authentication rejected."},
};}
extern "C" const char* guiw_locale_lookup(const char* key){for(const auto& text:texts)if(!std::strcmp(key,text.key))return text.value;return nullptr;}
