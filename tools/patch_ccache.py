#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给 arduino-esp32 的 platform.txt 插入 ccache 占位符。

3.x 的 recipe 里没有 {compiler.cache_cmd}，直接传 --build-property 无效。
本脚本在 recipe.c.o / recipe.cpp.o 前插入占位符，并定义空默认值。

GitHub Actions 的 checkout 会刷新所有文件 mtime，导致 arduino-cli 认为
.o 全部过期、每次全量重编；ccache 按内容哈希命中，正好绕过这个问题。
"""
import glob
import io
import sys

patched = 0
for p in glob.glob('/home/runner/.arduino15/packages/esp32/hardware/esp32/*/platform.txt') + \
         glob.glob('/home/*/.arduino15/packages/esp32/hardware/esp32/*/platform.txt'):
    s = io.open(p, encoding='utf-8').read()
    if '{compiler.cache_cmd}' in s:
        print('已含占位符:', p)
        continue
    if 'compiler.cache_cmd=' not in s:
        s = s.replace('compiler.c.cmd={compiler.prefix}gcc',
                      'compiler.cache_cmd=' + chr(10) + 'compiler.c.cmd={compiler.prefix}gcc', 1)
    for key in ('recipe.c.o.pattern=', 'recipe.cpp.o.pattern='):
        i = s.find(key)
        if i < 0:
            continue
        j = i + len(key)
        s = s[:j] + '{compiler.cache_cmd} ' + s[j:]
    io.open(p, 'w', encoding='utf-8').write(s)
    print('已打补丁:', p)
    patched += 1

if patched == 0:
    print('没有需要处理的 platform.txt', file=sys.stderr)

