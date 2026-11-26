#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用文泉驿微米黑烘点阵字库，输出 .h(声明) + .bin(二进制数据)。

为什么要拆成 bin：
  4000+ 字形塞进 C 数组会被编进固件，每次改一行 sketch 都要重新解析 450KB 源码。
  改成二进制后烧到独立 flash 分区，运行时 mmap 直接当数组用，性能和 rodata 一样，
  但编译不再承担这份开销。

二进制格式（小端，每字形 32 字节）:
  uint16 rune     码位
  uint8  w        字形宽度(1-16)
  uint8  pad      对齐填充
  uint16 rows[14] 14 行位图，bit0..bit15 = 第 0..15 列

光栅化参数与 Orange Pi 面板工程 tools/genfont.py 完全一致：
  SIZE=14  ROWS=14  BASELINE=12  PAD=3  阈值>96

用法:
  python3 genfont_c.py                       # GB2312 一二级 + 符号
  python3 genfont_c.py --full                # CJK 4E00-9FFF 全量
  python3 genfont_c.py --chars "测试"         # 只留指定汉字
  python3 genfont_c.py --freq freq.txt --top 6000
                                             # 符号 + GB2312 + 语料前 N 高频字
  语料字频用 tools/freq_chars.py 从小说/项目文本里统计，结果一行一个频率降序。
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FONT = "/home/wayne/date/panel/assets/wqy-microhei.ttc"

SIZE = 14
ROWS = 14
BASELINE = 12
PAD = 3
GLYPH_BYTES = 32          # 2 + 1 + 1pad + 14*2


def render(font, ch):
    adv = font.getlength(ch)
    w = int(round(adv))
    if w <= 0:
        w = 7
    if w > 16:
        w = 16
    img = Image.new("L", (w + PAD * 2, ROWS + BASELINE), 0)
    d = ImageDraw.Draw(img)
    d.text((PAD, BASELINE), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    rows = []
    blank = True
    for r in range(ROWS):
        bits = 0
        for c in range(w):
            if px[PAD + c, r] > 96:
                bits |= 1 << c
                blank = False
        rows.append(bits)
    if blank:
        if ch in (" ", "\t", "\u3000", "\xa0"):
            return w, [0] * ROWS
        return None
    return w, rows


def gb2312_range(hi_from, hi_to):
    out = set()
    for hi in range(hi_from, hi_to):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.add(ord(ch))
    return out


def gb2312_level1():
    """一级常用汉字 3755 个（0xB0-0xD7）"""
    return gb2312_range(0xB0, 0xD8)


def gb2312_level2():
    """二级次常用汉字 3008 个（0xD8-0xF7）——之前整段缺失，是缺字的主因"""
    return gb2312_range(0xD8, 0xF8)


def build_charset(args):
    cs = set(range(0x20, 0x7F))          # ASCII
    cs |= set(range(0x00A0, 0x0100))     # 拉丁补充（° × 等）
    cs |= set(range(0x2000, 0x2070))     # 通用标点：“”‘’…—‰† 等（之前整段缺失）
    cs |= set(range(0x3000, 0x3040))     # CJK 标点：、。「」『』【】・等
    cs |= set(range(0xFF01, 0xFF66))     # 全角 ASCII

    if args.chars:
        cs |= {ord(c) for c in args.chars}
    elif args.full:
        cs |= set(range(0x4E00, 0xA000))      # CJK 基本区全量
        cs |= gb2312_level1() | gb2312_level2()
    else:
        cs |= gb2312_level1() | gb2312_level2()

    # --freq：语料字频表（每行一个字，按频率降序），取前 --top 个并进字集。
    # 和 GB2312 取并集：常用字覆盖日常阅读，GB2312 兜住语料里没出现的常用字。
    if args.freq:
        with open(args.freq, encoding="utf-8") as f:
            freq = [ln.strip() for ln in f if ln.strip()]
        top = args.top if args.top > 0 else len(freq)
        for ch in freq[:top]:
            cs.add(ord(ch[0]))
        print("字频表 %s：共 %d 字，取前 %d" % (args.freq, len(freq), min(top, len(freq))))
    return sorted(cs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--chars", default="")
    ap.add_argument("--freq", default="")
    ap.add_argument("--top", type=int, default=0)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("-o", "--out", default=os.path.join(os.path.dirname(HERE), "font.bin"))
    ap.add_argument("--header", default=os.path.join(os.path.dirname(HERE), "src", "font_wqy14.h"))
    args = ap.parse_args()

    if not os.path.exists(args.font):
        sys.exit("找不到字体文件: " + args.font)

    font = ImageFont.truetype(args.font, SIZE)
    codes, skipped = [], 0
    for code in build_charset(args):
        try:
            got = render(font, chr(code))
        except Exception:
            got = None
        if got is None:
            skipped += 1
            continue
        codes.append((code, got[0], got[1]))

    # ---- 二进制 ----
    buf = bytearray()
    for code, w, rows in codes:
        buf.append(code & 0xFF)
        buf.append((code >> 8) & 0xFF)
        buf.append(w)
        buf.append(0)
        for r in rows:
            buf.append(r & 0xFF)
            buf.append((r >> 8) & 0xFF)
    assert len(buf) == len(codes) * GLYPH_BYTES

    with open(args.out, "wb") as f:
        f.write(buf)

    # ---- 头文件 ----
    H = []
    H.append("// 由 tools/genfont_c.py 生成，请勿手改。")
    H.append("// 字体：文泉驿微米黑 14px（SIZE=14 ROWS=14 BASELINE=12 PAD=3）")
    H.append("// 数据在 font.bin，烧到 font 分区，运行时 mmap 后赋给 gWqy14")
    H.append("#pragma once")
    H.append("#include <stdint.h>")
    H.append("")
    H.append("#define WQY14_HEIGHT 14")
    H.append("#define WQY14_COUNT  %d" % len(codes))
    H.append("#define WQY14_GLYPH_BYTES %d" % GLYPH_BYTES)
    H.append("")
    H.append("typedef struct {")
    H.append("  uint16_t rune;")
    H.append("  uint8_t  w;")
    H.append("  uint16_t rows[WQY14_HEIGHT];")
    H.append("} wqy14_glyph_t;")
    H.append("")
    H.append("/* 由 main 在 mmap font 分区后赋值 */")
    H.append("extern const wqy14_glyph_t *gWqy14;")
    H.append("")
    with open(args.header, "w", encoding="utf-8") as f:
        f.write("\n".join(H))

    print("字体: %s" % args.font)
    print("字形: %d  跳过: %d" % (len(codes), skipped))
    print("bin : %s  %d 字节 (%.1f KB)" % (args.out, len(buf), len(buf) / 1024))
    print("hdr : %s" % args.header)


if __name__ == "__main__":
    main()

