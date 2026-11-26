/*
 * reader.cpp - 纯文本阅读器实现
 * 见 reader.h。书目 + 分文件进度 + 按字节偏移排版。
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

#include "reader.h"

/* ESP32-C3 ROM 里自带 miniz 的 tinfl（esp32c3.rom.ld: tinfl_decompress @0x400000f4），
   用它解 algo=1 的 raw deflate 块，不占 flash。头文件在 esp_rom/include。 */
#include "miniz.h"

extern void appLog(const char *fmt, ...);
extern int  appCpWidth(uint32_t cp);     /* 主固件：字库里的字宽，缺字 8px */

#define RD_DIR       "/books"
#define RD_TOC_MAX   1024   /* 只存偏移（4B/章），名字按需读，大书也够用 */
#define EZB_BLOCK    16384   /* 块上限，实际值以文件头为准 */
#define EZB_HDR_V01  24      /* v0.1 头长：magic|algo|flags|rsv|rawSize|blockSize|blockCount|metaLen */
#define EZB_META_MAX 4096    /* 元数据区上限，超了按头部损坏处理 */
#define RD_HIST_MAX  64
#define RD_BUF_SIZE  1024
#define RD_NVS_NS    "rd"
#define RD_IDX_KEY   "idx"

static File     gFile;
static bool     gOpen   = false;
static char     gName[RD_NAME_MAX];
static uint32_t gSize   = 0;
static uint32_t gStart  = 0;
static uint32_t gEnd    = 0;
static char     gLines[RD_LINES][RD_LINE_BYTES];
static uint8_t  gLineLvl[RD_LINES];     /* 0=正文 1=# 卷 2=## 章 */
static uint32_t gLineOff[RD_LINES];     /* 该行起始的正文偏移 */

static uint8_t  gBuf[RD_BUF_SIZE];
static uint32_t gBufPos = 0;
static int      gBufLen = 0;

/* EZZB 分块压缩容器：algo 0=LZ4，1=raw deflate */
static bool      gEzb = false;
static uint8_t   gAlgo   = 0;
static uint32_t  gBlkSize = EZB_BLOCK;
static uint32_t  gBlkCount = 0;
static uint32_t *gBlkOff = nullptr;
static uint8_t   gBlk[EZB_BLOCK];
static uint32_t  gBlkLen = 0;
static int32_t   gBlkCur = -1;
static uint8_t   gBlkIn[EZB_BLOCK + 256];
static tinfl_decompressor *gTinf = nullptr;   /* 按需 malloc，约 11KB */

/* 目录 */
static uint32_t gTocOff[RD_TOC_MAX];
static char     gTocNameBuf[RD_TOC_NAME];   /* 最近一次 rdTocName() 的结果 */
static int      gTocN = 0;

static uint32_t gHist[RD_HIST_MAX];
static int      gHistN = 0;

/* 书目 */
static char     gBooks[RD_BOOK_MAX][RD_NAME_MAX];
static char     gBookTitle[RD_BOOK_MAX][RD_TITLE_MAX];   /* 元数据书名，空则用文件名 */
static uint32_t gBookSize[RD_BOOK_MAX];
static uint32_t gBookPos[RD_BOOK_MAX];
static uint32_t gBookRaw[RD_BOOK_MAX];   /* 未压缩长度：ezzbk 用头部里的 rawSize，txt 就是文件大小 */
static int      gBookN = 0;

/* ==================== 进度表（name -> offset） ==================== */
static void progLoad(JsonDocument &doc) {
  Preferences p;
  p.begin(RD_NVS_NS, true);
  String s = p.getString(RD_IDX_KEY, "{}");
  p.end();
  if (s.length() == 0 || deserializeJson(doc, s)) doc.clear();
  if (!doc["m"].is<JsonObject>()) doc["m"].to<JsonObject>();
}

static void progSave(JsonDocument &doc) {
  String s;
  serializeJson(doc, s);
  Preferences p;
  p.begin(RD_NVS_NS, false);
  p.putString(RD_IDX_KEY, s);
  p.end();
}

static uint32_t progGet(JsonDocument &doc, const char *name) {
  JsonObject m = doc["m"].as<JsonObject>();
  return (uint32_t)(m[name] | 0u);
}

/* ==================== 带缓冲的按字节读 ==================== */
static void bufReset() { gBufLen = 0; gBufPos = 0; gBlkCur = -1; }

/* LZ4 block 解压（无帧头，与 tools/ezbk2ezb.py 对应） */
static int lz4Decode(const uint8_t *src, int srcLen, uint8_t *dst, int dstCap) {
  int i = 0, o = 0;
  while (i < srcLen) {
    uint8_t token = src[i++];
    int lit = token >> 4;
    if (lit == 15) {
      uint8_t b;
      do {
        if (i >= srcLen) return -1;
        b = src[i++];
        lit += b;
      } while (b == 255);
    }
    if (i + lit > srcLen || o + lit > dstCap) return -1;
    memcpy(dst + o, src + i, lit);
    i += lit;
    o += lit;
    if (i + 2 > srcLen) break;

    int off = src[i] | (src[i + 1] << 8);
    i += 2;
    if (off == 0 || off > o) return -1;

    int ml = token & 0x0F;
    if (ml == 15) {
      uint8_t b;
      do {
        if (i >= srcLen) return -1;
        b = src[i++];
        ml += b;
      } while (b == 255);
    }
    ml += 4;
    if (o + ml > dstCap) return -1;
    int start = o - off;
    for (int k = 0; k < ml; k++) dst[o + k] = dst[start + k];
    o += ml;
  }
  return o;
}

/* raw deflate 块解压，走 ROM 里的 tinfl（miniz）。
   每块独立 flush，因此输出缓冲不必回卷。 */
static int inflateRaw(const uint8_t *src, int srcLen, uint8_t *dst, int dstCap) {
  if (!gTinf) {
    gTinf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    if (!gTinf) { appLog("[阅读] tinfl 状态分配失败\r\n"); return -1; }
  }
  tinfl_init(gTinf);
  size_t inLen = (size_t)srcLen, outLen = (size_t)dstCap;
  tinfl_status st = tinfl_decompress(gTinf, src, &inLen, dst, dst, &outLen,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (st != TINFL_STATUS_DONE) return -1;
  return (int)outLen;
}

static bool loadBlock(uint32_t idx) {
  if (!gEzb) return false;
  if ((int32_t)idx == gBlkCur && gBlkLen > 0) return true;
  if (idx >= gBlkCount) return false;

  uint32_t off = gBlkOff[idx], end = gBlkOff[idx + 1];
  uint32_t len = end - off;
  if (len == 0 || len > sizeof(gBlkIn)) return false;
  if (!gFile.seek(off)) return false;
  if ((uint32_t)gFile.read(gBlkIn, len) != len) return false;

  int n = (gAlgo == 1) ? inflateRaw(gBlkIn, (int)len, gBlk, sizeof(gBlk))
                       : lz4Decode(gBlkIn, (int)len, gBlk, sizeof(gBlk));
  if (n < 0) return false;
  gBlkLen = (uint32_t)n;
  gBlkCur = (int32_t)idx;
  return true;
}

static uint8_t byteAt(uint32_t p) {
  if (gEzb) {
    uint32_t blk = p / gBlkSize;
    if (!loadBlock(blk)) return 0xFF;
    uint32_t in = p % gBlkSize;
    if (in >= gBlkLen) return 0xFF;
    return gBlk[in];
  }
  if (gBufLen <= 0 || p < gBufPos || p >= gBufPos + (uint32_t)gBufLen) {
    if (!gFile.seek(p)) return 0xFF;
    gBufLen = gFile.read(gBuf, sizeof(gBuf));
    gBufPos = p;
    if (gBufLen <= 0) return 0xFF;
  }
  return gBuf[p - gBufPos];
}

static uint32_t cpAt(uint32_t p, uint32_t *next) {
  uint8_t c = byteAt(p);
  if (c == 0xFF) { *next = p; return 0; }
  if (c < 0x80) { *next = p + 1; return c; }

  uint32_t cp; int n;
  if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; n = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
  else                         { *next = p + 1; return '?'; }

  for (int i = 0; i < n; i++) {
    uint8_t cc = byteAt(p + 1 + i);
    if ((cc & 0xC0) != 0x80) { *next = p + 1; return '?'; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  *next = p + 1 + n;
  return cp;
}

static int utf8Put(char *dst, uint32_t cp) {
  if (cp < 0x80) { dst[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = (char)(0xF0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* ==================== 排版 ==================== */
/* 排版：只按像素宽度折行，不做标点避头尾。
 *
 * 关键：宽度不够时「不消费」当前这个字（p 不动）直接换行，
 * 下一页就从它开始——既不重复也不丢字。
 * 旧实现会把 p 回退到上一字的起点，于是行尾那个字在下一页又出现一次，
 * 也就是看到的重字。 */
static uint32_t layout(uint32_t start) {
  uint32_t pos = start;

  for (int li = 0; li < RD_LINES; li++) {
    char *out = gLines[li];
    int n = 0, w = 0;
    uint32_t p = pos;
    int      lineLvl = 0;
    uint32_t lineOff = pos;

    while (p < gSize) {
      uint32_t nx = p;
      uint32_t cp = cpAt(p, &nx);
      if (cp == 0) break;
      if (cp == '\r') { p = nx; continue; }
      if (cp == '\n') { p = nx; break; }

      /* 行首反斜杠是转义：只丢这个字符，后面照常显示 */
      if (cp == '\\' && (p == 0 || byteAt(p - 1) == '\n')) {
        p = nx;
        continue;
      }

      /* 卷/章标题行照常排版：只把行首的 # 去掉，记下级别供高亮用。
       * 以前是整行跳过，正文里看不到标题，翻到章节开头也认不出在哪。 */
      if (cp == '#' && w == 0 && lineLvl == 0 &&
          (p == 0 || byteAt(p - 1) == '\n')) {
        lineOff = p;
        uint32_t hp = p;
        while (hp < gSize) {
          uint32_t nx2;
          uint32_t c2 = cpAt(hp, &nx2);
          if (c2 != '#') break;
          lineLvl++;
          hp = nx2;
        }
        p = hp;
        continue;
      }

      int gw = appCpWidth(cp);
      if (w + gw > RD_WIDTH && w > 0) break;   /* 放不下：本行到此为止，字留在 p */

      if (n + 5 < RD_LINE_BYTES) n += utf8Put(out + n, cp);
      w += gw;
      p = nx;
    }

    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    gLineLvl[li] = (uint8_t)(lineLvl > 2 ? 2 : lineLvl);
    gLineOff[li] = lineOff;

    if (p <= pos) {                       /* 一个字都没排进去，收工 */
      pos = p;
      for (int k = li + 1; k < RD_LINES; k++) { gLines[k][0] = 0; gLineLvl[k] = 0; }
      break;
    }
    pos = p;
    if (pos >= gSize) {
      for (int k = li + 1; k < RD_LINES; k++) { gLines[k][0] = 0; gLineLvl[k] = 0; }
      break;
    }
  }

  gEnd = pos;
  return pos;
}

/* ==================== 目录 ====================
 * ezbk 的 # 卷 / ## 章在正文里是独立行。这里只记偏移（每章 4 字节）：
 * 大书动辄两三千章，把章节名一起驻留 RAM 装不下（旧实现 96 章就截断，
 * 所以"大书目录不全"）。名字在显示时按需从书里读。
 *
 * 扫描分帧进行：开书时先扫前 256KB（马上就有目录），剩下的由
 * rdTocPump() 在阅读过程中按时间片补齐，不会把界面卡住。
 */
static uint32_t gTocPos  = 0;      /* 增量扫描进度（正文偏移） */
static bool     gTocDone = false;

static void tocReset() {
  gTocN    = 0;
  gTocPos  = 0;
  gTocDone = (gSize == 0);
}

/* 扫一段，最多处理 budget 字节正文；返回 true 表示还没扫完。
 * 每次调用都从行首开始、到行首结束，所以可以随时分段。 */
static bool tocPump(uint32_t budget) {
  if (gTocDone) return false;

  uint32_t end = gTocPos + budget;
  if (end < gTocPos || end > gSize) end = gSize;
  uint32_t p = gTocPos;

  while (p < end && p < gSize && gTocN < RD_TOC_MAX) {
    if (byteAt(p) == '#') {
      uint32_t q = p, h = 0;
      while (q < gSize && byteAt(q) == '#') { h++; q++; }
      if (h <= 2) {
        uint32_t r = q;
        while (r < gSize && byteAt(r) == ' ') r++;
        uint8_t c = byteAt(r);
        if (c != '\n' && c != '\r' && c != 0xFF) gTocOff[gTocN++] = p;
      }
      p = q;
    }
    /* 跳到下一行行首：一定要跨过行尾，保证下次仍从行首继续 */
    while (p < gSize) {
      uint32_t nx = p;
      uint32_t c = cpAt(p, &nx);
      if (c == 0) { p = gSize; break; }
      p = nx;
      if (c == '\n') break;
    }
  }

  gTocPos = p;
  if (p >= gSize || gTocN >= RD_TOC_MAX) gTocDone = true;
  return !gTocDone;
}

/* 给主循环调：按毫秒预算继续扫目录，扫完就什么都不做 */
void rdTocPump(uint32_t budgetMs) {
  if (gTocDone || !gOpen) return;
  uint32_t t0 = millis();
  while (!gTocDone && (uint32_t)(millis() - t0) < budgetMs) {
    if (!tocPump(16 * 1024)) break;
  }
}

bool rdTocReady() { return gTocDone; }

/* ==================== 书目 ==================== */
/* 只认 .ezzbk：纯文本/二进制没有容器头，正文里出现 0x00、超大单行都会让
 * 排版和分块读取出问题，干脆收口到唯一格式。转换交给 neobook。 */
static bool isBook(const char *n) {
  size_t l = strlen(n);
  return (l > 6 && !strcasecmp(n + l - 6, ".ezzbk"));
}

/* .ezzbk 头里的原始长度；不是 EZZB 就返回文件大小 */
static inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ezzbk 头部字段 */
typedef struct {
  uint8_t  algo;
  uint32_t raw;      /* 解压后的正文长度 */
  uint32_t bs;       /* 块大小 */
  uint32_t cnt;      /* 块数 */
  uint32_t offBase;  /* 偏移表起始位置 = 头长 + metaLen */
  uint32_t metaLen;  /* 元数据区长度，旧头为 0 */
} EzbHdr;

static bool rdU32At(File &f, uint32_t pos, uint32_t *v) {
  uint8_t b[4];
  if (!f.seek(pos) || f.read(b, 4) != 4) return false;
  *v = rd32(b);
  return true;
}

/* 解析 ezzbk 头。
 *
 * v0.1（当前规范）：
 *   "EZZB"|u8 algo|u8 flags|u16 rsv|u32 rawSize|u32 blockSize|u32 blockCount
 *   |u32 metaLen | meta[metaLen] | u32 off[blockCount+1]
 * 旧 20 字节头（无 meta 区）与更旧 16 字节头（无 algo/flags）也认。
 * 三种布局字段位置互相重叠，只能靠 off[0]=偏移表末尾、off[cnt]=文件长度 对账
 * 区分——rawSize 的低字节可能恰好落在 algo 位上，只看字段不可靠。 */
static bool ezbHeader(File &f, EzbHdr *hd) {
  hd->algo = 0; hd->raw = 0; hd->bs = EZB_BLOCK; hd->cnt = 0;
  hd->offBase = 0; hd->metaLen = 0;

  uint32_t fsz = (uint32_t)f.size();
  uint8_t h[24] = {0};
  if (!f.seek(0)) return false;
  int hn = f.read(h, 24);
  if (hn < 16 || memcmp(h, "EZZB", 4)) return false;

  for (int pass = 0; pass < 3; pass++) {
    uint32_t algo, raw, bs, cnt, base = 0, metaLen = 0;
    if (pass == 0) {                        /* v0.1：带 meta 区 */
      if (hn < 24) continue;
      algo    = h[4];
      raw     = rd32(h + 8);
      bs      = rd32(h + 12);
      cnt     = rd32(h + 16);
      metaLen = rd32(h + 20);
      if (metaLen > EZB_META_MAX) continue;
      base = EZB_HDR_V01 + metaLen;
    } else if (pass == 1) {                 /* 20 字节头，无 meta */
      algo = h[4];
      raw  = rd32(h + 8);
      bs   = rd32(h + 12);
      cnt  = rd32(h + 16);
      base = 20;
    } else {                                /* 16 字节头，只有 LZ4 */
      algo = 0;
      raw  = rd32(h + 4);
      bs   = rd32(h + 8);
      cnt  = rd32(h + 12);
      base = 16;
    }

    if (algo > 1 || raw == 0 || bs == 0 || bs > EZB_BLOCK) continue;
    if (cnt == 0 || cnt > 4096) continue;
    if (base + 4 * (cnt + 1) > fsz) continue;

    uint32_t o0, on;
    if (!rdU32At(f, base, &o0) || !rdU32At(f, base + 4 * cnt, &on)) continue;
    if (o0 != base + 4 * (cnt + 1) || on != fsz || on <= o0) continue;

    hd->algo    = (uint8_t)algo;
    hd->raw     = raw;
    hd->bs      = bs;
    hd->cnt     = cnt;
    hd->offBase = base;
    hd->metaLen = metaLen;
    return true;
  }
  return false;
}

/* UTF-8 截断落在多字节字符中间时，回退到上一个字符边界 */
static void utf8TrimTail(char *s) {
  size_t n = strlen(s), i = n;
  while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) i--;   /* 跳过续字节 */
  if (i == 0) { s[0] = 0; return; }
  i--;                                                       /* i 指向该字符首字节 */
  uint8_t c = (uint8_t)s[i];
  size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
                (c & 0xF8) == 0xF0 ? 4 : 1;
  if (i + need > n) s[i] = 0;
}

/* 取 meta 区里某个键的值。键值按第一个 '=' 分割，值里可以有 '='。
 * 值内的 \n \r 还原成空格（单行显示），\\ 还原成反斜杠。找不到留空串。 */
static void metaGet(File &f, const EzbHdr &hd, const char *key, char *val, size_t vn) {
  if (vn == 0) return;
  val[0] = 0;
  if (!hd.metaLen || hd.metaLen > EZB_META_MAX) return;

  /* 元数据区很小（几十到几百字节），临时借一次堆，不占常驻 RAM */
  uint8_t *buf = (uint8_t *)malloc(hd.metaLen);
  if (!buf) return;
  if (!f.seek(EZB_HDR_V01) || f.read(buf, hd.metaLen) != hd.metaLen) { free(buf); return; }

  size_t kl = strlen(key);
  uint32_t i = 0;
  while (i < hd.metaLen) {
    uint32_t j = i;
    while (j < hd.metaLen && buf[j] != '\n') j++;
    if (j > i + kl && !memcmp(buf + i, key, kl) && buf[i + kl] == '=') {
      size_t o = 0;
      for (uint32_t k = i + kl + 1; k < j && o + 1 < vn; k++) {
        char c = (char)buf[k];
        if (c == '\\' && k + 1 < j) {
          c = (char)buf[++k];
          if (c == 'n' || c == 'r') c = ' ';
        }
        val[o++] = c;
      }
      val[o] = 0;
      while (o > 0 && val[o - 1] == ' ') { o--; val[o] = 0; }
      utf8TrimTail(val);
      break;
    }
    i = j + 1;
  }
  free(buf);
}

/* 读一本书的元数据：未压缩长度 + 书名（meta 里的 title） */
static void bookMeta(File &f, uint32_t *raw, char *title, size_t tn) {
  uint32_t fsz = (uint32_t)f.size();
  uint32_t save = f.position();
  title[0] = 0;
  *raw = fsz;

  EzbHdr hd;
  if (ezbHeader(f, &hd)) {
    *raw = hd.raw;
    metaGet(f, hd, "title", title, tn);
  }
  f.seek(save);
}

void rdScan() {
  gBookN = 0;

  JsonDocument doc;
  progLoad(doc);

  File dir = LittleFS.open(RD_DIR);
  if (dir) {
    File f = dir.openNextFile();
    while (f && gBookN < RD_BOOK_MAX) {
      if (!f.isDirectory()) {
        const char *nm = f.name();
        const char *slash = strrchr(nm, '/');
        const char *base = slash ? slash + 1 : nm;
        if (isBook(base)) {
          snprintf(gBooks[gBookN], RD_NAME_MAX, "%s", base);
          gBookSize[gBookN] = (uint32_t)f.size();
          bookMeta(f, &gBookRaw[gBookN], gBookTitle[gBookN], RD_TITLE_MAX);
          if (gBookRaw[gBookN] == 0) gBookRaw[gBookN] = gBookSize[gBookN];
          gBookN++;
        }
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }

  /* 按名字排序，列表位置才稳定 */
  for (int i = 1; i < gBookN; i++) {
    char tn[RD_NAME_MAX];
    char tt[RD_TITLE_MAX];
    uint32_t ts, tr;
    snprintf(tn, sizeof(tn), "%s", gBooks[i]);
    ts = gBookSize[i];
    tr = gBookRaw[i];
    snprintf(tt, sizeof(tt), "%s", gBookTitle[i]);
    int j = i - 1;
    while (j >= 0 && strcmp(gBooks[j], tn) > 0) {
      snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", gBooks[j]);
      gBookSize[j + 1] = gBookSize[j];
      gBookRaw[j + 1]  = gBookRaw[j];
      snprintf(gBookTitle[j + 1], RD_TITLE_MAX, "%s", gBookTitle[j]);
      j--;
    }
    snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", tn);
    gBookSize[j + 1] = ts;
    gBookRaw[j + 1]  = tr;
    snprintf(gBookTitle[j + 1], RD_TITLE_MAX, "%s", tt);
  }

  /* 进度：只保留还在的书，孤儿进度直接删 */
  bool changed = false;
  JsonObject m = doc["m"].as<JsonObject>();
  for (int i = 0; i < gBookN; i++) {
    uint32_t pos = (uint32_t)(m[gBooks[i]] | 0u);
    if (pos >= gBookRaw[i]) pos = 0;     /* 用未压缩长度判断，ezzbk 才不会误清 */
    gBookPos[i] = pos;
  }
  {
    /* 反向剪枝：进度表里有、书目里没有的键删掉 */
    JsonArray deadKeys;
    JsonDocument tmp;
    JsonArray dead = tmp.to<JsonArray>();
    for (JsonPair kv : m) {
      bool found = false;
      for (int i = 0; i < gBookN; i++)
        if (!strcmp(kv.key().c_str(), gBooks[i])) { found = true; break; }
      if (!found) { dead.add((const char *)kv.key().c_str()); }
    }
    (void)deadKeys;
    for (JsonVariant v : dead) {
      m.remove(v.as<const char *>());
      changed = true;
    }
  }
  if (changed) progSave(doc);

  appLog("[阅读] 书目 %d 本\r\n", gBookN);
  for (int i = 0; i < gBookN; i++)
    appLog("[阅读]   %d) %s  %u B  %d%%\r\n", i, gBooks[i],
           (unsigned)gBookSize[i], rdBookPercent(i));
}

int rdBookCount() { return gBookN; }

const char *rdBookTitle(int i) {
  if (i < 0 || i >= gBookN) return "";
  return gBookTitle[i][0] ? gBookTitle[i] : gBooks[i];
}

const char *rdBookName(int i) {
  return (i >= 0 && i < gBookN) ? gBooks[i] : "";
}

/* 蓝牙书目要显示书名，但不一定先扫过书目，所以这里直接开文件读 meta */
void rdBookMetaTitle(const char *name, char *title, size_t tn) {
  if (!title || tn == 0) return;
  title[0] = 0;
  if (!name || !*name) return;

  char path[64];
  snprintf(path, sizeof(path), RD_DIR "/%s", name);
  File f = LittleFS.open(path, "r");
  if (!f) return;
  uint32_t raw = 0;
  bookMeta(f, &raw, title, tn);
  f.close();
}

int rdBookPercent(int i) {
  if (i < 0 || i >= gBookN || gBookRaw[i] == 0) return 0;
  uint64_t pct = (uint64_t)gBookPos[i] * 100 / gBookRaw[i];
  return pct > 100 ? 100 : (int)pct;
}

/* ==================== 打开 / 关闭 ==================== */
static void closeCurrent() {
  if (gOpen) { gFile.close(); }
  if (gBlkOff) { free(gBlkOff); gBlkOff = nullptr; }
  gEzb = false;
  gAlgo = 0;
  gBlkCount = 0;
  gBlkCur = -1;
  gTocN = 0;
  gTocPos = 0;
  gTocDone = true;
  gOpen = false;
  gName[0] = 0;
  gSize = 0;
  gStart = gEnd = 0;
  gHistN = 0;
  gLines[0][0] = 0;
  for (int i = 0; i < RD_LINES; i++) { gLineLvl[i] = 0; gLineOff[i] = 0; }
}

bool rdOpenIndex(int i) {
  if (i < 0 || i >= gBookN) return false;
  closeCurrent();

  char path[64];
  snprintf(path, sizeof(path), RD_DIR "/%s", gBooks[i]);
  gFile = LittleFS.open(path, "r");
  if (!gFile) {
    appLog("[阅读] 打不开 %s\r\n", path);
    return false;
  }

  snprintf(gName, sizeof(gName), "%s", gBooks[i]);

  /* 判断是不是 .ezb 压缩容器 */
  if (gBlkOff) { free(gBlkOff); gBlkOff = nullptr; }
  gEzb = false;
  gAlgo = 0;
  gBlkCount = 0;
  gBlkSize = EZB_BLOCK;
  gSize = (uint32_t)gFile.size();

  uint8_t magic[4] = {0};
  gFile.seek(0);
  if (gFile.read(magic, 4) == 4 && !memcmp(magic, "EZZB", 4)) {
    /* 头部三种布局统一走 ezbHeader()，旧 20/16 字节头的书照样能打开 */
    EzbHdr hd;
    if (!ezbHeader(gFile, &hd)) {
      appLog("[阅读] %s 是 EZZB 但头部损坏\r\n", gName);
      gFile.close();
      return false;
    }

    const uint32_t fsz = (uint32_t)gFile.size();
    size_t want = (size_t)(hd.cnt + 1) * sizeof(uint32_t);
    uint32_t *off = (uint32_t *)malloc(want);
    if (!off) {
      appLog("[阅读] 偏移表内存不够（%u 块）\r\n", (unsigned)hd.cnt);
      gFile.close();
      return false;
    }
    gFile.seek(hd.offBase);
    if (gFile.read((uint8_t *)off, want) != want) {
      free(off);
      appLog("[阅读] %s 偏移表读不出来\r\n", gName);
      gFile.close();
      return false;
    }

    /* 偏移必须严格递增、落在文件内：块数据坏在末尾时 file.read 会短读，别硬解 */
    bool good = true;
    for (uint32_t k = 0; k <= hd.cnt; k++) {
      if (off[k] > fsz || (k > 0 && off[k] <= off[k - 1])) { good = false; break; }
    }
    if (!good || off[0] != hd.offBase + 4 * (hd.cnt + 1) || off[hd.cnt] != fsz) {
      free(off);
      appLog("[阅读] %s 的 EZZB 偏移表越界\r\n", gName);
      gFile.close();
      return false;
    }

    gBlkOff   = off;
    gAlgo     = hd.algo;
    gBlkSize  = hd.bs;
    gBlkCount = hd.cnt;
    gSize     = hd.raw;
    gEzb      = true;

    char bt[RD_TITLE_MAX];
    metaGet(gFile, hd, "title", bt, sizeof(bt));
    appLog("[阅读] EZZB algo=%s 块=%u 块数=%u 元数据=%u%s%s\r\n",
           gAlgo == 1 ? "deflate" : "lz4", (unsigned)gBlkSize, (unsigned)gBlkCount,
           (unsigned)hd.metaLen, bt[0] ? " 书名=" : "", bt);
  }

  gOpen = true;
  bufReset();
  tocReset();
  tocPump(256 * 1024);        /* 先扫前 256KB，目录页立刻有内容 */

  gStart = gBookPos[i];
  if (gStart >= gSize) gStart = 0;
  layout(gStart);
  appLog("[阅读] 打开 %s  %u 字节  从 %u 继续  %d%%\r\n",
         gName, (unsigned)gSize, (unsigned)gStart, rdPercent());
  return true;
}

void rdInit() {
  if (!LittleFS.exists(RD_DIR)) LittleFS.mkdir(RD_DIR);
  rdScan();
}

void rdForget(const char *name) {
  if (!name || !*name) return;
  JsonDocument doc;
  progLoad(doc);
  JsonObject m = doc["m"].as<JsonObject>();
  if (m[name].is<JsonVariant>()) {
    m.remove(name);
    progSave(doc);
    appLog("[阅读] 已清除 %s 的进度\r\n", name);
  }
  rdScan();
}

bool rdLoaded()      { return gOpen; }
const char *rdName() { return gName; }
uint32_t rdSize()    { return gSize; }
int rdPercent()      { return gSize ? (int)((uint64_t)gStart * 100 / gSize) : 0; }
const char *rdLine(int i) { return (i >= 0 && i < RD_LINES) ? gLines[i] : ""; }

uint8_t rdLineLevel(int i)  { return (i >= 0 && i < RD_LINES) ? gLineLvl[i] : 0; }
uint32_t rdLineOffset(int i){ return (i >= 0 && i < RD_LINES) ? gLineOff[i] : 0; }

/* 当前所在的章节：最后一个起始位置 <= gStart 的目录项 */
int rdCurrentTocIndex() {
  int best = 0;
  for (int i = 0; i < gTocN; i++)
    if (gTocOff[i] <= gStart) best = i;
  return best;
}

void rdSaveProgress() {
  if (!gOpen) return;
  JsonDocument doc;
  progLoad(doc);
  doc["m"][gName] = gStart;
  progSave(doc);

  for (int i = 0; i < gBookN; i++)
    if (!strcmp(gBooks[i], gName)) { gBookPos[i] = gStart; break; }
}

int rdTocCount() { return gTocN; }

/* 章节名按需从书里读（共用一节静态缓冲，调用方拿到就立刻用） */
const char *rdTocName(int i) {
  gTocNameBuf[0] = 0;
  if (i < 0 || i >= gTocN) return gTocNameBuf;

  uint32_t p = gTocOff[i];
  while (p < gSize && byteAt(p) == '#') p++;
  while (p < gSize && byteAt(p) == ' ') p++;

  int o = 0;
  while (p < gSize && o < RD_TOC_NAME - 1) {
    uint8_t c = byteAt(p);
    if (c == '\n' || c == 0xFF) break;
    if (c != '\r') gTocNameBuf[o++] = (char)c;
    p++;
  }
  gTocNameBuf[o] = 0;
  while (o > 0 && gTocNameBuf[o - 1] == ' ') gTocNameBuf[--o] = 0;
  utf8TrimTail(gTocNameBuf);          /* 截断别落在半个汉字上 */
  return gTocNameBuf;
}

bool rdGotoToc(int i) {
  if (!gOpen || i < 0 || i >= gTocN) return false;
  gHistN = 0;
  gStart = gTocOff[i];
  if (gStart >= gSize) gStart = 0;
  layout(gStart);
  rdSaveProgress();
  return true;
}

/* 历史是环形的前进记录：满了就把最老的一条挤掉。
 * 旧写法在装满 64 条后就不再记录，于是读到第 100 页按上一页会跳回第 64 页。 */
static void histPush(uint32_t off) {
  if (gHistN == RD_HIST_MAX) {
    memmove(gHist, gHist + 1, sizeof(gHist[0]) * (RD_HIST_MAX - 1));
    gHistN--;
  }
  gHist[gHistN++] = off;
}

void rdNext() {
  if (!gOpen || gEnd >= gSize) return;
  histPush(gStart);
  gStart = gEnd;
  layout(gStart);
  rdSaveProgress();
}

void rdPrev() {
  if (!gOpen) return;

  if (gHistN > 0) {
    gStart = gHist[--gHistN];
  } else {
    /* 刚打开书时没有历史：从头翻到上一页（不往历史里塞东西） */
    if (gStart == 0) return;
    uint32_t target = gStart, prev = 0, cur = 0;
    while (cur < target && cur < gSize) {
      prev = cur;
      gStart = cur;
      uint32_t nx = layout(cur);
      if (nx <= cur) break;              /* 防御：不前进就停 */
      cur = nx;
    }
    gStart = prev;
  }
  layout(gStart);
  rdSaveProgress();
}
