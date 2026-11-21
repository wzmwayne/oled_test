/*
 * reader.cpp - 纯文本阅读器实现
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

#include "reader.h"

extern void appLog(const char *fmt, ...);
extern int  appCpWidth(uint32_t cp);     /* 主固件：字库里的字宽，缺字 8px */

#define RD_DIR        "/books"
#define RD_HIST_MAX   64
#define RD_BUF_SIZE   1024

static File     gFile;
static bool     gOpen   = false;
static char     gName[40];
static uint32_t gSize   = 0;
static uint32_t gStart  = 0;      /* 当前页起始偏移 */
static uint32_t gEnd    = 0;      /* 当前页结束偏移（下一页起点） */
static char     gLines[RD_LINES][RD_LINE_BYTES];

static uint8_t  gBuf[RD_BUF_SIZE];
static uint32_t gBufPos = 0;
static int      gBufLen = 0;

static uint32_t gHist[RD_HIST_MAX];
static int      gHistN = 0;

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

/* 避头：不能出现在行首的标点 */
static bool noStart(uint32_t c) {
  switch (c) {
    case 0xFF0C: case 0x3002: case 0xFF01: case 0xFF1F: case 0x3001:
    case 0xFF1B: case 0xFF1A: case 0x300D: case 0x300F: case 0x3011:
    case 0xFF09: case 0x300B: case 0x3015: case 0x2026: case 0xFF5D:
      return true;
  }
  return false;
}
/* 避尾：不能留在一行末尾的标点 */
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
        /* 放不下：优先退到上一个可断点（按词/按字），否则硬断 */
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

    if (p <= pos) {                       /* 没进展，防止死循环 */
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

/* ==================== 进度存取 ==================== */
static void saveProgress() {
  if (!gOpen) return;
  Preferences p;
  p.begin("rd", false);
  p.putString("name", gName);
  p.putUInt("pos", gStart);
  p.end();
}

static uint32_t loadProgress(const char *name) {
  Preferences p;
  p.begin("rd", true);
  String n = p.getString("name", "");
  uint32_t pos = p.getUInt("pos", 0);
  p.end();
  if (n != name) return 0;
  return pos;
}

/* ==================== 对外接口 ==================== */
void rdInit() {
  if (!LittleFS.exists(RD_DIR)) LittleFS.mkdir(RD_DIR);
}

bool rdOpen(const char *name) {
  if (!name || !*name) return false;
  rdClose();

  char path[64];
  snprintf(path, sizeof(path), RD_DIR "/%s", name);
  gFile = LittleFS.open(path, "r");
  if (!gFile) {
    appLog("[阅读] 打不开 %s\r\n", path);
    return false;
  }

  snprintf(gName, sizeof(gName), "%s", name);
  gSize = gFile.size();
  gOpen = true;
  gHistN = 0;
  bufReset();

  uint32_t saved = loadProgress(gName);
  gStart = (saved < gSize) ? saved : 0;
  layout(gStart);
  appLog("[阅读] %s  %u 字节  从 %u 继续  %d%%\r\n",
         gName, (unsigned)gSize, (unsigned)gStart, rdPercent());
  return true;
}

bool rdOpenLast() {
  Preferences p;
  p.begin("rd", true);
  String n = p.getString("name", "");
  p.end();
  if (n.length() && rdOpen(n.c_str())) return true;

  /* 没有记录就打开目录里第一个 txt */
  File dir = LittleFS.open(RD_DIR);
  if (!dir) return false;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char *nm = f.name();
      const char *slash = strrchr(nm, '/');
      bool ok = rdOpen(slash ? slash + 1 : nm);
      f.close();
      dir.close();
      return ok;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return false;
}

void rdClose() {
  if (gOpen) { saveProgress(); gFile.close(); }
  gOpen = false;
  gName[0] = 0;
  gSize = 0;
  gStart = gEnd = 0;
  gHistN = 0;
  gLines[0][0] = 0;
}

bool rdLoaded()      { return gOpen; }
const char *rdName() { return gName; }
uint32_t rdSize()    { return gSize; }
uint32_t rdPos()     { return gStart; }
int rdPercent()      { return gSize ? (int)((uint64_t)gStart * 100 / gSize) : 0; }
const char *rdLine(int i) { return (i >= 0 && i < RD_LINES) ? gLines[i] : ""; }

void rdNext() {
  if (!gOpen || gEnd >= gSize) return;
  if (gHistN < RD_HIST_MAX) gHist[gHistN] = gStart;
  if (gHistN < RD_HIST_MAX) gHistN++;
  gStart = gEnd;
  layout(gStart);
  saveProgress();
}

void rdPrev() {
  if (!gOpen) return;

  if (gHistN == 0) {
    if (gStart == 0) return;
    /* 历史栈空了（比如上次退出后重新打开）：从头重排一遍把页起点补回来。
     * 一页约 150 字节，几十 KB 的书也就几百次 layout，肉眼几乎无感。 */
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
  saveProgress();
}

bool rdNextBook() {
  File dir = LittleFS.open(RD_DIR);
  if (!dir) return false;

  /* 把所有 txt 收进来，找到当前这本的下一本 */
  char names[16][40];
  int n = 0;
  File f = dir.openNextFile();
  while (f && n < 16) {
    if (!f.isDirectory()) {
      const char *nm = f.name();
      const char *slash = strrchr(nm, '/');
      snprintf(names[n], sizeof(names[n]), "%s", slash ? slash + 1 : nm);
      n++;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  if (n <= 1) return false;
  int cur = -1;
  for (int i = 0; i < n; i++) if (!strcmp(names[i], gName)) { cur = i; break; }
  int next = (cur + 1) % n;
  return rdOpen(names[next]);
}

void rdSaveProgress() { saveProgress(); }
