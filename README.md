# inpkg：OSInfo 软件包工具

同一项目提供 Linux 程序 `build/linux/inpkg` 和 OSInfo 程序 `build/packages/inpkg/inpkg`。`src/common/` 共享 manifest、ustar/gzip、SHA-256、文件清单、数据库和事务；`src/linux/`、`src/osinfo/` 分别适配文件系统、锁和用户范围。`include/inpkg/gzip.h` 由本项目的两个目标共用，基础工具保留自己的归档头文件。

编译：

```sh
make
```

`make host` 构建 Linux 宿主程序。可通过 `make install` 安装到 `/usr/local/bin/inpkg`；使用 `PREFIX` 和 `DESTDIR` 可覆盖前缀或暂存安装，例如 `make install PREFIX=/usr DESTDIR=/tmp/stage`。`make uninstall` 移除该可执行文件。

InfoIDE 通过 PATH 中的 `inpkg` 可执行程序进行打包、校验和镜像更新，不再链接 inpkg 源码。先安装 inpkg，再安装或运行 InfoIDE。若未安装到 PATH，可在调用 InfoIDE 时设置 `INPKG=/path/to/inpkg`。

若需构建 OSInfo 目标包，先安装 InfoIDE 的 Linux 发行包；项目根目录的 ./infoide 二进制已忽略，自动发现已安装 SDK，无需相邻源码目录。目标构建可用 `OSINFO_SDK` 和 `INFOIDE` 显式覆盖，宿主使用原生 `CXX`。包处理不调用 Python 或外部 tar；归档直接使用共享 C++ 实现。

```sh
build/linux/inpkg pack build/payload --manifest package.conf --output hello.inpk
build/linux/inpkg check hello.inpk
build/linux/inpkg import hello.inpk --package-dir /path/to/osinfo/packages
build/linux/inpkg install hello.inpk --sysroot /mnt/osinfo
build/linux/inpkg upgrade hello.inpk --sysroot /mnt/osinfo
build/linux/inpkg upgrade hello.inpk --sysroot /mnt/osinfo --install-missing
build/linux/inpkg list --sysroot /mnt/osinfo
build/linux/inpkg info hello --sysroot /mnt/osinfo
build/linux/inpkg remove hello --sysroot /mnt/osinfo
```

install 只首次安装；upgrade 要求已安装；upgrade --install-missing 仅在该包确实未安装时首次安装。损坏包、数据库、冲突、权限或更新失败不会转为首次安装。同版本可显式更新，以给定归档为准，不自动求解版本或依赖。`-y` 适用于已明确选择目标的自动化操作。

Linux 的安装、更新、卸载及已安装查询必须指定 sysroot，拒绝宿主根 `/`。安装路径相对打开的根目录描述符逐层解析，拒绝符号链接与特殊成员；不自动挂载、格式化或运行目标程序。数据库为 sysroot 内的 `/var/lib/inpkg`。

OSInfo 程序使用 `--system`、`--local` 或 `--root`（root 用户的私有范围），原有用户/XDG 路径约定保留；Linux 不接受这些范围选项，OSInfo 不接受 sysroot。系统包需要 root 修改权限。

数据库保留原始 `NAME.conf`，增加 `NAME.state` 记录相对路径、SHA-256 和模式。拒绝跨包覆盖、无归属文件覆盖或用户修改的文件。升级可以添加/移除文件；卸载也会保护修改过的文件。解释器、版本配置及包数据库路径不允许普通包占用。

旧数据库可查询；没有摘要的旧记录必须显式采纳当前文件后才能修改：

```sh
build/linux/inpkg adopt NAME --sysroot /mnt/osinfo
```

adopt 将当前文件作为新基线，无法追溯此前是否已修改，因此需要调用者明确选择；不会自动覆盖已有摘要。旧布局中同时登记相同文件的废弃包记录需要先解决归属冲突，升级不会偷偷抹掉它们。

写操作先预检整个输入集合，再逐包准备新文件和备份。`.transaction/journal` 在变更之前发布，完成文件与记录更新后写入 committed 标记；进程中断后，下次写操作恢复未提交事务，已提交事务只清理备份。也可显式执行：

```sh
build/linux/inpkg recover --sysroot /mnt/osinfo
```

查询遇到待恢复事务会明确失败。Linux 使用 flock，OSInfo 使用带进程身份的目录锁；各平台锁文件分开，离线挂载操作仍须由镜像流程排除运行中的磁盘使用。保证范围为单包事务与已测试的进程中断恢复，不宣称整批或未验证的断电原子性。

OSInfo 目前没有 fsync 操作，目标适配依赖已确认完成的文件写入与关闭；不承诺断电持久性。Linux 与目标系统的摘要和数据库格式相同，离线写入的记录可在 OSInfo 内继续升级或卸载。

`catalog --config build/packages.conf --package-dir packages --makefile build/packages.mk` 读取 configure 生成的包集。manifest 行指定 OS 运行库定义，archive 行指定扫描发现的 .inpk；不需要维护应用名字列表。省略 makefile 时打包并检查包集，--check 检查现有归档，指定 --sysroot 时先全量预检再按 upgrade --install-missing 安装。
