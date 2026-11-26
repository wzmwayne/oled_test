#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用文泉驿微米黑烘点阵字库，输出 .h(声明) + .bin(二进制数据)。

为什么要拆成 bin：
  4000+ 字形塞进 C 数组会被编进固件，每次改一行 sketch 都要重新解析 450KB 源码。
  改成二进制后烧到独立 flash 分区，运行时 mmap 直接当数组用，性能和 rodata 一样，
  但编译不再承担这份开销。

二进制格式（FNT2 容器，小端）:
  头 40 字节: "FNT2" | u16 ver | u16 tierA 字数 | u32 tierA 偏移
              | u16 每块字数 | u16 块数 | u32 块表偏移 | u32 数据偏移
              | u32 数据长度 | u32 tierB 字数 | u32 总字数 | u32 保留
  tierA: 32 字节/字，按码位升序（二进制查找，不用解压）
  tierB: 块表 {u16 首码位, u16 字数, u32 相对偏移} + 每块独立 raw deflate
  字形记录: u16 rune | u8 w | u8 pad | u16 rows[14]

光栅化参数与 Orange Pi 面板工程 tools/genfont.py 完全一致：
  SIZE=14  ROWS=14  BASELINE=12  PAD=3  阈值>96

用法:
  python3 genfont_c.py --full                # 全 CJK（21419 字，约 441KB，推荐）
  python3 genfont_c.py                       # 只做 GB2312 一二级 + 符号
  python3 genfont_c.py --chars "测试"         # 只留指定汉字
  python3 genfont_c.py --full --tier-a 3000  # 指定"常用字原样存"的个数

分层依据是 tools/freq_zh.txt（Jun Da《现代汉语单字频率列表》，公共语料统计）：
排名靠前的字原样存（查找不用解压），其余按码位分块 deflate。
"""
import argparse
import os
import struct
import sys
import zlib

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

    # --freq：字频表（每行一个字，按频率降序），取前 --top 个并进字集。
    # 默认是 tools/freq_zh.txt（公共统计），既决定字集补充，也决定分层顺序。
    if args.freq and os.path.exists(args.freq):
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
    ap.add_argument("--freq", default=os.path.join(HERE, "freq_zh.txt"))
    ap.add_argument("--top", type=int, default=0)
    ap.add_argument("--tier-a", type=int, default=3000,
                    help="常用字原样存几个（其余分块压缩），0 表示全部原样")
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

    # ---- 二进制（FNT2 分层容器） ----
    def rec(code, w, rows):
        b = bytearray()
        b.append(code & 0xFF)
        b.append((code >> 8) & 0xFF)
        b.append(w)
        b.append(0)
        for r in rows:
            b.append(r & 0xFF)
            b.append((r >> 8) & 0xFF)
        return bytes(b)

    # 常用度排序：--freq 给的表越靠前越常用；表外的字排在最后（按码位）
    rank = {}
    if args.freq and os.path.exists(args.freq):
        i = 0
        for ln in open(args.freq, encoding="utf-8"):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            rank[ord(ln[0])] = i
            i += 1
        print("字频表 %s：%d 条" % (args.freq, len(rank)))

    by_code = {code: (w, rows) for code, w, rows in codes}
    tier_n = args.tier_a if args.tier_a > 0 else len(by_code)
    order = sorted(by_code, key=lambda c: (rank.get(c, 1 << 30), c))
    tier_a = sorted(order[:tier_n])
    tier_b = sorted(order[tier_n:])

    # tierB 分块压缩（每块 block_glyphs 字）
    block_glyphs = 256
    blocks, idx = [], []
    for i in range(0, len(tier_b), block_glyphs):
        part = tier_b[i:i + block_glyphs]
        raw = b"".join(rec(c, *by_code[c]) for c in part)
        cobj = zlib.compressobj(9, zlib.DEFLATED, -14)
        comp = cobj.compress(raw) + cobj.flush()
        idx.append((part[0], len(part), len(comp)))
        blocks.append(comp)

    hdr_len = 40
    tierA_off = hdr_len
    tierA_bytes = b"".join(rec(c, *by_code[c]) for c in tier_a)
    block_idx_off = tierA_off + len(tierA_bytes)
    data_off = block_idx_off + len(idx) * 8
    data_len = sum(len(b) for b in blocks)

    buf = bytearray()
    buf += b"FNT2"
    buf += struct.pack("<HH", 2, len(tier_a))
    buf += struct.pack("<I", tierA_off)
    buf += struct.pack("<HH", block_glyphs, len(idx))
    buf += struct.pack("<I", block_idx_off)
    buf += struct.pack("<I", data_off)
    buf += struct.pack("<I", data_len)
    buf += struct.pack("<II", len(tier_b), len(by_code))
    buf += struct.pack("<I", 0)
    assert len(buf) == hdr_len, len(buf)

    buf += tierA_bytes
    rel = 0
    for first, cnt, clen in idx:
        buf += struct.pack("<HHI", first, cnt, rel)
        rel += clen
    for b in blocks:
        buf += b
    assert len(buf) == data_off + data_len

    if not args.full:
        pass
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
    H.append("#define WQY14_TIER_A %d" % len(tier_a))
    H.append("#define WQY14_BLOCK_GLYPHS %d" % block_glyphs)
    H.append("#define WQY14_GLYPH_BYTES %d" % GLYPH_BYTES)
    H.append("")
    H.append("typedef struct {")
    H.append("  uint16_t rune;")
    H.append("  uint8_t  w;")
    H.append("  uint16_t rows[WQY14_HEIGHT];")
    H.append("} wqy14_glyph_t;")
    H.append("")
    H.append("/* 容器头（mmap 到 font 分区开头） */")
    H.append("typedef struct {")
    H.append("  char     magic[4];")
    H.append("  uint16_t ver;")
    H.append("  uint16_t tier_a_count;")
    H.append("  uint32_t tier_a_off;")
    H.append("  uint16_t block_glyphs;")
    H.append("  uint16_t block_count;")
    H.append("  uint32_t block_idx_off;")
    H.append("  uint32_t data_off;")
    H.append("  uint32_t data_len;")
    H.append("  uint32_t tier_b_count;")
    H.append("  uint32_t total_count;")
    H.append("  uint32_t reserved;")
    H.append("} wqy14_hdr_t;")
    H.append("")
    H.append("typedef struct {")
    H.append("  uint16_t first_rune;")
    H.append("  uint16_t count;")
    H.append("  uint32_t rel_off;")
    H.append("} wqy14_blk_t;")
    H.append("")
    with open(args.header, "w", encoding="utf-8") as f:
        f.write("\n".join(H))

    print("字体: %s" % args.font)
    print("字形: %d  跳过: %d" % (len(codes), skipped))
    print("bin : %s  %d 字节 (%.1f KB)  tierA=%d (%d KB) tierB=%d/%d 块"
          % (args.out, len(buf), len(buf) / 1024, len(tier_a), len(tierA_bytes) // 1024,
             len(tier_b), len(idx)))
    print("hdr : %s" % args.header)


if __name__ == "__main__":
    main()

