#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
hps=$(cd "$here/.." && pwd)
upstream=${NGM_UPSTREAM:-$here/UPSTREAM}
patch=${NGM_PATCH:-$here/hooks.patch}
W=${NGM_W:-$hps/build/main/work}
out=${NGM_OUT:-$hps/build/main}
img=ngage-main-build:local
plat=linux/amd64
tree=$W/main

die() {
    echo "build.sh: $*" >&2
    exit 1
}

mode=fork
case ${1:-} in
--upstream-only) mode=upstream ;;
'') ;;
*) die "unknown argument $1" ;;
esac

url=$(sed -n 's/^URL=//p' "$upstream")
pin=$(sed -n 's/^COMMIT=//p' "$upstream")
[ -n "$url" ] && [ -n "$pin" ] || die "no URL or COMMIT in $upstream"
case $W in '' | /) die "bad work dir" ;; esac

if [ -e "$tree" ]; then
    [ -f "$tree/.ngm_clone" ] || { echo "build.sh: $tree was not created by build.sh, refusing to remove it" >&2; exit 2; }
    rm -rf "$tree"
fi
mkdir -p "$W"
git clone -q --no-checkout "$url" "$tree"
: >"$tree/.ngm_clone"
git -C "$tree" -c advice.detachedHead=false checkout -q --detach "$pin" 2>/dev/null || die "unknown pin $pin in $url"

if [ "$mode" = fork ]; then
    git -C "$tree" apply --check "$patch" || die "patch does not apply: $patch"
    git -C "$tree" apply "$patch"
    mod=$tree/support/ngage
    mkdir -p "$mod/lib" "$mod/common"
    cp "$here/support/ngage/ngage.h" "$here/support/ngage/ngage.cpp" "$mod/"
    cp "$hps"/lib/*.c "$hps"/lib/*.h "$mod/lib/"
    cp "$hps"/common/*.h "$mod/common/"
    name=MiSTer_ngage
else
    name=MiSTer_upstream
fi

docker image inspect "$img" >/dev/null 2>&1 || docker build --platform "$plat" -t "$img" "$here"
docker run --rm --platform "$plat" -v "$tree:/src" -w /src "$img" make
[ -f "$tree/bin/MiSTer" ] || die "make produced no bin/MiSTer"

mkdir -p "$out"
cp "$tree/bin/MiSTer" "$out/$name"
echo "artefact $out/$name"
file -b "$out/$name"
if command -v md5sum >/dev/null 2>&1; then
    echo "md5 $(md5sum "$out/$name" | cut -d' ' -f1)"
else
    echo "md5 $(md5 -q "$out/$name")"
fi
info=$(docker run --rm --platform "$plat" -v "$out:/out:ro" "$img" sh -c \
    'arm-none-linux-gnueabihf-readelf -d "/out/$0"; arm-none-linux-gnueabihf-readelf -V "/out/$0"' "$name")
echo "$info" | sed -n 's/.*Shared library: \[\(.*\)\].*/NEEDED \1/p'
echo "$info" | grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9][0-9.]*' | sort -uV | sed 's/^/VERSION /'
