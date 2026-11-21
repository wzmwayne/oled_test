#!/bin/bash
# 把最新 Android App 拉到 /sdcard/Download
# 用法: ./getapk.sh
#       GH_DIRECT=1 ./getapk.sh   直连 gh CLI（镜像全挂时的手动兜底）
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"

# 镜像表：模式|地址|需要的 Cookie（顺序 = 实测速度）
#   自建代理是替换式：把 github.com 换成它的域名，不接受前缀拼接
MIRRORS=(
  "prefix|https://gh-proxy.com/|"
  "replace|https://gh.gzxz.us.ci/|proxy_consent_v1=1"
  "prefix|https://ghproxy.net/|"
)

TAG=$(gh release list --repo "$REPO" --limit 30 --json tagName \
        --jq '[.[] | select(.tagName | test("^apk-build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "还没有 App 构建产物" >&2
  exit 1
fi
echo "最新版本: $TAG"

OUT="$DST/oled-panel-$TAG.apk"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build_url() {
  local mode="$1" base="$2"
  if [ "$mode" = "replace" ]; then
    printf '%s%s/releases/download/%s/oled-panel.apk' "$base" "$REPO" "$TAG"
  else
    printf '%shttps://github.com/%s/releases/download/%s/oled-panel.apk' "$base" "$REPO" "$TAG"
  fi
}

ok=0
for entry in "${MIRRORS[@]}"; do
  IFS='|' read -r mode base cookie <<< "$entry"
  url=$(build_url "$mode" "$base")
  extra=()
  [ -n "$cookie" ] && extra=(-H "Cookie: $cookie")
  t0=$(date +%s)
  if curl -fsS -L --connect-timeout 8 -m 600 "${extra[@]}" -o "$OUT" "$url" \
     && [ -s "$OUT" ] && ! head -c 16 "$OUT" | grep -qi '<!doctype'; then
    dt=$(( $(date +%s) - t0 )); [ "$dt" -lt 1 ] && dt=1
    echo "  下载 oled-panel.apk  ($(du -h "$OUT" | cut -f1) / ${dt}s via ${base#https://})"
    ok=1
    break
  fi
  echo "  ${base#https://} 不可用，换下一个" >&2
  rm -f "$OUT"
done

if [ "$ok" = "0" ]; then
  echo "  镜像全不通，回退 gh CLI" >&2
  gh release download "$TAG" --repo "$REPO" --pattern "oled-panel.apk" --dir "$TMP" --clobber
  cp "$TMP/oled-panel.apk" "$OUT"
fi

printf '  %-30s %8s  sha256=%s\n' "$(basename "$OUT")" \
  "$(du -h "$OUT" | cut -f1)" \
  "$(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "安装: 文件管理器打开 $OUT"
