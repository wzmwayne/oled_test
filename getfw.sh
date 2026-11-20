#!/bin/bash
# 把最新 release 的固件拉到 /sdcard/Download
# 用法: ./getfw.sh          -> 只拿 app 固件（刷 0x10000，保留字库/NVS）
#       ./getfw.sh full     -> 同时拿 4MB 整包（刷 0x0，全清）
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"
MODE="${1:-app}"

TAG=$(gh release list --repo "$REPO" --limit 1 --json tagName --jq '.[0].tagName')
if [ -z "$TAG" ]; then
  echo "取不到 release tag" >&2
  exit 1
fi
echo "最新版本: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

get() {
  local pattern="$1" outname="$2"
  gh release download "$TAG" --repo "$REPO" --pattern "$pattern" --dir "$TMP" --clobber
  local src="$TMP/$pattern"
  [ -f "$src" ] || { echo "没下到 $pattern" >&2; return 1; }
  cp "$src" "$DST/$outname"
  printf '  %-34s %8s  sha256=%s\n' "$outname" \
    "$(du -h "$src" | cut -f1)" \
    "$(sha256sum "$src" | cut -d' ' -f1)"
}

echo "写入 $DST:"
get "oled_test.ino.bin" "oled-app-$TAG.bin"

if [ "$MODE" = "full" ]; then
  get "oled-full.bin" "oled-full-$TAG.bin"
  echo
  echo "整包刷写地址 0x0（会清掉 NVS/绑定，刷完手机端要忘记设备重新配对）"
else
  echo
  echo "刷写地址 0x10000（保留字库与 NVS）"
  echo "需要整包请加参数: ./getfw.sh full"
fi

