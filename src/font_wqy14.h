// 由 tools/genfont_c.py 生成，请勿手改。
// 字体：文泉驿微米黑 14px（SIZE=14 ROWS=14 BASELINE=12 PAD=3）
// 数据在 font.bin，烧到 font 分区，运行时 mmap 后赋给 gWqy14
#pragma once
#include <stdint.h>

#define WQY14_HEIGHT 14
#define WQY14_COUNT  4015
#define WQY14_GLYPH_BYTES 32

typedef struct {
  uint16_t rune;
  uint8_t  w;
  uint16_t rows[WQY14_HEIGHT];
} wqy14_glyph_t;

/* 由 main 在 mmap font 分区后赋值 */
extern const wqy14_glyph_t *gWqy14;
