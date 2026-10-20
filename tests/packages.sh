#!/bin/sh
set -eu
cli=${1:?inpkg executable}
stage=$(mktemp -d '/tmp/inpkg-test.XXXXXX')
trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/payload" "$stage/root" "$stage/import"
printf 'old bytes\n' >"$stage/payload/tool"
cat >"$stage/package.conf" <<'CONF'
name=probe
version=1
bin=/usr/bin/probe
install=tool -> /usr/bin/probe
CONF
"$cli" pack "$stage/payload" --manifest "$stage/package.conf" --output "$stage/probe.inpk"
"$cli" check "$stage/probe.inpk"
tar -xOf "$stage/probe.inpk" tool | cmp - "$stage/payload/tool"
if "$cli" install "$stage/probe.inpk" -y; then echo 'missing sysroot accepted' >&2; exit 1; fi
if "$cli" install "$stage/probe.inpk" --sysroot / -y; then echo 'host root accepted' >&2; exit 1; fi
if "$cli" upgrade "$stage/probe.inpk" --sysroot "$stage/root" -y; then echo 'upgrade installed absent package' >&2; exit 1; fi
"$cli" upgrade "$stage/probe.inpk" --sysroot "$stage/root" --install-missing -y
if "$cli" install "$stage/probe.inpk" --sysroot "$stage/root" -y; then echo 'install replaced registered package' >&2; exit 1; fi
test "$(sha256sum "$stage/root/usr/bin/probe" | cut -d ' ' -f1)" = "$(cut -f2 "$stage/root/var/lib/inpkg/probe.state")"
printf 'new bytes\n' >"$stage/payload/tool"
printf 'new data\n' >"$stage/payload/extra"
cat >>"$stage/package.conf" <<'CONF'
file=/usr/share/probe
install=extra -> /usr/share/probe
CONF
"$cli" pack "$stage/payload" --manifest "$stage/package.conf" --output "$stage/new.inpk"
"$cli" upgrade "$stage/new.inpk" --sysroot "$stage/root" -y
cmp "$stage/root/usr/bin/probe" "$stage/payload/tool"
"$cli" upgrade "$stage/probe.inpk" --sysroot "$stage/root" -y
test ! -e "$stage/root/usr/share/probe"
printf 'user change\n' >"$stage/root/usr/bin/probe"
if "$cli" upgrade "$stage/new.inpk" --sysroot "$stage/root" --install-missing -y; then echo 'overwrote local change' >&2; exit 1; fi
printf 'old bytes\n' >"$stage/root/usr/bin/probe"
chmod 755 "$stage/root/usr/bin/probe"
"$cli" remove probe --sysroot "$stage/root" -y
test ! -e "$stage/root/usr/bin/probe"

# Traversal, symlinks, malformed manifests, and checksum corruption fail closed.
mkdir -p "$stage/escape" "$stage/root2"
ln -s "$stage/escape" "$stage/root2/usr"
if "$cli" install "$stage/probe.inpk" --sysroot "$stage/root2" -y; then echo 'followed sysroot symlink' >&2; exit 1; fi
test ! -e "$stage/escape/bin/probe"
cp "$stage/probe.inpk" "$stage/bad.inpk"
printf x | dd of="$stage/bad.inpk" bs=1 seek=20 conv=notrunc 2>/dev/null
if "$cli" check "$stage/bad.inpk"; then echo 'accepted corrupt archive' >&2; exit 1; fi
cp "$stage/package.conf" "$stage/reserved.conf"
printf 'install=extra -> /lib/version.1\n' >>"$stage/reserved.conf"
if "$cli" pack "$stage/payload" --manifest "$stage/reserved.conf" --output "$stage/reserved.inpk"; then exit 1; fi
"$cli" import "$stage/probe.inpk" --package-dir "$stage/import"
"$cli" check "$stage/import/probe.inpk"

# A separate test-only executable injects an abrupt exit after mutation 1.
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
c++ -std=c++20 -O1 -DINPKG_TESTING -I"$project/include" "$project"/src/common/*.cc "$project"/src/linux/*.cc -o "$stage/inpkg-crash"
"$cli" install "$stage/probe.inpk" --sysroot "$stage/root" -y
if INPKG_TEST_CRASH_AFTER=1 "$stage/inpkg-crash" upgrade "$stage/new.inpk" --sysroot "$stage/root" -y; then echo 'crash not injected' >&2; exit 1; fi
cp "$stage/root/var/lib/inpkg/.transaction/journal" "$stage/saved-journal"
printf 'corrupt journal\n' >"$stage/root/var/lib/inpkg/.transaction/journal"
if "$cli" upgrade "$stage/new.inpk" --sysroot "$stage/root" --install-missing -y; then echo 'corrupt transaction triggered install fallback' >&2; exit 1; fi
cmp "$stage/payload/tool" "$stage/root/usr/bin/probe"
cp "$stage/saved-journal" "$stage/root/var/lib/inpkg/.transaction/journal"
"$cli" recover --sysroot "$stage/root"
tar -xOf "$stage/probe.inpk" tool | cmp - "$stage/root/usr/bin/probe"
test ! -e "$stage/root/usr/share/probe"
"$cli" upgrade "$stage/new.inpk" --sysroot "$stage/root" -y
"$cli" list --sysroot "$stage/root" | grep -q 'probe 1'
"$cli" info probe --sysroot "$stage/root" | grep -q 'name=probe'
"$cli" remove probe --sysroot "$stage/root" -y
make -C "$project" install DESTDIR="$stage/install"
installed="$stage/install/usr/local/bin/inpkg"
test -x "$installed"
"$installed" check "$stage/probe.inpk"
make -C "$project" uninstall DESTDIR="$stage/install"
test ! -e "$installed"
echo 'inpkg archive, sysroot, upgrade, modified-file, SHA256 and interrupted-transaction tests passed'
