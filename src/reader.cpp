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
#define RD_TOC_MAX   96
#define RD_TOC_NAME  40
#define EZB_BLOCK    16384   /* 块上限，实际值以文件头为准 */
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
static char     gTocName[RD_TOC_MAX][RD_TOC_NAME];
static int      gTocN = 0;

static uint32_t gHist[RD_HIST_MAX];
static int      gHistN = 0;

/* 书目 */
static char     gBooks[RD_BOOK_MAX][RD_NAME_MAX];
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
      if (w + gw > RD_WIDTH && w > 0) break;   /* 放不下：本行到此为止，字留在 p */

      if (n + 5 < RD_LINE_BYTES) n += utf8Put(out + n, cp);
      w += gw;
      p = nx;
    }

    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;

    if (p <= pos) {                       /* 一个字都没排进去，收工 */
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
/* 只认 .ezzbk：纯文本/二进制没有容器头，正文里出现 0x00、超大单行都会让
 * 排版和分块读取出问题，干脆收口到唯一格式。转换交给 neobook。 */
static bool isBook(const char *n) {
  size_t l = strlen(n);
  return (l > 6 && !strcasecmp(n + l - 6, ".ezzbk"));
}

/* .ezzbk 头里的原始长度；不是 EZZB 就返回文件大小 */
static uint32_t bookRawSize(File &f) {
  uint32_t fsz = (uint32_t)f.size();
  uint32_t save = f.position();
  uint8_t  h[20];
  f.seek(0);
  int n = f.read(h, 20);
  f.seek(save);
  if (n == 20 && !memcmp(h, "EZZB", 4))
    return (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
  return fsz;
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
          gBookRaw[gBookN]  = bookRawSize(f);
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
    uint32_t ts, tr;
    snprintf(tn, sizeof(tn), "%s", gBooks[i]);
    ts = gBookSize[i];
    tr = gBookRaw[i];
    int j = i - 1;
    while (j >= 0 && strcmp(gBooks[j], tn) > 0) {
      snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", gBooks[j]);
      gBookSize[j + 1] = gBookSize[j];
      gBookRaw[j + 1]  = gBookRaw[j];
      j--;
    }
    snprintf(gBooks[j + 1], RD_NAME_MAX, "%s", tn);
    gBookSize[j + 1] = ts;
    gBookRaw[j + 1]  = tr;
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

const char *rdBookName(int i) {
  return (i >= 0 && i < gBookN) ? gBooks[i] : "";
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
  gAlgo = 0;
  gBlkCount = 0;
  gBlkSize = EZB_BLOCK;
  gSize = (uint32_t)gFile.size();

  uint8_t magic[4] = {0};
  gFile.seek(0);
  if (gFile.read(magic, 4) == 4 && !memcmp(magic, "EZZB", 4)) {
    /* v2: "EZZB"|algo|flags|rsv(2)|rawSize|blockSize|blockCount|off[n+1]  头 20B
       旧: "EZZB"|rawSize|blockSize|blockCount|off[n+1]                   头 16B
       两者靠 off[0]/off[n] 与文件长度对账区分（rawSize 低字节可能恰好是 0/1，不可靠）。 */
    uint8_t h[20] = {0};
    gFile.seek(0);
    int hn = gFile.read(h, 20);
    const uint32_t fsz = (uint32_t)gFile.size();

    bool ok = false;
    for (int pass = 0; pass < 2 && !ok; pass++) {
      uint32_t n_algo, raw, bs, cnt, hdrLen;
      if (pass == 0) {                       /* 新布局 */
        if (hn < 20) continue;
        n_algo = h[4];
        raw = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
        bs  = (uint32_t)h[12] | ((uint32_t)h[13] << 8) | ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
        cnt = (uint32_t)h[16] | ((uint32_t)h[17] << 8) | ((uint32_t)h[18] << 16) | ((uint32_t)h[19] << 24);
        hdrLen = 20;
        if (n_algo > 1) continue;
      } else {                               /* 旧布局，只支持 LZ4 */
        if (hn < 16) continue;
        n_algo = 0;
        raw = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        bs  = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
        cnt = (uint32_t)h[12] | ((uint32_t)h[13] << 8) | ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
        hdrLen = 16;
      }
      if (bs == 0 || bs > EZB_BLOCK || cnt == 0 || cnt > 4096 || raw == 0) continue;
      if (hdrLen + 4 * (cnt + 1) > fsz) continue;

      uint32_t *off = (uint32_t *)malloc((cnt + 1) * sizeof(uint32_t));
      if (!off) continue;
      gFile.seek(hdrLen);
      if (gFile.read((uint8_t *)off, (cnt + 1) * sizeof(uint32_t)) !=
          (int)((cnt + 1) * sizeof(uint32_t))) {
        free(off);
        continue;
      }
      /* 对账：首块紧跟头部，末块正好落在文件尾 */
      if (off[0] != hdrLen + 4 * (cnt + 1) || off[cnt] != fsz) { free(off); continue; }

      gBlkOff = off;
      gAlgo = (uint8_t)n_algo;
      gBlkSize = bs;
      gBlkCount = cnt;
      gSize = raw;
      gEzb = true;
      ok = true;
    }

    if (!gEzb) {
      appLog("[阅读] %s 是 EZZB 但头部损坏\r\n", gName);
      gFile.close();
      return false;
    }
    appLog("[阅读] EZZB algo=%s 块=%u 块数=%u\r\n",
           gAlgo == 1 ? "deflate" : "lz4", (unsigned)gBlkSize, (unsigned)gBlkCount);
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
