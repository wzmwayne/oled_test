#!/bin/bash
# 把最新 Android App 拉到 /sdcard/Download
# 用法: ./getapk.sh
# 下载默认走 gh-proxy 镜像；GH_DIRECT=1 回退 gh CLI
set -euo pipefail

REPO="wzmwayne/oled_test"
DST="/sdcard/Download"
MIRRORS=("https://gh-proxy.com/" "https://ghproxy.net/")

TAG=$(gh release list --repo "$REPO" --limit 30 --json tagName \
        --jq '[.[] | select(.tagName | test("^apk-build-[0-9]+$"))][0].tagName')
if [ -z "${TAG:-}" ]; then
  echo "还没有 App 构建产物" >&2
  exit 1
fi
echo "最新版本: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

OUT="$DST/oled-panel-$TAG.apk"
done_dl=0
if [ "${GH_DIRECT:-0}" != "1" ]; then
  for m in "${MIRRORS[@]}"; do
    url="${m}https://github.com/$REPO/releases/download/$TAG/oled-panel.apk"
    t0=$(date +%s)
    if curl -fsS -L --connect-timeout 8 --retry 2 -m 600 -o "$OUT" "$url" && [ -s "$OUT" ]; then
      dt=$(( $(date +%s) - t0 )); [ "$dt" -lt 1 ] && dt=1
      echo "  下载 oled-panel.apk  ($(du -h "$OUT" | cut -f1) / ${dt}s via ${m#https://})"
      done_dl=1
      break
    fi
    echo "  ${m} 失败，试下一个" >&2
    rm -f "$OUT"
  done
fi
if [ "$done_dl" = "0" ]; then
  echo "  镜像都不通，回退 gh CLI" >&2
  gh release download "$TAG" --repo "$REPO" --pattern "oled-panel.apk" --dir "$TMP" --clobber
  cp "$TMP/oled-panel.apk" "$OUT"
fi

printf '  %-30s %8s  sha256=%s\n' "$(basename "$OUT")" \
  "$(du -h "$OUT" | cut -f1)" \
  "$(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "安装: 文件管理器打开 $OUT"
