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

extern void appLog(const char *fmt, ...);
extern int  appCpWidth(uint32_t cp);     /* 主固件：字库里的字宽，缺字 8px */

#define RD_DIR       "/books"
#define RD_TOC_MAX   96
#define RD_TOC_NAME  40
#define EZB_BLOCK    4096
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

static uint8_t  gBuf[RD_BUF_SIZE];
static uint32_t gBufPos = 0;
static int      gBufLen = 0;

/* .ezb：LZ4 分块压缩容器 */
static bool      gEzb = false;
static uint32_t  gBlkSize = EZB_BLOCK;
static uint32_t  gBlkCount = 0;
static uint32_t *gBlkOff = nullptr;
static uint8_t   gBlk[EZB_BLOCK];
static uint32_t  gBlkLen = 0;
static int32_t   gBlkCur = -1;
static uint8_t   gBlkIn[EZB_BLOCK + 256];

/* 目录 */
static uint32_t gTocOff[RD_TOC_MAX];
static char     gTocName[RD_TOC_MAX][RD_TOC_NAME];
static int      gTocN = 0;

static uint32_t gHist[RD_HIST_MAX];
static int      gHistN = 0;

/* 书目 */
static char     gBooks[RD_BOOK_MAX][RD_NAME_MAX];
static uint32_t gBookSize[RD_BOOK_MAX];
static uint32_t gBookPos[RD_BOOK_MAX];
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

static bool loadBlock(uint32_t idx) {
  if (!gEzb) return false;
  if ((int32_t)idx == gBlkCur && gBlkLen > 0) return true;
  if (idx >= gBlkCount) return false;

  uint32_t off = gBlkOff[idx], end = gBlkOff[idx + 1];
  uint32_t len = end - off;
  if (len == 0 || len > sizeof(gBlkIn)) return false;
  if (!gFile.seek(off)) return false;
  if ((uint32_t)gFile.read(gBlkIn, len) != len) return false;

  int n = lz4Decode(gBlkIn, (int)len, gBlk, sizeof(gBlk));
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

static bool noStart(uint32_t c) {
  switch (c) {
    case 0xFF0C: case 0x3002: case 0xFF01: case 0xFF1F: case 0x3001:
    case 0xFF1B: case 0xFF1A: case 0x300D: case 0x300F: case 0x3011:
    case 0xFF09: case 0x300B: case 0x3015: case 0x2026: case 0xFF5D:
      return true;
  }
  return false;
}
static bool noEnd(uint32_t c) {
  switch (c) {
    case 0x300C: case 0x300E: case 0x3010: case 0xFF08: case 0x300A:
    case 0x3014: case 0xFF5B:
      return true;
  }
  return false;
}

/* ==================== 排版 ==================== */
static uint32_t layout(uint32_t start) {
  uint32_t pos = start;

  for (int li = 0; li < RD_LINES; li++) {
    char *out = gLines[li];
    int n = 0, w = 0;
    uint32_t lastBreak = 0;
    bool hasBreak = false;
    uint32_t p = pos;

    while (p < gSize) {
      uint32_t nx = p;
      uint32_t cp = cpAt(p, &nx);
      if (cp == 0) break;
      if (cp == '\r') { p = nx; continue; }
      if (cp == '\n') { p = nx; break; }

      /* 章节标记行不参与排版（只在目录里出现） */
      if (cp == '#' && w == 0 && (p == 0 || byteAt(p - 1) == '\n')) {
        while (p < gSize) {
          uint32_t nx2;
          uint32_t c2 = cpAt(p, &nx2);
          p = nx2;
          if (c2 == '\n' || c2 == 0) break;
        }
        continue;
      }

      int gw = appCpWidth(cp);
      if (w + gw > RD_WIDTH && w > 0) {
        if (hasBreak && lastBreak > pos) p = lastBreak;
        break;
      }

      if (cp == ' ')                              { lastBreak = nx; hasBreak = true; }
      else if (!noStart(cp) && !noEnd(cp))        { lastBreak = p;  hasBreak = true; }

      if (n + 5 < RD_LINE_BYTES) n += utf8Put(out + n, cp);
      w += gw;
      p = nx;
    }

    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;

    if (p <= pos) {
      pos = p;
      for (int k = li + 1; k < RD_LINES; k++) gLines[k][0] = 0;
      break;
    }
    pos = p;
    if (pos >= gSize) {
      for (int k = li + 1; k < RD_LINES; k++) gLines[k][0] = 0;
      break;
    }
  }

  gEnd = pos;
  return pos;
}

/* ==================== 目录 ====================
 * ezbk 的 # 卷 / ## 章在正文里是独立行，扫一遍记下偏移与标题。
 * 96 条以内足够日常阅读；超出的书只看前 96 章。
 */
static void buildToc() {
  gTocN = 0;
  uint32_t p = 0;
  while (p < gSize && gTocN < RD_TOC_MAX) {
    if (byteAt(p) == '#') {
      uint32_t q = p, h = 0;
      while (q < gSize && byteAt(q) == '#') { h++; q++; }
      if (h <= 2) {
        while (q < gSize && byteAt(q) == ' ') q++;
        int o = 0;
        while (q < gSize && o < RD_TOC_NAME - 1) {
          uint8_t c = byteAt(q);
          if (c == '\n' || c == 0xFF) break;
          if (c != '\r') gTocName[gTocN][o++] = (char)c;
          q++;
        }
        gTocName[gTocN][o] = 0;
        if (o > 0) {
          gTocOff[gTocN] = p;
          gTocN++;
        }
      }
    }
    /* 跳到下一行行首 */
    while (p < gSize) {
      uint32_t nx = p;
      uint32_t c = cpAt(p, &nx);
      if (c == 0) break;
      p = nx;
      if (c == '\n') break;
    }
  }
}

/* ==================== 书目 ==================== */
static bool isBook(const char *n) {
  size_t l = strlen(n);
  if (l < 4) return false;
  if (!strcasecmp(n + l - 4, ".txt")) return true;
  if (l > 6 && !strcasecmp(n + l - 6, ".ezzbk")) return true;
  return false;
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
    uint32_t ts;
    snprintf(tn, sizeof(tn), "%s", gBooks[i]);
    ts = gBookSize[i];
    int j = i - 1;
    while (j >= 0 && strcmp(gBooks[j], tn) > 0) {
      snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", gBooks[j]);
      gBookSize[j + 1] = gBookSize[j];
      j--;
    }
    snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", tn);
    gBookSize[j + 1] = ts;
  }

  /* 进度：只保留还在的书，孤儿进度直接删 */
  bool changed = false;
  JsonObject m = doc["m"].as<JsonObject>();
  for (int i = 0; i < gBookN; i++) {
    uint32_t pos = (uint32_t)(m[gBooks[i]] | 0u);
    if (pos >= gBookSize[i]) pos = 0;
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
           (unsigned)gBookSize[i],
           gBookSize[i] ? (int)((uint64_t)gBookPos[i] * 100 / gBookSize[i]) : 0);
}

int rdBookCount() { return gBookN; }

const char *rdBookName(int i) {
  return (i >= 0 && i < gBookN) ? gBooks[i] : "";
}

int rdBookPercent(int i) {
  if (i < 0 || i >= gBookN || gBookSize[i] == 0) return 0;
  return (int)((uint64_t)gBookPos[i] * 100 / gBookSize[i]);
}

/* ==================== 打开 / 关闭 ==================== */
static void closeCurrent() {
  if (gOpen) { gFile.close(); }
  if (gBlkOff) { free(gBlkOff); gBlkOff = nullptr; }
  gEzb = false;
  gBlkCount = 0;
  gBlkCur = -1;
  gTocN = 0;
  gOpen = false;
  gName[0] = 0;
  gSize = 0;
  gStart = gEnd = 0;
  gHistN = 0;
  gLines[0][0] = 0;
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
  gBlkCount = 0;
  gBlkSize = EZB_BLOCK;
  gSize = (uint32_t)gFile.size();

  uint8_t magic[4] = {0};
  gFile.seek(0);
  if (gFile.read(magic, 4) == 4 && !memcmp(magic, "EZZB", 4)) {
    uint32_t hdr[3] = {0, 0, 0};
    gFile.seek(4);
    if (gFile.read((uint8_t *)hdr, 12) == 12) {
      uint32_t raw = hdr[0], bs = hdr[1], cnt = hdr[2];
      if (bs > 0 && bs <= EZB_BLOCK && cnt > 0 && cnt <= 4096) {
        gBlkOff = (uint32_t *)malloc((cnt + 1) * sizeof(uint32_t));
        if (gBlkOff) {
          gFile.seek(16);
          if (gFile.read((uint8_t *)gBlkOff, (cnt + 1) * sizeof(uint32_t)) ==
              (int)((cnt + 1) * sizeof(uint32_t))) {
            gEzb = true;
            gBlkSize = bs;
            gBlkCount = cnt;
            gSize = raw;
          } else {
            free(gBlkOff);
            gBlkOff = nullptr;
          }
        }
      }
    }
    if (!gEzb) {
      appLog("[阅读] %s 是 EZZB 但头部损坏\r\n", gName);
      gFile.close();
      return false;
    }
  }

  gOpen = true;
  bufReset();
  buildToc();

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

const char *rdTocName(int i) {
  return (i >= 0 && i < gTocN) ? gTocName[i] : "";
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

void rdNext() {
  if (!gOpen || gEnd >= gSize) return;
  if (gHistN < RD_HIST_MAX) gHist[gHistN++] = gStart;
  gStart = gEnd;
  layout(gStart);
  rdSaveProgress();
}

void rdPrev() {
  if (!gOpen) return;

  if (gHistN == 0) {
    if (gStart == 0) return;
    uint32_t target = gStart;
    gHistN = 0;
    gStart = 0;
    layout(0);
    while (gEnd < target && gEnd < gSize) {
      if (gHistN < RD_HIST_MAX) gHist[gHistN++] = gStart;
      gStart = gEnd;
      layout(gStart);
    }
    if (gHistN == 0) return;
  }

  gHistN--;
  gStart = gHist[gHistN];
  layout(gStart);
  rdSaveProgress();
}
