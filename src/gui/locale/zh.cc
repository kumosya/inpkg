#include <cstring>
namespace {struct Text{const char* key;const char* value;};const Text texts[]={
    {"PackageInstaller","软件包安装器"},
    {"ChoosePackage","选择软件包"},
    {"Install","安装"},
    {"Remove","卸载"},
    {"ConfirmInstall","安装此软件包？"},
    {"ConfirmRemove","卸载此软件包？"},
    {"Completed","操作完成。"},
    {"SystemScope","为所有用户安装"},
    {"UserScope","仅为当前用户安装"},
    {"AuthFailed","权限验证失败。"},
};}
extern "C" const char* guiw_locale_lookup(const char* key){for(const auto& text:texts)if(!std::strcmp(key,text.key))return text.value;return nullptr;}
