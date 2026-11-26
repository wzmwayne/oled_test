#!/bin/bash
# 把最新的 PV 程序拉到 /sdcard/Download
# 用法: ./getpv.sh          -> oled-pv-<tag>.bin，刷 0x10000
#       GH_DIRECT=1 ./getpv.sh   直连 gh CLI
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"

MIRRORS=(
  "prefix|https://gh-proxy.com/|"
  "replace|https://gh.gzxz.us.ci/|proxy_consent_v1=1"
  "prefix|https://ghproxy.net/|"
)

TAG=$(gh release list --repo "$REPO" --limit 40 --json tagName \
        --jq '[.[] | select(.tagName | test("^pv-build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "还没有 PV 构建产物（先跑 pv workflow）" >&2
  exit 1
fi
echo "最新版本: $TAG"

OUT="$DST/oled-pv-$TAG.bin"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build_url() {
  local mode="$1" base="$2"
  if [ "$mode" = "replace" ]; then
    printf '%s%s/releases/download/%s/pv.ino.bin' "$base" "$REPO" "$TAG"
  else
    printf '%shttps://github.com/%s/releases/download/%s/pv.ino.bin' "$base" "$REPO" "$TAG"
  fi
}

ok=0
for entry in "${MIRRORS[@]}"; do
  IFS='|' read -r mode base cookie <<< "$entry"
  url=$(build_url "$mode" "$base")
  extra=()
  [ -n "$cookie" ] && extra=(-H "Cookie: $cookie")
  if curl -fsS -L --connect-timeout 8 -m 300 "${extra[@]}" -o "$OUT" "$url" \
     && [ -s "$OUT" ] && ! head -c 16 "$OUT" | grep -qi '<!doctype'; then
    ok=1
    break
  fi
  echo "  ${base#https://} 不可用，换下一个" >&2
  rm -f "$OUT"
done

if [ "$ok" = "0" ]; then
  gh release download "$TAG" --repo "$REPO" --pattern "pv.ino.bin" --dir "$TMP" --clobber
  cp "$TMP/pv.ino.bin" "$OUT"
fi

printf '  %-30s %8s  sha256=%s\n' "$(basename "$OUT")" \
  "$(du -h "$OUT" | cut -f1)" "$(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "刷写: esptool --chip esp32c3 -p /dev/ttyACM0 -b 921600 write_flash 0x10000 $OUT"
