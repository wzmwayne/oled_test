#!/bin/bash
# 把最新 Android App 拉到 /sdcard/Download
# 用法: ./getapk.sh
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"

# 只看 apk-build-<n>，和固件的 build-<n> 区分开
TAG=$(gh release list --repo "$REPO" --limit 30 --json tagName \
        --jq '[.[] | select(.tagName | test("^apk-build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "还没有 App 构建产物" >&2
  exit 1
fi
echo "最新版本: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

gh release download "$TAG" --repo "$REPO" --pattern "oled-panel.apk" --dir "$TMP" --clobber
SRC="$TMP/oled-panel.apk"
[ -f "$SRC" ] || { echo "没下到 apk" >&2; exit 1; }

cp "$SRC" "$DST/oled-panel-$TAG.apk"
printf '  %-30s %8s  sha256=%s\n' "oled-panel-$TAG.apk" \
  "$(du -h "$SRC" | cut -f1)" \
  "$(sha256sum "$SRC" | cut -d' ' -f1)"
echo
echo "安装: 手机文件管理器打开 $DST/oled-panel-$TAG.apk"
