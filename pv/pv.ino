/*
 * pv.ino — world.execute(me); 风格的字幕 PV（纯视觉，无音乐）
 *
 * 独立小程序，沿用主固件的分区表与 FNT2 字库：
 *   - 只占用 app0（0x10000），字库从 font 分区（0x1A0000）mmap
 *   - 上电直接进 PV 待机画面，按 # 开始播放，按 * 回待机
 *   - 不碰 LittleFS / NVS：刷这个程序不会动书库和配置
 *
 * 刷写：esptool write_flash 0x10000 pv.ino.bin
 * 回到主固件：./getfw.sh 再刷 0x10000
 *
 * 时间轴里只放标题和抽象字形/符号，不含歌词文本；想换成自己的字幕，
 * 改下面的 CUES 表重新编译即可。
 */
#include <Wire.h>
#include <esp_partition.h>
#include <esp_random.h>
#include "miniz.h"
#include "font_wqy14.h"    /* 构建前由 CI/refresh_pv.sh 从 src/ 复制过来 */

#define OLED_SDA   6
#define OLED_SCL   7
#define KEY_HASH   4        /* # 开始 */
#define KEY_STAR   5        /* * 回待机 */

#define SCREEN_W   128
#define SCREEN_H   64
#define I2C_CHUNK  64
#define FRAME_MS   33       /* 30fps */

/* ==================== OLED ==================== */
static uint8_t  gBuf[SCREEN_W * SCREEN_H / 8];
static uint8_t  gOledAddr = 0x3C;
static uint32_t gI2cErr = 0;

static bool i2cWrite(uint8_t addr, uint8_t ctrl, const uint8_t *d, size_t n) {
  bool ok = true;
  for (size_t off = 0; off < n; off += I2C_CHUNK) {
    size_t m = (n - off < I2C_CHUNK) ? (n - off) : I2C_CHUNK;
    Wire.beginTransmission(addr);
    Wire.write(ctrl);
    Wire.write(d + off, m);
    if (Wire.endTransmission() != 0) { ok = false; gI2cErr++; }
  }
  return ok;
}
static void oledCmd(uint8_t c) { i2cWrite(gOledAddr, 0x00, &c, 1); }
static void oledCmdSeq(const uint8_t *c, size_t n) { i2cWrite(gOledAddr, 0x00, c, n); }

static void clearBuf() { memset(gBuf, 0, sizeof(gBuf)); }
static void fillBuf(uint8_t v) { memset(gBuf, v, sizeof(gBuf)); }

static void setPixel(int x, int y, bool on) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
  uint16_t i = (uint16_t)y / 8 * SCREEN_W + x;
  uint8_t  b = 1 << (y & 7);
  if (on) gBuf[i] |= b; else gBuf[i] &= (uint8_t)~b;
}
static void hline(int x0, int x1, int y) { for (int x = x0; x <= x1; x++) setPixel(x, y, true); }
static void vline(int y0, int y1, int x) { for (int y = y0; y <= y1; y++) setPixel(x, y, true); }
static void fillRect(int x0, int y0, int x1, int y1) {
  for (int y = y0; y <= y1; y++) hline(x0, x1, y);
}
static void drawRect(int x0, int y0, int x1, int y1) {
  hline(x0, x1, y0); hline(x0, x1, y1);
  vline(y0, y1, x0); vline(y0, y1, x1);
}
static void flush() {
  static const uint8_t win[6] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
  oledCmdSeq(win, sizeof(win));
  i2cWrite(gOledAddr, 0x40, gBuf, sizeof(gBuf));
}

static bool oledInit() {
  static const uint8_t seq[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
    0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF
  };
  if (!i2cWrite(gOledAddr, 0x00, seq, sizeof(seq))) return false;
  clearBuf(); flush();
  return true;
}

/* ==================== 字库（FNT2 分层容器） ==================== */
static const wqy14_hdr_t   *gFontHdr  = nullptr;
static const wqy14_glyph_t *gTierA    = nullptr;
static const wqy14_blk_t   *gBlkIdx   = nullptr;
static const uint8_t       *gBlkData  = nullptr;
static bool                 gFontOk   = false;
static bool                 gFontLegacy = false;
static wqy14_glyph_t        gBlkGlyphs[WQY14_BLOCK_GLYPHS];
static int                  gBlkCur = -1;
static int                  gBlkCnt = 0;
static tinfl_decompressor  *gTinf = nullptr;

static bool fontLoadBlock(int b) {
  if (b == gBlkCur) return true;
  if (!gFontHdr || b < 0 || b >= gFontHdr->block_count) return false;
  if (!gTinf) {
    gTinf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    if (!gTinf) return false;
  }
  const wqy14_blk_t *e = &gBlkIdx[b];
  uint32_t off = e->rel_off;
  uint32_t end = (b + 1 < gFontHdr->block_count) ? gBlkIdx[b + 1].rel_off : gFontHdr->data_len;
  if (end <= off || end > gFontHdr->data_len) return false;

  tinfl_init(gTinf);
  size_t inLen = end - off, outLen = sizeof(gBlkGlyphs);
  tinfl_status st = tinfl_decompress(gTinf, gBlkData + off, &inLen,
                                     (uint8_t *)gBlkGlyphs, (uint8_t *)gBlkGlyphs,
                                     &outLen, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  uint32_t cnt = (uint32_t)(outLen / WQY14_GLYPH_BYTES);
  if (st != TINFL_STATUS_DONE || cnt == 0 || cnt > WQY14_BLOCK_GLYPHS) { gBlkCur = -1; return false; }
  gBlkCnt = (int)cnt;
  gBlkCur = b;
  return true;
}

static bool fontMount() {
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "font");
  if (!p) return false;
  const void *ptr = nullptr;
  esp_partition_mmap_handle_t map = 0;
  if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &map) != ESP_OK || !ptr)
    return false;

  const wqy14_hdr_t *h = (const wqy14_hdr_t *)ptr;
  const uint8_t *base = (const uint8_t *)ptr;
  bool ok = (p->size > sizeof(wqy14_hdr_t))
            && !memcmp(h->magic, "FNT2", 4)
            && (h->ver == 3 || h->ver == 2)
            && h->tier_a_count == WQY14_TIER_A
            && h->total_count == WQY14_COUNT
            && h->block_glyphs == WQY14_BLOCK_GLYPHS
            && (uint32_t)h->tier_a_off + (uint32_t)h->tier_a_count * WQY14_GLYPH_BYTES <= p->size
            && (uint32_t)h->block_idx_off + (uint32_t)h->block_count * sizeof(wqy14_blk_t) <= p->size
            && (uint32_t)h->data_off + h->data_len <= p->size;
  if (!ok) return false;

  gFontHdr = h;
  gFontLegacy = (h->ver == 2);
  gTierA   = (const wqy14_glyph_t *)(base + h->tier_a_off);
  gBlkIdx  = (const wqy14_blk_t *)(base + h->block_idx_off);
  gBlkData = base + h->data_off;
  gFontOk  = true;
  return true;
}

static const wqy14_glyph_t *findGlyph(uint32_t cp) {
  if (!gFontOk || cp > 0xFFFF) return nullptr;
  int lo = 0, hi = (int)gFontHdr->tier_a_count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t r = gTierA[mid].rune;
    if (r == cp) return &gTierA[mid];
    if (r < cp) lo = mid + 1; else hi = mid - 1;
  }
  lo = 0; hi = (int)gFontHdr->block_count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    const wqy14_blk_t *e = &gBlkIdx[mid];
    uint32_t upper = gFontLegacy ? (uint32_t)e->first_rune + e->last_rune : e->last_rune;
    if (cp < e->first_rune) { hi = mid - 1; continue; }
    if (cp > upper)         { lo = mid + 1; continue; }
    if (!fontLoadBlock(mid)) return nullptr;
    for (int k = 0; k < gBlkCnt; k++)
      if (gBlkGlyphs[k].rune == cp) return &gBlkGlyphs[k];
    return nullptr;
  }
  return nullptr;
}

/* ==================== 文本 ==================== */
static uint32_t utf8Next(const char *&s) {
  uint8_t c = (uint8_t)*s++;
  if (c < 0x80) return c;
  uint32_t cp; int n;
  if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; n = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
  else return '?';
  while (n-- > 0) {
    if ((*s & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | ((uint8_t)*s++ & 0x3F);
  }
  return cp;
}

static int drawTextMode(int x, int y, const char *s, uint8_t scale, bool mode) {
  int cx = x;
  const char *p = s;
  while (*p) {
    uint32_t cp = utf8Next(p);
    const wqy14_glyph_t *g = findGlyph(cp);
    if (!g) { cx += 8 * scale; continue; }
    for (int row = 0; row < WQY14_HEIGHT; row++) {
      uint16_t bits = g->rows[row];
      if (!bits) continue;
      for (int col = 0; col < g->w && col < 16; col++) {
        if (!(bits & (1u << col))) continue;
        if (scale == 1) setPixel(cx + col, y + row, mode);
        else for (int dy = 0; dy < scale; dy++)
               for (int dx = 0; dx < scale; dx++)
                 setPixel(cx + col * scale + dx, y + row * scale + dy, mode);
      }
    }
    cx += g->w * scale;
  }
  return cx;
}

static int textWidth(const char *s, uint8_t scale) {
  int w = 0; const char *p = s;
  while (*p) { const wqy14_glyph_t *g = findGlyph(utf8Next(p)); w += (g ? g->w : 8) * scale; }
  return w;
}
static void drawText(int x, int y, const char *s) { drawTextMode(x, y, s, 1, true); }
static void drawCenter(int y, const char *s, uint8_t scale) {
  int w = textWidth(s, scale);
  drawTextMode((SCREEN_W - w) / 2, y, s, scale, true);
}

/* ==================== 时间轴 ==================== */
enum Fx {
  FX_TITLE,     /* 标题两行 + 闪烁光标 */
  FX_TYPE,      /* 打字机 */
  FX_BIG,       /* 2 倍大字 + 反白闪 */
  FX_GLITCH,    /* 文字 + 随机行错位/噪点 */
  FX_BARS,      /* 竖条扫过 + 小字 */
  FX_MATRIX,    /* 0/1 雨 */
  FX_INVERT,    /* 反白底黑字 */
  FX_SCROLL,    /* 横向滚动 */
  FX_COUNT      /* 帧计数 */
};

typedef struct {
  uint16_t    dur;
  uint8_t     fx;
  const char *txt;
} Cue;

/* 只放标题与抽象符号，不含歌词。想换成自己的字幕就改这张表。 */
static const Cue CUES[] = {
  { 2600, FX_TITLE,   "world.execute(me);" },
  { 1400, FX_TYPE,    "execute" },
  { 1400, FX_TYPE,    "me;" },
  { 1600, FX_BIG,     "me;" },
  { 1200, FX_GLITCH,  "world.execute(me);" },
  { 1500, FX_BARS,    "SYSTEM" },
  { 2400, FX_MATRIX,  "" },
  { 1200, FX_INVERT,  "01" },
  { 2400, FX_SCROLL,  "// world.execute(me); //" },
  { 1500, FX_COUNT,   "" },
  { 2600, FX_TITLE,   "world.execute(me);" },
};
#define CUE_N (sizeof(CUES) / sizeof(CUES[0]))

static uint32_t gTotalMs = 0;

static void computeTotal() {
  gTotalMs = 0;
  for (uint32_t i = 0; i < CUE_N; i++) gTotalMs += CUES[i].dur;
}

/* ==================== 效果 ==================== */
static void fxTitle(const char *txt, uint32_t ms, uint16_t dur) {
  drawCenter(18, "world.", 1);
  drawCenter(36, "execute(me);", 1);
  if (((ms / 400) & 1) == 0) {
    int w2 = textWidth("execute(me);", 1);
    int cx = (SCREEN_W + w2) / 2 + 2;          /* 紧跟第二行文字右边 */
    fillRect(cx, 36, cx + 1, 48);
  }
  if (ms > dur - 400) { fillRect(0, 0, SCREEN_W - 1, 13); }
  (void)txt;
}

static void fxType(const char *txt, uint32_t ms, uint16_t dur) {
  int len = 0;
  for (const char *p = txt; *p; ) { utf8Next(p); len++; }
  int shown = (int)((uint64_t)len * ms / (dur > 600 ? dur - 300 : dur));
  if (shown > len) shown = len;
  char buf[40];
  int o = 0, k = 0;
  for (const char *p = txt; *p && k < shown; ) {
    const char *q = p; utf8Next(q);
    int n = (int)(q - p);
    if (o + n < (int)sizeof(buf)) { memcpy(buf + o, p, n); o += n; }
    p = q; k++;
  }
  buf[o] = 0;
  drawCenter(24, buf, 1);
  if (((ms / 200) & 1) == 0) {
    int w = textWidth(buf, 1);
    fillRect((SCREEN_W - w) / 2 + w + 1, 24, (SCREEN_W - w) / 2 + w + 2, 36);
  }
}

static void fxBig(const char *txt, uint32_t ms, uint16_t dur) {
  bool inv = ((ms / 250) & 1) != 0;
  if (inv) fillBuf(0xFF);
  int w = textWidth(txt, 2);
  drawTextMode((SCREEN_W - w) / 2, 18, txt, 2, !inv);
  if (!inv) { hline(20, 108, 50); hline(20, 108, 12); }
  (void)dur;
}

static void fxGlitch(const char *txt, uint32_t ms, uint16_t dur) {
  int w = textWidth(txt, 1);
  int x = (SCREEN_W - w) / 2;
  int y = 24;
  drawText(x, y, txt);
  /* 随机把几行整体左右推一下，制造错位 */
  int shift = (int)(ms / 60) % 7;
  for (int r = 0; r < 3; r++) {
    int yy = 20 + r * 10 + (esp_random() % 3);
    int dx = (int)(esp_random() % 11) - 5 + shift;
    for (int xx = 0; xx < SCREEN_W; xx++) {
      int sx = xx + dx;
      if (sx < 0 || sx >= SCREEN_W) continue;
      uint16_t i = (uint16_t)yy / 8 * SCREEN_W + xx;
      uint8_t  b = 1 << (yy & 7);
      uint16_t j = (uint16_t)yy / 8 * SCREEN_W + sx;
      if (gBuf[j] & b) gBuf[i] |= b; else gBuf[i] &= (uint8_t)~b;
    }
  }
  for (int i = 0; i < 12; i++) setPixel(esp_random() % SCREEN_W, esp_random() % 14, true);
  (void)dur;
}

static void fxBars(const char *txt, uint32_t ms, uint16_t dur) {
  int phase = (int)(ms / 8);
  for (int x = 0; x < SCREEN_W; x += 8) {
    int h = (int)((x * 7 + phase * 3) % 32) + 4;
    fillRect(x, SCREEN_H - h, x + 3, SCREEN_H - 1);
  }
  fillRect(0, 0, SCREEN_W - 1, 13);
  drawCenter(0, txt, 1);
  (void)dur;
}

static void fxMatrix(uint32_t ms) {
  static const char *D = "01";
  for (int c = 0; c < 16; c++) {
    int x = c * 8;
    int head = (int)((ms / 18 + c * 37) % 72) - 8;
    for (int k = 0; k < 5; k++) {
      int y = head - k * 12;
      if (y < -12 || y > SCREEN_H) continue;
      char s[2] = { D[(c + k) & 1], 0 };
      drawText(x, y, s);
    }
  }
}

static void fxInvert(const char *txt) {
  fillBuf(0xFF);
  int w = textWidth(txt, 2);
  drawTextMode((SCREEN_W - w) / 2, 18, txt, 2, false);
  drawRect(2, 2, SCREEN_W - 3, SCREEN_H - 3);
}

static void fxScroll(const char *txt, uint32_t ms, uint16_t dur) {
  int w = textWidth(txt, 1);
  int span = w + SCREEN_W;
  int x = SCREEN_W - (int)((uint64_t)span * ms / (dur ? dur : 1));
  drawText(x, 24, txt);
}

static void fxCount(uint32_t ms, uint32_t t) {
  char b[16];
  snprintf(b, sizeof(b), "%05u", (unsigned)(t / 100));
  drawCenter(18, b, 2);
  char b2[16];
  snprintf(b2, sizeof(b2), "%02u%%", (unsigned)(100.0 * t / (gTotalMs ? gTotalMs : 1)));
  drawCenter(48, b2, 1);
  (void)ms;
}

/* 播放一帧：t 为整段时间轴上的毫秒数 */
static void playFrame(uint32_t t) {
  if (gTotalMs == 0) return;
  uint32_t tt = t % gTotalMs;
  uint32_t acc = 0;
  for (uint32_t i = 0; i < CUE_N; i++) {
    if (tt < acc + CUES[i].dur) {
      uint32_t local = tt - acc;
      clearBuf();
      switch (CUES[i].fx) {
        case FX_TITLE:  fxTitle(CUES[i].txt, local, CUES[i].dur); break;
        case FX_TYPE:   fxType(CUES[i].txt, local, CUES[i].dur); break;
        case FX_BIG:    fxBig(CUES[i].txt, local, CUES[i].dur); break;
        case FX_GLITCH: fxGlitch(CUES[i].txt, local, CUES[i].dur); break;
        case FX_BARS:   fxBars(CUES[i].txt, local, CUES[i].dur); break;
        case FX_MATRIX: fxMatrix(local); break;
        case FX_INVERT: fxInvert(CUES[i].txt); break;
        case FX_SCROLL: fxScroll(CUES[i].txt, local, CUES[i].dur); break;
        default:        fxCount(local, t); break;
      }
      /* 底部进度条 */
      int pw = (int)((uint64_t)SCREEN_W * tt / gTotalMs);
      if (pw > 0) hline(0, pw - 1, SCREEN_H - 1);
      return;
    }
    acc += CUES[i].dur;
  }
}

/* ==================== 待机画面 ==================== */
static void idleFrame(uint32_t t) {
  clearBuf();
  drawCenter(10, "world.execute", 1);
  drawCenter(26, "(me);", 1);
  hline(26, 101, 42);
  if (((t / 500) & 1) == 0) drawCenter(46, "按 # 开始", 1);
  flush();
}

/* ==================== 按键 ==================== */
static bool gHashPrev = false, gStarPrev = false;

static bool pressed(uint8_t pin, bool &prev) {
  bool cur = (digitalRead(pin) == LOW);
  bool hit = cur && !prev;
  prev = cur;
  return hit;
}

/* ==================== setup/loop ==================== */
static bool gPlaying = false;
static uint32_t gStartMs = 0;

void setup() {
  pinMode(KEY_HASH, INPUT_PULLUP);
  pinMode(KEY_STAR, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL, 100000);
  Wire.setClock(400000);
  Wire.beginTransmission(0x3D);
  if (Wire.endTransmission() == 0) gOledAddr = 0x3D;
  oledInit();

  fontMount();
  computeTotal();
  gStartMs = millis();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();

  if (pressed(KEY_HASH, gHashPrev)) {
    gPlaying = true;
    gStartMs = now;            /* 每次按 # 都从头播 */
  }
  if (pressed(KEY_STAR, gStarPrev)) gPlaying = false;

  if (gPlaying) {
    if (now - last >= FRAME_MS) {
      last = now;
      playFrame(now - gStartMs);
      flush();
    }
  } else {
    if (now - last >= FRAME_MS) {
      last = now;
      idleFrame(now - gStartMs);
    }
  }
}
