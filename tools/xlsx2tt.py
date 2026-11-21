#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把课程表 xlsx 转成 App 可导入的课表元数据。

表格约定：C 列是 `开始-结束`，D..H 列是周一到周五，B 列是节次名。
下午的时间写成 1:50 这种，按「小时 < 7 视为下午」补 12 小时。

用法: python3 tools/xlsx2tt.py 课表.xlsx > tt.json
"""
import json
import re
import sys

import openpyxl

DAY_COL = {'D': 1, 'E': 2, 'F': 3, 'G': 4, 'H': 5, 'I': 6, 'J': 7}


def to_minutes(text):
    m = re.match(r'\s*(\d{1,2}):(\d{2})\s*[-~—]\s*(\d{1,2}):(\d{2})', text)
    if not m:
        return None
    h1, m1, h2, m2 = (int(x) for x in m.groups())
    if h1 < 7:
        h1 += 12
    if h2 < 7:
        h2 += 12
    return h1 * 60 + m1, h2 * 60 + m2


def main(path):
    wb = openpyxl.load_workbook(path, data_only=True)
    ws = wb.worksheets[0]
    courses = []
    for row in ws.iter_rows(min_row=1, max_row=ws.max_row):
        time_cell = row[2] if len(row) > 2 else None
        if time_cell is None or not isinstance(time_cell.value, str):
            continue
        span = to_minutes(time_cell.value)
        if span is None:
            continue
        start, end = span
        for cell in row:
            col = re.sub(r'\d', '', cell.coordinate)
            day = DAY_COL.get(col)
            if not day or cell.value is None:
                continue
            name = str(cell.value).strip()
            if not name:
                continue
            courses.append({'n': name, 'd': day, 's': start, 'e': end, 'p': 0})

    courses.sort(key=lambda c: (c['d'], c['s']))
    print(json.dumps(courses, ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'tt.xlsx')
