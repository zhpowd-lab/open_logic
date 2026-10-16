#!/usr/bin/env bash
# 只编译到指定新目录；不部署、不修改原 SDK、原项目或启动配置。
set -euo pipefail
task_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
output=${1:?Usage: build.sh NEW_OUTPUT}
output=$(realpath -m -- "$output")
test ! -e "$output" || { echo 'Output already exists' >&2; exit 1; }
: "${A7_PREFIX:?Set bare-metal compiler prefix}"
: "${LINUX_PREFIX:?Set Linux compiler prefix}"
: "${KERNEL_SOURCE:?Set isolated matching kernel source}"
: "${KERNEL_BUILD:?Set prepared matching kernel build}"
mkdir -p "$output/source"
cp -a "$task_root/firmware" "$task_root/linux" "$task_root/test" "$task_root/logic16.h" "$task_root/logic16-layout.h" \
    "$task_root/build.sh" "$task_root/run-device.sh" "$output/source/"
src="$output/source"
# 在 Kbuild 生成中间文件前固定源文件清单，便于核对部署版本对应的本地源码。
(cd "$src" && find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.ld' \
    -o -name '*.py' -o -name '*.sh' -o -name Makefile \) -print0 | sort -z | xargs -0 sha256sum) > "$output/SOURCE_SHA256SUMS"
"${A7_PREFIX}gcc" -E -P -DL16_LINKER -x c "$src/firmware/link.ld" -o "$output/link.ld"
"${A7_PREFIX}gcc" -mcpu=cortex-a7 -marm -mfloat-abi=soft -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -O2 -flto -Wall -Wextra -Werror -nostdlib -nostartfiles \
    -Wl,--build-id=none,-Map,"$output/logic16.map" -T "$output/link.ld" \
    "$src/firmware/start.S" "$src/firmware/main.c" "$src/firmware/board.c" \
    "$src/firmware/memory.c" "$src/firmware/cache.S" -lgcc -o "$output/logic16.elf"
"${A7_PREFIX}readelf" -h -l "$output/logic16.elf" > "$output/elf-layout.txt"
"${A7_PREFIX}objdump" -d "$output/logic16.elf" > "$output/disassembly.txt"
export BSP_TOP=bsp/ KERNEL_SRC="$KERNEL_SOURCE" LICHEE_KERN_DIR="$KERNEL_SOURCE"
make -j4 -C "$KERNEL_SOURCE" O="$KERNEL_BUILD" ARCH=arm CROSS_COMPILE="$LINUX_PREFIX" \
    M="$src/linux" W=1 cmd_checkdoc= EXTRA_CFLAGS=-Werror modules
cp "$src/linux/logic16_access.ko" "$output/"
cp "$src/linux/service.py" "$output/service.py"
cp "$src/run-device.sh" "$output/"
modinfo -F vermagic "$output/logic16_access.ko" > "$output/vermagic.txt"
(cd "$output" && sha256sum logic16.elf logic16_access.ko service.py run-device.sh > SHA256SUMS)
