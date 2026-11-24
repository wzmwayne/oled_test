#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ezbk(stable) -> .ezzbk 设备格式。

做三件事：
  1. 剥掉 @image / @cover / @blob 与 --- 分隔（设备不显示图片）
  2. 正文去掉行首反斜杠，保留 # / ## 标题行给目录用
  3. 每 16384 字节一块做 raw deflate 压缩，输出可直接给固件随机读取

.ezzbk 布局（小端，v0.1）:
  "EZZB" | u8 algo | u8 flags | u16 reserved
  | u32 rawSize | u32 blockSize | u32 blockCount | u32 metaLen
  | meta[metaLen]（每行 key=value，UTF-8） | u32 off[blockCount+1] | block0 ... blockN

  algo 0 = LZ4，1 = raw deflate（16KB 窗口）。默认输出 deflate。
  元数据只收 title / writer / intro / source / date，设备用其中的 title 当书名。

用法:
  python3 tools/ezbk2ezzbk.py book.ezbk -o book.ezzbk
  python3 tools/ezbk2ezzbk.py book.txt  -o book.ezzbk --lz4
"""
import struct
import sys
import zlib

MAGIC = b'EZZB'
BLOCK = 16384
ALGO_LZ4 = 0
ALGO_DEFLATE = 1
HDR = 24   # "EZZB" | algo | flags | u16 reserved | rawSize | blockSize | blockCount | metaLen


def parse_ezbk(data: bytes):
    """返回 (meta, 线性正文)。stable 档：忽略未知 @ 指令，丢弃图片/封面。"""
    text = data.decode('utf-8', errors='replace')
    if text.startswith('\ufeff'):
        text = text[1:]
    lines = text.split('\n')
    if not lines or lines[0].strip() != 'EZBK/1':
        raise SystemExit('不是 ezbk 文件（magic 不对）')

    meta = {}
    i = 1
    while i < len(lines) and lines[i].startswith('@'):
        body = lines[i][1:]
        key, _, value = body.partition(' ')
        key = key.strip()
        if key and key not in ('image', 'blob', 'cover'):
            meta[key] = value.strip()
        i += 1

    out = []
    for line in lines[i:]:
        if line == '---':
            continue
        if line.startswith('#'):
            out.append(line.rstrip('\r'))          # 卷/章标题，留给目录
            continue
        if line.startswith('@'):
            continue                                 # @image / @cover / @blob 一律丢弃
        if line.startswith('\\'):
            body = line[1:].rstrip('\r')            # 去掉正文前缀
            # 正文以 # 或 \ 开头时保留一个转义反斜杠，避免被当成章标题
            out.append('\\' + body if body[:1] in ('#', '\\') else body)
        elif line.strip() == '':
            out.append('')
        else:
            out.append(line.rstrip('\r'))           # 容错：裸行照收
    return meta, '\n'.join(out).encode('utf-8')


def lz4_block(src: bytes) -> bytes:
    """LZ4 block 压缩（贪心 + 4 字节哈希，窗口 64KB）。"""
    n = len(src)
    out = bytearray()
    table = {}
    anchor = 0
    i = 0
    limit = n - 12
    while i < limit:
        key = src[i:i + 4]
        ref = table.get(key)
        table[key] = i
        if ref is not None and i - ref < 65535 and src[ref:ref + 4] == key:
            mlen = 4
            while i + mlen < n and src[ref + mlen] == src[i + mlen] and mlen < 65535:
                mlen += 1
            lit = i - anchor
            token_lit = 15 if lit >= 15 else lit
            token_ml = 15 if mlen - 4 >= 15 else mlen - 4
            out.append((token_lit << 4) | token_ml)
            if lit >= 15:
                rem = lit - 15
                while rem >= 255:
                    out.append(255); rem -= 255
                out.append(rem)
            out += src[anchor:i]
            out.append((i - ref) & 0xFF)
            out.append(((i - ref) >> 8) & 0xFF)
            if mlen - 4 >= 15:
                rem = mlen - 4 - 15
                while rem >= 255:
                    out.append(255); rem -= 255
                out.append(rem)
            i += mlen
            anchor = i
            continue
        i += 1

    lit = n - anchor
    out.append((15 if lit >= 15 else lit) << 4)
    if lit >= 15:
        rem = lit - 15
        while rem >= 255:
            out.append(255); rem -= 255
        out.append(rem)
    out += src[anchor:]
    return bytes(out)


def lz4_decompress(src: bytes, out_size: int) -> bytes:
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0:
            break
        ml = token & 0x0F
        if ml == 15:
            while True:
                b = src[i]; i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        for k in range(ml):
            out.append(out[start + k])
    if out_size and len(out) != out_size:
        raise SystemExit('解压长度不符: %d != %d' % (len(out), out_size))
    return bytes(out)


def deflate_block(src: bytes) -> bytes:
    c = zlib.compressobj(9, zlib.DEFLATED, -14)   # raw deflate，16KB 窗口
    return c.compress(src) + c.flush()


def encode_block(src: bytes, algo: int) -> bytes:
    return deflate_block(src) if algo == ALGO_DEFLATE else lz4_block(src)


def decode_block(src: bytes, out_size: int, algo: int) -> bytes:
    if algo == ALGO_DEFLATE:
        out = zlib.decompress(src, -14)
        if out_size and len(out) != out_size:
            raise SystemExit('deflate 解压长度不符: %d != %d' % (len(out), out_size))
        return out
    return lz4_decompress(src, out_size)


META_KEYS = ('title', 'writer', 'intro', 'source', 'date')


def escape_meta_value(v: str) -> str:
    """值内换行/回车/反斜杠转义，保证一条记录占一行。"""
    return v.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r')


def build_meta(meta: dict) -> bytes:
    lines = []
    for k in META_KEYS:
        v = (meta.get(k) or '').strip()
        if v:
            lines.append('%s=%s' % (k, escape_meta_value(v)))
    return ('\n'.join(lines) + ('\n' if lines else '')).encode('utf-8')


def build_ezb(raw: bytes, algo: int = ALGO_DEFLATE, meta: bytes = b'') -> bytes:
    blocks = [encode_block(raw[i:i + BLOCK], algo) for i in range(0, len(raw), BLOCK)]
    if not blocks:
        blocks = [encode_block(b'', algo)]
    n = len(blocks)
    header_len = HDR + len(meta) + 4 * (n + 1)
    offs = [header_len]
    for b in blocks:
        offs.append(offs[-1] + len(b))
    head = MAGIC + bytes([algo & 0xFF, 0, 0, 0])
    head += struct.pack('<IIII', len(raw), BLOCK, n, len(meta))
    head += meta
    head += b''.join(struct.pack('<I', o) for o in offs)
    return head + b''.join(blocks)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    src = argv[1]
    dst = 'book.ezzbk'
    if '-o' in argv:
        dst = argv[argv.index('-o') + 1]

    data = open(src, 'rb').read()
    if data.startswith(b'EZBK/1') or data.startswith(b'\xef\xbb\xbfEZBK/1'):
        meta, raw = parse_ezbk(data)
        print('ezbk: %s / %s' % (meta.get('title', ''), meta.get('writer', '')))
    else:
        meta, raw = {}, data
        print('纯文本')

    algo = ALGO_LZ4 if '--lz4' in argv else ALGO_DEFLATE
    mbytes = build_meta(meta)

    # 校验往返：偏移表在 meta 之后，解包时别按固定 24 字节算
    ezb = build_ezb(raw, algo, mbytes)
    back = bytearray()
    raw_size, _bs, n, meta_len = struct.unpack_from('<IIII', ezb, 8)
    off_base = HDR + meta_len
    offs = struct.unpack_from('<%dI' % (n + 1), ezb, off_base)
    for k in range(n):
        blk = ezb[offs[k]:offs[k + 1]]
        want = min(BLOCK, raw_size - k * BLOCK)
        back += decode_block(blk, want, algo)
    assert bytes(back) == raw, '往返不一致'

    open(dst, 'wb').write(ezb)
    print('原文 %d B -> %d B (%.1f%%)  块数 %d  algo=%s  元数据 %d B  -> %s' %
          (len(raw), len(ezb), len(ezb) * 100.0 / max(1, len(raw)), n,
           'deflate' if algo == ALGO_DEFLATE else 'lz4', len(mbytes), dst))


if __name__ == '__main__':
    main(sys.argv)
