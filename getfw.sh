#!/bin/bash
# 把最新固件拉到 /sdcard/Download
# 用法: ./getfw.sh          -> 只拿 app 固件（刷 0x10000，保留字库/NVS）
#       ./getfw.sh full     -> 同时拿 4MB 整包（刷 0x0，全清）
# 下载默认走 gh-proxy 镜像；直连不通时用 GH_DIRECT=1 回退 gh CLI
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"
MODE="${1:-app}"
MIRRORS=("https://gh-proxy.com/" "https://ghproxy.net/")

# 只看固件构建（build-<n>），排除安卓 App 的 apk-build-<n>
TAG=$(gh release list --repo "$REPO" --limit 30 --json tagName \
        --jq '[.[] | select(.tagName | test("^build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "取不到 release tag" >&2
  exit 1
fi
echo "最新版本: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# fetch <asset 名> <落地路径>
fetch() {
  local name="$1" out="$2" m url t0 dt
  if [ "${GH_DIRECT:-0}" != "1" ]; then
    for m in "${MIRRORS[@]}"; do
      url="${m}https://github.com/$REPO/releases/download/$TAG/$name"
      t0=$(date +%s)
      if curl -fsS -L --connect-timeout 8 --retry 2 -m 600 -o "$out" "$url" && [ -s "$out" ]; then
        dt=$(( $(date +%s) - t0 )); [ "$dt" -lt 1 ] && dt=1
        echo "  下载 $name  ($(du -h "$out" | cut -f1) / ${dt}s via ${m#https://})"
        return 0
      fi
      echo "  ${m} 失败，试下一个" >&2
      rm -f "$out"
    done
  fi
  echo "  镜像都不通，回退 gh CLI" >&2
  gh release download "$TAG" --repo "$REPO" --pattern "$name" --dir "$TMP" --clobber
  cp "$TMP/$name" "$out"
}

put() {
  local name="$1" outname="$2"
  fetch "$name" "$TMP/$outname"
  cp "$TMP/$outname" "$DST/$outname"
  printf '  %-34s %8s  sha256=%s\n' "$outname" \
    "$(du -h "$DST/$outname" | cut -f1)" \
    "$(sha256sum "$DST/$outname" | cut -d' ' -f1)"
}

echo "写入 $DST:"
put "oled_test.ino.bin" "oled-app-$TAG.bin"

if [ "$MODE" = "full" ]; then
  put "oled-full.bin" "oled-full-$TAG.bin"
  echo
  echo "整包刷写地址 0x0（会清掉 NVS/绑定，刷完手机端要忘记设备重新配对）"
else
  echo
  echo "刷写地址 0x10000（保留字库与 NVS）"
  echo "需要整包请加参数: ./getfw.sh full"
fi
