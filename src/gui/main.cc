#include <osgui/application.h>
#include <osgui/dialogs.h>
#include <osgui/controls.h>
#include <osgui/packages.h>
#include <os/user.h>
#include <os/service.h>
#include <service/common/runtime.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include <new>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "../common/package.h"

namespace {
void erase(char* text,size_t size){volatile char* p=text;while(size--)*p++=0;}
int execute(bool removing,const char* target,bool system,char* secret) {
    const auto child=fork();
    if(child==0) {
        if(system && geteuid()!=0) {
            os::User::Profile profile;
            const int status=os::User().authorizeSudo(secret,profile);
            erase(secret,128);if(status)_exit(125);
        } else erase(secret,128);
        char* arguments[]={const_cast<char*>("inpkg"),const_cast<char*>(removing?"remove":"upgrade"),
            const_cast<char*>(target),const_cast<char*>(system?"--system":"--local"),
            const_cast<char*>("-y"),const_cast<char*>("--install-missing"),nullptr};
        if(removing)arguments[5]=nullptr;
        execv("/usr/bin/inpkg",arguments);_exit(127);
    }
    erase(secret,128);
    if(child<0)return errno;
    int status=0;
    while(waitpid(child,&status,0)<0){if(errno!=EINTR)return errno;}
    return WIFEXITED(status)?WEXITSTATUS(status):EIO;
}
class Installer {
    osgui::Connection connection_;
    osgui::Window window_{connection_};
    osgui::Ui ui_;
    osgui::Button choose_{{16,16,156,32}},install_{{16,180,156,32}},remove_{{184,180,156,32}};
    osgui::FileDialog picker_;
    osgui::AuthBox authentication_;
    osgui::ComboBox scope_{{16,266,400,32}};
    osgui::MessageBox message_;
    char target_[384]{},name_[64]{},version_[64]{};
    bool system_=true,removing_=false,running_=true,dirty_=true,done_=false,confirming_=false;
    int result_=0;
    std::thread* worker_=nullptr;
    void paintDetails(osgui::Canvas& canvas) {
        const auto theme=osgui::defaultTheme();
        canvas.text(16,70,name_,theme.text,osgui::FontFace::Ui,22);
        canvas.text(16,108,version_,theme.text);
        canvas.text(16,140,target_,theme.muted,osgui::FontFace::Ui,12);
        canvas.text(16,230,osgui::tr(system_?"SystemScope":"UserScope"),theme.muted);
    }
    void labels(){choose_.setTextKey("ChoosePackage");install_.setTextKey("Install");remove_.setTextKey("Remove");const char* names[]={osgui::tr("SystemScope"),osgui::tr("UserScope")};scope_.setItems(names,2);scope_.setSelected(system_?0:1);scope_.takeChanged();dirty_=true;}
    void report(const char* key) {message_.show("PackageInstaller",osgui::tr(key));ui_.setModal(&message_);dirty_=true;}
    void inspect(const char* path) {
        inpkg::Package package;
        if(!path || std::strlen(path)>=sizeof(target_) || !package.load(path)){report("Failed");return;}
        std::snprintf(target_,sizeof(target_),"%s",path);
        std::snprintf(name_,sizeof(name_),"%s",package.manifest.name.c_str());
        std::snprintf(version_,sizeof(version_),"%s",package.manifest.version.c_str());
        removing_=false;install_.setEnabled(true);remove_.setEnabled(false);dirty_=true;
    }
    void begin(char* secret) {
        if(worker_){erase(secret,128);return;}
        char password[128]{};std::memcpy(password,secret,128);erase(secret,128);
        done_=false;scope_.setEnabled(false);choose_.setEnabled(false);install_.setEnabled(false);remove_.setEnabled(false);window_.setBusy(true);
        worker_=new(std::nothrow)std::thread([this,password]() mutable {
            result_=execute(removing_,target_,system_,password);
            __atomic_store_n(&done_,true,__ATOMIC_RELEASE);
        });
        erase(password,sizeof(password));
        if(!worker_ || !worker_->joinable()){delete worker_;worker_=nullptr;window_.setBusy(false);choose_.setEnabled(true);scope_.setEnabled(!removing_);install_.setEnabled(!removing_);remove_.setEnabled(removing_);report("Failed");}
    }
    void confirm() {
        if(!*target_ || worker_)return;
        confirming_=true;
        char text[512];std::snprintf(text,sizeof(text),"%s\n%s %s\n%s",osgui::tr(removing_?"ConfirmRemove":"ConfirmInstall"),name_,version_,target_);
        message_.show("PackageInstaller",text,osgui::MessageBoxKind::Confirm,osgui::MessageBoxButtons::OkCancel);
        ui_.setModal(&message_);dirty_=true;
    }
public:
    ~Installer(){if(worker_){worker_->join();delete worker_;}}
    int run(const char* argument) {
        rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);
        if(!connection_.connect() || !window_.open(640,440,osgui::tr("PackageInstaller")))return 1;
        window_.setSizeLimits(560,360);window_.show();ui_.bind(window_);
        ui_.setBackgroundPainter([](osgui::Canvas& canvas,osgui::Rect,void* context){static_cast<Installer*>(context)->paintDetails(canvas);},this);
        ui_.add(choose_);ui_.add(install_);ui_.add(remove_);ui_.add(scope_);ui_.add(picker_);ui_.add(authentication_);ui_.add(message_);labels();
        install_.setEnabled(false);remove_.setEnabled(false);
        if(argument && !std::strncmp(argument,"--remove=",9)) {
            const auto* scope=argument+9;const auto* colon=std::strchr(scope,':');
            if(!colon || (!std::strncmp(scope,"system:",7)?false:std::strncmp(scope,"user:",5)!=0) || !inpkg::identifier(colon+1))return 2;
            system_=!std::strncmp(scope,"system:",7);removing_=true;
            std::snprintf(target_,sizeof(target_),"%s",colon+1);std::snprintf(name_,sizeof(name_),"%s",colon+1);remove_.setEnabled(true);
        } else if(argument)inspect(argument);
        system_=removing_?system_:geteuid()==0;
        scope_.setSelected(system_?0:1);scope_.takeChanged();scope_.setEnabled(!removing_);
        while(running_ && connection_.valid()) {
            osgui::WindowEvent event{};
            while(window_.pollEvent(event)) {
                if(event.type==osgui::WindowEventType::WindowClose){running_=false;break;}
                ui_.event(event);if(event.type==osgui::WindowEventType::SettingsChanged){labels();window_.setTitle(osgui::tr("PackageInstaller"));}
                if(event.type==osgui::WindowEventType::WindowResize)dirty_=true;
            }
            if(choose_.takeAction()){osgui::FileDialogOptions options;options.extensions=".inpk";picker_.show(options);ui_.setModal(&picker_);dirty_=true;}
            if(picker_.poll())dirty_=true;
            if(picker_.takeAction()){ui_.setModal(nullptr);if(picker_.accepted())inspect(picker_.path());dirty_=true;}
            if(install_.takeAction() || remove_.takeAction())confirm();
            if(scope_.takeChanged()){system_=scope_.selected()==0;dirty_=true;}
            if(message_.takeAction()) {
                ui_.setModal(nullptr);dirty_=true;
                if(confirming_ && message_.accepted()) {
                    if(system_ && geteuid()!=0){authentication_.show(osgui::tr(removing_?"ConfirmRemove":"ConfirmInstall"));ui_.setModal(&authentication_);}
                    else {char secret[128]{};begin(secret);}
                }
                confirming_=false;
            }
            char secret[128]{};
            if(authentication_.takeCredentials(secret,sizeof(secret))){authentication_.dismiss(false);ui_.setModal(nullptr);begin(secret);dirty_=true;}
            erase(secret,sizeof(secret));
            if(authentication_.takeAction()){ui_.setModal(nullptr);dirty_=true;}
            if(worker_ && __atomic_load_n(&done_,__ATOMIC_ACQUIRE)) {
                worker_->join();delete worker_;worker_=nullptr;window_.setBusy(false);choose_.setEnabled(true);
                scope_.setEnabled(!removing_);install_.setEnabled(!removing_);remove_.setEnabled(removing_);report(result_==125?"AuthFailed":result_?"Failed":"Completed");
            }
            if(dirty_ || ui_.dirty()) {
                osgui::Canvas canvas(window_);
                ui_.invalidateAll();ui_.paint(canvas);dirty_=!canvas.flush(true);
            }
            os::sleep(16);
        }
        window_.close();return 0;
    }
};
}
int main(int argc,char** argv){osgui::initializeApplication("inpkg");Installer installer;return installer.run(argc>1?argv[1]:nullptr);}
