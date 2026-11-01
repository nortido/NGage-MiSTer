# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${NGH_MON_OUT:-$(cd "$here/.." && pwd)/build/mon}
cc=${NGH_CC:-arm-none-eabi-gcc}
objcopy=${NGH_OBJCOPY:-arm-none-eabi-objcopy}
size=${NGH_SIZE:-arm-none-eabi-size}
ver=${NGH_MON_VERSION:-b13-dev}
monva=$(awk '$1 == "#define" && $2 == "MON_VA_BASE" { sub(/u$/, "", $3); print $3 }' "$here/guest_census.h")
[ -n "$monva" ] || { echo "build: MON_VA_BASE missing in guest_census.h" >&2; exit 1; }

mkdir -p "$out"
"$cc" -mcpu=cortex-a9 -marm -mfloat-abi=soft -mno-unaligned-access -std=gnu99 -O2 -Wall -Wextra -Werror \
    -ffreestanding -fno-tree-loop-distribute-patterns -nostdlib -static \
    -DMON_VERSION="\"$ver\"" ${NGH_MON_DEFS:-} -Wl,-T,"$here/link.ld" -Wl,--defsym=MON_VA="$monva" \
    -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
    -o "$out/mon.elf" "$here/start.S" "$here/vectors.S" "$here/main.c" \
    "$here/cp15.c" "$here/patch.c" "$here/ldst.c" "$here/pamap.c" "$here/shadow.c" "$here/nor.c" "$here/audio.c" \
    "$here/pmuc.c" "$here/cache.c" "$here/rumble.c" "$here/light.c"
"$objcopy" -O binary "$out/mon.elf" "$out/mon.bin"
"$size" "$out/mon.elf"
ls -l "$out/mon.bin"
