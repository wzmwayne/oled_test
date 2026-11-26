
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统计语料字频，输出一行一个字符（按频率降序），供 genfont_c.py --freq 用。

用法:
  python3 tools/freq_chars.py 小说1.txt 小说2.ezzbk 目录/ -o freq.txt
  python3 tools/freq_chars.py -o freq.txt            # 用下面 DEFAULT_SOURCES

只统计 CJK（4E00-9FFF / 3400-4DBF），其他字符由 genfont_c.py 的符号集合覆盖。
.ezzbk 会先解压再统计（复用 neobook 的 py/ezzbk.py，找不到就跳过）。
"""
import argparse
import collections
import os
import sys

DEFAULT_SOURCES = [
    "/home/wayne/date/ocepub/全部章节文本.txt",
]

CJK = ((0x3400, 0x4DBF), (0x4E00, 0x9FFF))


def is_cjk(o):
    return any(lo <= o <= hi for lo, hi in CJK)


def read_any(path):
    low = path.lower()
    if low.endswith(".ezzbk"):
        sys.path.insert(0, "/home/wayne/date/neobook/py")
        try:
            import ezzbk
        except ImportError:
            return ""
        try:
            return ezzbk.unpack(open(path, "rb").read()).decode("utf-8", "ignore")
        except Exception:
            return ""
    try:
        return open(path, encoding="utf-8", errors="ignore").read()
    except Exception:
        return ""


def collect(paths):
    text = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.lower().endswith((".txt", ".ezzbk")):
                        text.append(read_any(os.path.join(root, f)))
        else:
            text.append(read_any(p))
    return "".join(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", nargs="*", default=[])
    ap.add_argument("-o", "--out", default="freq.txt")
    args = ap.parse_args()

    srcs = args.sources or DEFAULT_SOURCES
    text = collect(srcs)
    if not text:
        sys.exit("没有可统计的文本")

    cnt = collections.Counter(ch for ch in text if is_cjk(ord(ch)))
    total = sum(cnt.values())
    with open(args.out, "w", encoding="utf-8") as f:
        for ch, _ in cnt.most_common():
            f.write(ch + "\n")

    acc = 0
    marks = [1000, 2000, 3000, 4000, 5000, 6000]
    print("语料 %d 字，CJK %d 字，去重 %d" % (len(text), total, len(cnt)))
    for i, (_, c) in enumerate(cnt.most_common(), 1):
        acc += c
        if i in marks:
            print("  前 %5d 覆盖率 %.2f%%" % (i, 100.0 * acc / total))
    print("已写入 %s（%d 字）" % (args.out, len(cnt)))


if __name__ == "__main__":
    main()
