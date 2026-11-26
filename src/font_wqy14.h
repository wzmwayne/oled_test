// 由 tools/genfont_c.py 生成，请勿手改。
// 字体：文泉驿微米黑 14px（SIZE=14 ROWS=14 BASELINE=12 PAD=3）
// 数据在 font.bin，烧到 font 分区，运行时 mmap 后由 FNT2 容器解析
#pragma once
#include <stdint.h>

#define WQY14_HEIGHT 14
#define WQY14_COUNT  21419
#define WQY14_TIER_A 3000
#define WQY14_BLOCK_GLYPHS 256
#define WQY14_GLYPH_BYTES 32

typedef struct {
  uint16_t rune;
  uint8_t  w;
  uint16_t rows[WQY14_HEIGHT];
} wqy14_glyph_t;

/* 容器头（mmap 到 font 分区开头） */
typedef struct {
  char     magic[4];
  uint16_t ver;
  uint16_t tier_a_count;
  uint32_t tier_a_off;
  uint16_t block_glyphs;
  uint16_t block_count;
  uint32_t block_idx_off;
  uint32_t data_off;
  uint32_t data_len;
  uint32_t tier_b_count;
  uint32_t total_count;
  uint32_t reserved;
} wqy14_hdr_t;

typedef struct {
  uint16_t first_rune;
  uint16_t count;
  uint32_t rel_off;
} wqy14_blk_t;
