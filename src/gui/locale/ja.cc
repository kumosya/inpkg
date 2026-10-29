#include <cstring>
namespace {struct Text{const char* key;const char* value;};const Text texts[]={
    {"PackageInstaller","パッケージインストーラー"},
    {"ChoosePackage","パッケージを選択"},
    {"Install","インストール"},
    {"Remove","アンインストール"},
    {"ConfirmInstall","このパッケージをインストールしますか？"},
    {"ConfirmRemove","このパッケージをアンインストールしますか？"},
    {"Completed","操作が完了しました。"},
    {"SystemScope","すべてのユーザーにインストール"},
    {"UserScope","現在のユーザーにインストール"},
    {"AuthFailed","認証が拒否されました。"},
};}
extern "C" const char* guiw_locale_lookup(const char* key){for(const auto& text:texts)if(!std::strcmp(key,text.key))return text.value;return nullptr;}
