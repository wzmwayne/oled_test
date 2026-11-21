#!/bin/bash
# 把最新固件拉到 /sdcard/Download
# 用法: ./getfw.sh          -> 只拿 app 固件（刷 0x10000，保留字库/NVS）
#       ./getfw.sh full     -> 同时拿 4MB 整包（刷 0x0，全清）
#       GH_DIRECT=1 ./getfw.sh   直连 gh CLI（镜像全挂时的手动兜底）
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"
MODE="${1:-app}"

# 镜像表：模式|地址|需要的 Cookie
#   prefix  = 前缀式，base + https://github.com/...
#   replace = 替换式，直接把 github.com 换掉（自建代理属于这种，不接受拼接）
# 顺序按实测速度：gh-proxy ~150-400KB/s，自建 ~28KB/s，ghproxy.net 常挂
MIRRORS=(
  "prefix|https://gh-proxy.com/|"
  "replace|https://gh.gzxz.us.ci/|proxy_consent_v1=1"
  "prefix|https://ghproxy.net/|"
)

TAG=$(gh release list --repo "$REPO" --limit 30 --json tagName \
        --jq '[.[] | select(.tagName | test("^build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "取不到 release tag" >&2
  exit 1
fi
echo "最新版本: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build_url() {
  local mode="$1" base="$2" name="$3"
  if [ "$mode" = "replace" ]; then
    printf '%s%s/releases/download/%s/%s' "$base" "$REPO" "$TAG" "$name"
  else
    printf '%shttps://github.com/%s/releases/download/%s/%s' "$base" "$REPO" "$TAG" "$name"
  fi
}

# fetch <asset 名> <落地路径>
fetch() {
  local name="$1" out="$2" entry mode base cookie url t0 dt
  local extra=()
  for entry in "${MIRRORS[@]}"; do
    IFS='|' read -r mode base cookie <<< "$entry"
    url=$(build_url "$mode" "$base" "$name")
    extra=()
    [ -n "$cookie" ] && extra=(-H "Cookie: $cookie")
    t0=$(date +%s)
    if curl -fsS -L --connect-timeout 8 -m 600 "${extra[@]}" -o "$out" "$url" \
       && [ -s "$out" ] && ! head -c 16 "$out" | grep -qi '<!doctype'; then
      dt=$(( $(date +%s) - t0 )); [ "$dt" -lt 1 ] && dt=1
      echo "  下载 $name  ($(du -h "$out" | cut -f1) / ${dt}s via ${base#https://})"
      return 0
    fi
    echo "  ${base#https://} 不可用，换下一个" >&2
    rm -f "$out"
  done
  echo "  镜像全不通，回退 gh CLI" >&2
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
