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
static void bufReset() { gBufLen = 0; gBufPos = 0; }

static uint8_t byteAt(uint32_t p) {
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

/* ==================== 书目 ==================== */
static bool isTxt(const char *n) {
  size_t l = strlen(n);
  return l > 4 && !strcasecmp(n + l - 4, ".txt");
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
        if (isTxt(base)) {
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
  gSize = (uint32_t)gFile.size();
  gOpen = true;
  bufReset();

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
