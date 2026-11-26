/*
 * pv.ino — 128x64 单色 OLED 版 world.execute(me); 视觉 PV（无音乐、无歌词）
 *
 * 结构参考 FrostNovaOrg/world-execute-web（AGPL-3.0）公开的分段时间轴与
 * 视觉母题（终端 / 晶格 / 涟漪 / 隧道 / 网络 / 字形 / 频谱 / 翻牌 /
 * 碎屑 / 损坏 / 像素排序 / 代码 / 心电 / 地球 / 标志）。
 * 只借“哪一段是哪种画面”的结构与 130 BPM 节拍；代码自写，未复制其源码、
 * 字体、图片、音频。歌词与官方译文版权归 Mili，本项目不收录。
 *
 * 为这块屏专门做的取舍（不是把网页版硬塞进来）：
 *   - 1 bit 单色：只用实心块/粗线/大字，不搞细线线框和灰阶抖动
 *   - 128x64 太小：主体走 2 倍字（4 个字一行）或整块图形，细节一律砍掉
 *   - 防烧屏：不放常驻进度条、不在固定位置长期挂小字；
 *     章节号只在每段开场闪 2 秒，反白闪每次只留 1~2 帧
 *   - 帧率 25fps：整帧 1KB，400kHz I2C 下约 25KB/s，留出余量
 *
 * 独立程序：沿用主固件分区表与 FNT2 字库，只占 app0（0x10000），
 * 不碰 NVS / LittleFS。上电进待机，按 # 从头播放，按 * 回待机。
 */
#include <Wire.h>
#include <esp_partition.h>
#include <esp_random.h>
#include "miniz.h"
#include "font_wqy14.h"

#define OLED_SDA   6
#define OLED_SCL   7
#define KEY_HASH   4
#define KEY_STAR   5

#define SCREEN_W   128
#define SCREEN_H   64
#define I2C_CHUNK  64
#define FRAME_MS   40          /* 25fps，给 I2C 留余量 */

#define BEAT_MS    461.5f      /* 130 BPM 4/4，取自公开分析数据 */
#define BEAT0_MS   297.6f

/* ==================== OLED ==================== */
static uint8_t  gBuf[SCREEN_W * SCREEN_H / 8];
static uint8_t  gOledAddr = 0x3C;

static bool i2cWrite(uint8_t addr, uint8_t ctrl, const uint8_t *d, size_t n) {
  bool ok = true;
  for (size_t off = 0; off < n; off += I2C_CHUNK) {
    size_t m = (n - off < I2C_CHUNK) ? (n - off) : I2C_CHUNK;
    Wire.beginTransmission(addr);
    Wire.write(ctrl);
    Wire.write(d + off, m);
    if (Wire.endTransmission() != 0) ok = false;
  }
  return ok;
}
static void oledCmdSeq(const uint8_t *c, size_t n) { i2cWrite(gOledAddr, 0x00, c, n); }

static void clearBuf() { memset(gBuf, 0, sizeof(gBuf)); }
static void fillBuf(uint8_t v) { memset(gBuf, v, sizeof(gBuf)); }
static void setPixel(int x, int y, bool on) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
  uint16_t i = (uint16_t)y / 8 * SCREEN_W + x;
  uint8_t  b = 1 << (y & 7);
  if (on) gBuf[i] |= b; else gBuf[i] &= (uint8_t)~b;
}
static bool getPixel(int x, int y) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return false;
  return (gBuf[(uint16_t)y / 8 * SCREEN_W + x] >> (y & 7)) & 1;
}
static void hline(int x0, int x1, int y) { for (int x = x0; x <= x1; x++) setPixel(x, y, true); }
static void vline(int y0, int y1, int x) { for (int y = y0; y <= y1; y++) setPixel(x, y, true); }
static void fillRect(int x0, int y0, int x1, int y1) { for (int y = y0; y <= y1; y++) hline(x0, x1, y); }
static void drawRect(int x0, int y0, int x1, int y1) {
  hline(x0, x1, y0); hline(x0, x1, y1); vline(y0, y1, x0); vline(y0, y1, x1);
}
/* 粗线：小屏上 1px 斜线会断成虚点，至少要 2px */
static void thickLine(int x0, int y0, int x1, int y1, int w) {
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int n = (dx > dy ? dx : dy) + 1;
  for (int i = 0; i < n; i++) {
    int x = x0 + (x1 - x0) * i / n;
    int y = y0 + (y1 - y0) * i / n;
    fillRect(x, y, x + w - 1, y + w - 1);
  }
}
static void circle(int cx, int cy, int r, int w) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    fillRect(cx + x, cy + y, cx + x + w - 1, cy + y + w - 1);
    fillRect(cx + y, cy + x, cx + y + w - 1, cy + x + w - 1);
    fillRect(cx - y, cy + x, cx - y + w - 1, cy + x + w - 1);
    fillRect(cx - x, cy + y, cx - x + w - 1, cy + y + w - 1);
    fillRect(cx - x, cy - y, cx - x + w - 1, cy - y + w - 1);
    fillRect(cx - y, cy - x, cx - y + w - 1, cy - x + w - 1);
    fillRect(cx + y, cy - x, cx + y + w - 1, cy - x + w - 1);
    fillRect(cx + x, cy - y, cx + x + w - 1, cy - y + w - 1);
    y++;
    if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
  }
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

/* ==================== FNT2 字库 ==================== */
static const wqy14_hdr_t   *gFontHdr = nullptr;
static const wqy14_glyph_t *gTierA   = nullptr;
static const wqy14_blk_t   *gBlkIdx  = nullptr;
static const uint8_t       *gBlkData = nullptr;
static bool                 gFontOk  = false, gFontLegacy = false;
static wqy14_glyph_t        gBlkGlyphs[WQY14_BLOCK_GLYPHS];
static int                  gBlkCur = -1, gBlkCnt = 0;
static tinfl_decompressor  *gTinf = nullptr;

static bool fontLoadBlock(int b) {
  if (b == gBlkCur) return true;
  if (!gFontHdr || b < 0 || b >= gFontHdr->block_count) return false;
  if (!gTinf) { gTinf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor)); if (!gTinf) return false; }
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
  gBlkCnt = (int)cnt; gBlkCur = b; return true;
}
static bool fontMount() {
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "font");
  if (!p) return false;
  const void *ptr = nullptr;
  esp_partition_mmap_handle_t map = 0;
  if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &map) != ESP_OK || !ptr) return false;
  const wqy14_hdr_t *h = (const wqy14_hdr_t *)ptr;
  const uint8_t *base = (const uint8_t *)ptr;
  bool ok = (p->size > sizeof(wqy14_hdr_t)) && !memcmp(h->magic, "FNT2", 4)
            && (h->ver == 3 || h->ver == 2)
            && h->tier_a_count == WQY14_TIER_A && h->total_count == WQY14_COUNT
            && h->block_glyphs == WQY14_BLOCK_GLYPHS
            && (uint32_t)h->tier_a_off + (uint32_t)h->tier_a_count * WQY14_GLYPH_BYTES <= p->size
            && (uint32_t)h->block_idx_off + (uint32_t)h->block_count * sizeof(wqy14_blk_t) <= p->size
            && (uint32_t)h->data_off + h->data_len <= p->size;
  if (!ok) return false;
  gFontHdr = h; gFontLegacy = (h->ver == 2);
  gTierA = (const wqy14_glyph_t *)(base + h->tier_a_off);
  gBlkIdx = (const wqy14_blk_t *)(base + h->block_idx_off);
  gBlkData = base + h->data_off;
  gFontOk = true;
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
    if (cp > upper) { lo = mid + 1; continue; }
    if (!fontLoadBlock(mid)) return nullptr;
    for (int k = 0; k < gBlkCnt; k++) if (gBlkGlyphs[k].rune == cp) return &gBlkGlyphs[k];
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
  while (*s) {
    uint32_t cp = utf8Next(s);
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
  int w = 0;
  while (*s) { const wqy14_glyph_t *g = findGlyph(utf8Next(s)); w += (g ? g->w : 8) * scale; }
  return w;
}
static void drawText(int x, int y, const char *s) { drawTextMode(x, y, s, 1, true); }

/* 大标题：2 倍字，屏幕只放得下 4 个汉字 / 8 个 ASCII */
static void bigText(int y, const char *s) {
  drawTextMode((SCREEN_W - textWidth(s, 2)) / 2, y, s, 2, true);
}
static void smallText(int y, const char *s) {
  drawTextMode((SCREEN_W - textWidth(s, 1)) / 2, y, s, 1, true);
}

/* ==================== 节拍 ==================== */
static float gT = 0;
static float beatPhase() {
  float b = (gT * 1000.0f - BEAT0_MS) / BEAT_MS;
  float f = b - (long)b;
  return f < 0 ? f + 1.0f : f;
}
static float beatPulse() { return 1.0f - beatPhase(); }
static int irand(int n) { return (int)(esp_random() % (uint32_t)(n > 0 ? n : 1)); }

/* ==================== 效果（全部按“小屏可读”重画） ==================== */
enum Fx {
  FX_TERM, FX_LATTICE, FX_RIPPLE, FX_TUNNEL, FX_NET, FX_GLYPH,
  FX_FLIPDOT, FX_SPECTRUM, FX_SHARDS, FX_CORRUPT, FX_PIXELSORT,
  FX_CODE, FX_CHANT, FX_HEART, FX_WORLD, FX_LOGO
};

/* 1 终端：四行等宽字，逐行弹出，块状光标 */
static void fxTerm(float p) {
  static const char *L[] = { "> boot", "> font", "> run", "> me;" };
  int n = (int)(p * 4.0f) + 1;
  if (n > 4) n = 4;
  for (int i = 0; i < n; i++) drawText(4, 2 + i * 15, L[i]);
  int y = 2 + n * 15;
  if (y < 56 && ((int)(gT * 4) & 1) == 0) fillRect(4, y, 9, y + 11);
}

/* 2 晶格：旋转的大方块（2px 边）+ 十字 */
static void fxLattice(float p) {
  int r = 16 + (int)(beatPulse() * 6);
  float a = p * 12.566f;
  int x0 = 64 + (int)(cosf(a) * r), y0 = 32 + (int)(sinf(a) * r);
  int x1 = 64 + (int)(cosf(a + 1.5708f) * r), y1 = 32 + (int)(sinf(a + 1.5708f) * r);
  int x2 = 64 + (int)(cosf(a + 3.1416f) * r), y2 = 32 + (int)(sinf(a + 3.1416f) * r);
  int x3 = 64 + (int)(cosf(a + 4.7124f) * r), y3 = 32 + (int)(sinf(a + 4.7124f) * r);
  thickLine(x0, y0, x1, y1, 2); thickLine(x1, y1, x2, y2, 2);
  thickLine(x2, y2, x3, y3, 2); thickLine(x3, y3, x0, y0, 2);
  fillRect(62, 30, 65, 33);
}

/* 3 涟漪：三道 2px 圆环，随拍扩散 */
static void fxRipple(float p) {
  int r0 = 6 + (int)((1.0f - beatPulse()) * 26);
  for (int k = 0; k < 3; k++) {
    int r = r0 - k * 9;
    if (r > 2) circle(64, 32, r, 2);
  }
  fillRect(62, 30, 65, 33);
  (void)p;
}

/* 4 隧道：四层粗方框由中心向外 */
static void fxTunnel(float p) {
  for (int k = 0; k < 4; k++) {
    int z = ((int)(p * 36) + k * 12) % 48;
    int w = 4 + z, h = 3 + z / 2;
    if (2 * w >= SCREEN_W || 2 * h >= SCREEN_H) continue;
    drawRect(64 - w, 32 - h, 64 + w, 32 + h);
    if (k == 0) drawRect(64 - w + 1, 32 - h + 1, 64 + w - 1, 32 + h - 1);
  }
}

/* 5 网络：8 个节点 + 2px 连线 */
static void fxNet(float p) {
  enum { N = 8 };
  static int px[N], py[N], vx[N], vy[N];
  static bool init = false;
  if (!init) {
    init = true;
    for (int i = 0; i < N; i++) { px[i] = 16 + irand(96); py[i] = 10 + irand(44); vx[i] = (irand(2) ? 1 : -1); vy[i] = (irand(2) ? 1 : -1); }
  }
  for (int i = 0; i < N; i++) {
    px[i] += vx[i]; py[i] += vy[i];
    if (px[i] < 4 || px[i] > SCREEN_W - 5) vx[i] = -vx[i];
    if (py[i] < 4 || py[i] > SCREEN_H - 5) vy[i] = -vy[i];
  }
  for (int i = 0; i < N; i++)
    for (int j = i + 1; j < N; j++) {
      int dx = px[i] - px[j], dy = py[i] - py[j];
      if (dx * dx + dy * dy < 1600) thickLine(px[i], py[i], px[j], py[j], 1);
    }
  for (int i = 0; i < N; i++) fillRect(px[i] - 1, py[i] - 1, px[i] + 1, py[i] + 1);
  (void)p;
}

/* 6 字形：一次只放一个大字，每两拍换一个（排版母题） */
static void fxGlyph(float p) {
  static const char *G[] = { "世", "界", "执", "行", "我", "你", "爱" };
  int i = (int)(gT / (BEAT_MS * 2 / 1000.0f)) % 7;
  bigText(10, G[i]);
  drawRect(6, 6, SCREEN_W - 7, SCREEN_H - 7);
  (void)p;
}

/* 7 翻牌点阵：4x4 粗点按波翻转（最像真实翻牌屏） */
static void fxFlipdot(float p) {
  for (int gy = 0; gy < 8; gy++)
    for (int gx = 0; gx < 16; gx++) {
      float d = sqrtf((float)((gx * 8 - 60) * (gx * 8 - 60) + (gy * 8 - 28) * (gy * 8 - 28)));
      bool on = sinf(d * 0.22f - gT * 6.0f) > 0.0f;
      if (on) fillRect(gx * 8 + 1, gy * 8 + 1, gx * 8 + 5, gy * 8 + 5);
    }
  (void)p;
}

/* 8 频谱：10 根粗柱，随拍起伏 */
static void fxSpectrum() {
  float pulse = beatPulse();
  for (int i = 0; i < 10; i++) {
    int x = 4 + i * 12;
    int h = 6 + (int)((sinf(i * 0.9f + gT * 4.0f) * 0.5f + 0.5f) * 34 * (0.35f + 0.65f * pulse));
    fillRect(x, SCREEN_H - 1 - h, x + 8, SCREEN_H - 1);
  }
}

/* 9 碎屑：8 块大方块向外飞散 */
static void fxShards(float p) {
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785f + p * 1.5f;
    int r = 6 + (int)(p * 40);
    int x = 64 + (int)(cosf(a) * r);
    int y = 32 + (int)(sinf(a) * r * 0.8f);
    int s = 4 + (i % 3) * 2;
    fillRect(x - s / 2, y - s / 2, x + s / 2, y + s / 2);
  }
  fillRect(60, 28, 67, 35);
}

/* 10 损坏：三条粗错位带 + 大字 ERR */
static void fxCorrupt() {
  for (int b = 0; b < 3; b++) {
    int y = 8 + b * 20;
    int dx = irand(13) - 6;
    for (int x = 0; x < SCREEN_W; x++)
      setPixel(x, y + (irand(3) == 0 ? 1 : 0), getPixel((x + dx + SCREEN_W) % SCREEN_W, y));
    fillRect(0, y, SCREEN_W - 1, y + 1);
  }
  if (((int)(gT * 4) & 1) == 0) bigText(22, "ERR");
}

/* 11 像素排序：横向长短条上下错位 */
static void fxPixelsort(float p) {
  for (int y = 0; y < SCREEN_H; y += 4) {
    int len = 20 + (int)((sinf(y * 0.2f + p * 9.0f) * 0.5f + 0.5f) * 100);
    int x0 = (int)(gT * 40 + y * 5) % SCREEN_W;
    for (int k = 0; k < len; k++) setPixel((x0 + k) % SCREEN_W, y, true), setPixel((x0 + k) % SCREEN_W, y + 1, true);
  }
}

/* 12 代码：三行伪代码滚动 + 块光标 */
static void fxCode(float p) {
  static const char *L[] = {
    "for (i=0;i<n;i++)",
    " me.execute(world);",
    "return 0;",
  };
  int off = (int)(gT * 20) % 36;
  for (int i = 0; i < 3; i++) {
    int y = 2 + i * 18 - off % 18;
    if (y < -13 || y > 56) continue;
    drawText(3, y, L[(i + (int)(p)) % 3]);
  }
  if (((int)(gT * 4) & 1) == 0) fillRect(3, 50, 8, 61);
}

/* 13 吟唱：大字随拍切换 + 每拍一帧反白 */
static void fxChant(float p) {
  static const char *W[] = { "01", "//", "00", "me" };
  int i = (int)(gT / (BEAT_MS / 1000.0f)) % 4;
  bool inv = beatPhase() < 0.12f;          /* 每拍开头闪一帧 */
  if (inv) fillBuf(0xFF);
  int w = textWidth(W[i], 2);
  drawTextMode((SCREEN_W - w) / 2, 18, W[i], 2, !inv);
  (void)p;
}

/* 14 心电：粗波形 + 大心形 */
static void fxHeart() {
  for (int x = 0; x < SCREEN_W; x++) {
    int y = 50 + (int)(sinf(x * 0.5f + gT * 6.0f) * 3);
    setPixel(x, y, true);
    setPixel(x, y + 1, true);
  }
  for (int i = 0; i < 60; i++) {
    float t = i * 0.105f;
    float hx = 14 * powf(sinf(t), 3);
    float hy = -(12 * cosf(t) - 4.5f * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t));
    fillRect(64 + (int)hx, 20 + (int)(hy * 0.75f), 65 + (int)hx, 21 + (int)(hy * 0.75f));
  }
}

/* 15 地球：粗圆 + 赤道纬线 + 两条经线 */
static void fxWorld() {
  circle(64, 30, 24, 2);
  hline(41, 87, 30);
  for (int k = -1; k <= 1; k += 2) {
    int y = 30 + k * 12;
    int w = (int)sqrtf(24.0f * 24.0f - (float)((y - 30) * (y - 30)));
    hline(64 - w, 64 + w, y);
  }
  float rot = gT * 1.6f;
  for (int k = 0; k < 2; k++) {
    int x = 64 + (int)(sinf(rot + k * 3.1416f) * 24);
    int h = (int)sqrtf(fmaxf(0, 576 - (x - 64) * (x - 64)));
    vline(30 - h, 30 + h, x);
    vline(30 - h, 30 + h, x + 1);
  }
}

/* 16 收尾：大字 + 短促反白 */
static void fxLogo(float p) {
  bool inv = (p > 0.85f) && ((int)(gT * 6) & 1);
  if (inv) fillBuf(0xFF);
  int w = textWidth("me;", 2);
  drawTextMode((SCREEN_W - w) / 2, 18, "me;", 2, !inv);
  if (!inv) drawRect(4, 4, SCREEN_W - 5, SCREEN_H - 5);
}

/* ==================== 分段（时间取自公开分析数据） ==================== */
typedef struct { float start; uint8_t fx; uint8_t ch; } Section;
static const Section SEC[] = {
  {   0.00f, FX_TERM,      1 },
  {  15.07f, FX_LATTICE,   2 },
  {  29.84f, FX_RIPPLE,    3 },
  {  44.61f, FX_TUNNEL,    4 },
  {  59.38f, FX_NET,       5 },
  {  74.14f, FX_GLYPH,     6 },
  {  88.91f, FX_FLIPDOT,   7 },
  { 103.68f, FX_SPECTRUM,  8 },
  { 118.45f, FX_SHARDS,    9 },
  { 125.84f, FX_CORRUPT,  10 },
  { 133.22f, FX_PIXELSORT,11 },
  { 147.99f, FX_CODE,     12 },
  { 162.76f, FX_CHANT,    13 },
  { 177.53f, FX_HEART,    14 },
  { 192.30f, FX_WORLD,    15 },
  { 207.07f, FX_LOGO,     16 },
};
#define SEC_N (sizeof(SEC) / sizeof(SEC[0]))
#define TOTAL_S 211.9f

static void playFrame(float t) {
  gT = t;
  clearBuf();

  int i = 0;
  while (i + 1 < (int)SEC_N && t >= SEC[i + 1].start) i++;
  float t0 = SEC[i].start;
  float t1 = (i + 1 < (int)SEC_N) ? SEC[i + 1].start : TOTAL_S;
  float p = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;

  switch (SEC[i].fx) {
    case FX_TERM:      fxTerm(p); break;
    case FX_LATTICE:   fxLattice(p); break;
    case FX_RIPPLE:    fxRipple(p); break;
    case FX_TUNNEL:    fxTunnel(p); break;
    case FX_NET:       fxNet(p); break;
    case FX_GLYPH:     fxGlyph(p); break;
    case FX_FLIPDOT:   fxFlipdot(p); break;
    case FX_SPECTRUM:  fxSpectrum(); break;
    case FX_SHARDS:    fxShards(p); break;
    case FX_CORRUPT:   fxCorrupt(); break;
    case FX_PIXELSORT: fxPixelsort(p); break;
    case FX_CODE:      fxCode(p); break;
    case FX_CHANT:     fxChant(p); break;
    case FX_HEART:     fxHeart(); break;
    case FX_WORLD:     fxWorld(); break;
    default:           fxLogo(p); break;
  }

  /* 章节号只在开场 2 秒出现，避免固定位置长亮（烧屏） */
  if (t - t0 < 2.0f) {
    char b[4];
    snprintf(b, sizeof(b), "%02u", (unsigned)SEC[i].ch);
    drawText(2, 2, b);
  }
}

/* ==================== 待机 ==================== */
static void idleFrame(uint32_t ms) {
  clearBuf();
  bigText(8, "me;");
  smallText(36, "world.execute");
  if (((ms / 500) & 1) == 0) smallText(50, "# 播放");
  flush();
}

/* ==================== 按键 ==================== */
static bool gPrevH = false, gPrevS = false;
static bool pressed(uint8_t pin, bool &prev) {
  bool cur = (digitalRead(pin) == LOW);
  bool hit = cur && !prev;
  prev = cur;
  return hit;
}

static bool     gPlaying = false;
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
  gStartMs = millis();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();

  if (pressed(KEY_HASH, gPrevH)) { gPlaying = true; gStartMs = now; }
  if (pressed(KEY_STAR, gPrevS)) { gPlaying = false; }

  if (now - last < FRAME_MS) return;
  last = now;

  if (gPlaying) {
    float t = (now - gStartMs) / 1000.0f;
    if (t >= TOTAL_S) { gStartMs = now; t = 0; }
    playFrame(t);
    flush();
  } else {
    idleFrame(now);
  }
}
