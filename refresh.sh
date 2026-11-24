#!/bin/sh
# 编译桌面时钟固件 -> 合并字库 -> 同步到 /sdcard/Download
# 用法: ./refresh.sh    或   ./refresh.sh &   （后台）
set -e

FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc"
SKETCH="/home/wayne/date/oled_test"
BUILD="$SKETCH/.build"
DEST="/sdcard/Download/oled_test/bin"
ESTOOL="$HOME/.arduino15/packages/esp32/tools/esptool_py/5.3.0-cn/esptool"
export PATH="$HOME/bin:$PATH"

MAXSIZE=1048576          # 自定义分区表 app0 = 1MB（写大了会盖掉 font 分区）
CACHE=ccache             # platform.txt 已插入 {compiler.cache_cmd} 占位符

echo "==> 编译"
S=$(date +%s)
arduino-cli compile -j "$(nproc)" --build-path "$BUILD" \
    --build-property "upload.maximum_size=$MAXSIZE" \
    --build-property "compiler.cache_cmd=$CACHE" \
    --fqbn "$FQBN" "$SKETCH"
E=$(date +%s)
echo "==> 编译完成，用时 $((E-S)) 秒"
ccache -s 2>/dev/null | grep -E 'Hits|Misses|cache size' | sed 's/^/    /'

[ -f "$BUILD/oled_test.ino.bin" ] || { echo "找不到 app 产物"; exit 1; }
[ -f "$SKETCH/font.bin" ]        || { echo "找不到 font.bin"; exit 1; }

echo "==> 合并字库进完整镜像"
"$ESTOOL" --chip esp32c3 merge-bin \
    -o "$BUILD/oled-full-font.bin" \
    --pad-to-size 4MB \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0      "$BUILD/oled_test.ino.bootloader.bin" \
    0x8000   "$BUILD/oled_test.ino.partitions.bin" \
    0xe000   "$BUILD/boot_app0.bin" \
    0x10000  "$BUILD/oled_test.ino.bin" \
    0x110000 "$SKETCH/font.bin" >/dev/null

mkdir -p "$DEST"
cp -f "$BUILD/oled-full-font.bin" "$DEST/oled-full.bin"
cp -f "$BUILD/oled_test.ino.bin"  "$DEST/oled-app.bin"
cp -f "$BUILD/boot_app0.bin"      "$DEST/boot_app0.bin"
cp -f "$SKETCH/font.bin"          "$DEST/font.bin"

echo "==> 已同步到 $DEST"
ls -lh "$DEST"

