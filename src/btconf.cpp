/*
 * btconf.cpp - 蓝牙配置模块（单特征 JSON）
 *
 * 异步说明：NimBLEDevice::init() 会阻塞几十到几百毫秒，直接在主循环里调
 * 会让屏幕卡住。所以 btBegin() 只置状态并起一个任务，真正的初始化在
 * FreeRTOS 任务里做，主循环照常刷"启动服务"加载动画。
 */
#include "btconf.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>
#include <LittleFS.h>
#include "timetable.h"
#include "reader.h"

/* ---------- UUID ---------- */
#define BT_SVC_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BT_CHR_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BT_DAT_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   /* 二进制批量上传 */
#define BT_DEV_NAME  "OLED-Panel"

#define NVS_NS       "cfg"
#define MAX_PIN_ERR  3

/* BLE_HS_IO_DISPLAY_ONLY = 3，避免依赖 ble_hs.h 头文件路径 */
#ifndef BLE_HS_IO_DISPLAY_ONLY
#define BLE_HS_IO_DISPLAY_ONLY 3
#endif

/* ArduinoJson v6/v7 兼容 */
#if ARDUINOJSON_VERSION_MAJOR >= 7
typedef JsonDocument JDoc;
#else
typedef StaticJsonDocument<512> JDoc;
#endif

enum BtState { BT_STOPPED = 0, BT_STARTING, BT_ADVERTISING, BT_CONNECTED };
static volatile BtState gState = BT_STOPPED;
static char     gPin[8] = "000000";
static uint32_t gPinErr = 0;

static NimBLEServer          *gServer = nullptr;
static NimBLECharacteristic  *gChr    = nullptr;
static NimBLECharacteristic  *gDat    = nullptr;
static TaskHandle_t           gTask   = nullptr;

extern void appLog(const char *fmt, ...);

/* ==================== 二进制批量上传 ====================
 * 手机把文件切成 <=512 字节的小块，写 BT_DAT_UUID（无应答），
 * 设备边收边写盘并累计 FNV-1a 校验。每收满 gBulkChunk 字节 NOTIFY 一次
 * {len, hash}，手机核对后继续；对不上就 bbad 回退到上一次成功的位置重传。
 */
static NimBLECharacteristic *gCtrl = nullptr;      /* 就是 gChr，回包用 */
static File     gUpFile;
static char     gUpName[40] = "";
static uint32_t gUpLen    = 0;      /* 已收字节 */
static uint32_t gUpHash   = 2166136261u;
static uint32_t gUpAckLen = 0;      /* 上一次确认的位置/校验 */
static uint32_t gUpExpect = 0;      /* bopen 声明的总长度，用来给最后一段补 ack */
static uint32_t gUpAckHash= 2166136261u;
static uint32_t gBulkChunk = 32768; /* 每段校验大小，默认 32KB */

static inline uint32_t fnv1a(uint32_t h, const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

static void bulkClose() {
  if (gUpFile) gUpFile.close();
}

/* 回包里带上进度，手机会拿它对校验 */
static void bulkAck() {
  if (!gChr) return;
  char out[128];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"action\":\"bputack\",\"params\":{\"len\":%u,\"hash\":%u}}",
           (unsigned)gUpLen, (unsigned)gUpHash);
  gChr->setValue((uint8_t *)out, strlen(out));
  gChr->notify();
}


/* ==================== 配置存取 ==================== */
/* 设备不联网，只保留时区：手机发来的是 UTC 时间戳，写 RTC 前要加上它。 */
int btCfgGetTz() {
  Preferences p;
  p.begin(NVS_NS, true);
  int z = p.getInt("tz", 8);
  p.end();
  if (z < -12 || z > 14) z = 8;
  return z;
}

void btCfgSetTz(int tz) {
  if (tz < -12 || tz > 14) return;
  Preferences p;
  p.begin(NVS_NS, false);
  p.putInt("tz", tz);
  p.end();
}


/* ==================== 课表整体下发 ====================
 * 一次 BLE 写放不下整张表，按「追加 → 结束」两段式；分片边收边落盘，
 * 不再整表驻留内存，条数只受 flash 剩余空间限制：
 *   {"action":"settt","params":{"reset":true}}                   清暂存文件
 *   {"action":"settt","params":{"data":"<JSON 片段>"}}           多次追加，回 len/hash
 *   {"action":"settt","params":{"end":true,"size":N,"hash":H}}   校验后流式解析并整表替换
 * hash 是整表 JSON 的 FNV-1a 32 位（与传书同一个函数）：对不上就整份丢弃，
 * 旧课表一个字都不动。size/hash 传 0 或缺省时不校验，兼容老客户端。
 */
#define TT_TMP           "/tt.tmp"
#define TT_TMP_MAX       65536    /* 暂存上限 64KB（约 1500 条），防跑飞把 flash 写满 */
#define TT_TMP_FREE_MIN  16384    /* reset 时要求至少这么多空闲，免得写到一半满 */
static uint32_t gTtBytes = 0;
static uint32_t gTtHash  = 2166136261u;   /* FNV-1a 初值，与传书一致 */

static void ttTmpClear() {
  LittleFS.remove(TT_TMP);
  gTtBytes = 0;
  gTtHash  = 2166136261u;
}

/* ==================== 书籍上传（base64 分块） ==================== */
#define BOOK_DIR "/books"

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int b64decode(const char *in, uint8_t *out, int max) {
  int v = 0, bits = -8, n = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '=') break;
    int d = b64val(*p);
    if (d < 0) continue;            /* 跳过换行/空格 */
    v = (v << 6) | d;
    bits += 6;
    if (bits >= 0) {
      if (n >= max) return -1;
      out[n++] = (uint8_t)((v >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return n;
}

/* 只允许简单的文件名，防止 ../ 越界 */
static bool bookNameOk(const char *n) {
  if (!n || !*n || strlen(n) > 32) return false;
  for (const char *p = n; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':') return false;
  }
  size_t l = strlen(n);
  return (l > 6 && !strcasecmp(n + l - 6, ".ezzbk"));   /* 只收 .ezzbk */
}

/* 本地星期 1..7（周一=1，周日=7），课表按这个编号存 */
static int localWeekday() {
  int y = 0, mo = 1, d = 1;
  if (!appNowYmd(&y, &mo, &d)) return 1;
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (mo < 3) y -= 1;
  int w = (y + y / 4 - y / 100 + y / 400 + t[mo - 1] + d) % 7;   /* 0=周日 */
  return (w == 0) ? 7 : w;
}

/* ==================== 回包 ==================== */
static void reply(JDoc &d) {
  if (!gChr) return;
  String out;
  serializeJson(d, out);
  gChr->setValue((uint8_t *)out.c_str(), out.length());
  gChr->notify();
  appLog("[BT] -> %s\r\n", out.c_str());
}

/* ==================== 命令处理 ==================== */
static void handleJson(const char *in, size_t len) {
  JDoc doc;
  DeserializationError err = deserializeJson(doc, in, len);
  JDoc resp;

  if (err) {
    resp["ok"] = false;
    resp["msg"] = "JSON 解析失败";
    reply(resp);
    return;
  }

  const char *pin = doc["pin"] | "";
  if (strcmp(pin, gPin) != 0) {
    gPinErr++;
    resp["ok"] = false;
    resp["msg"] = "PIN 错误";
    reply(resp);
    appLog("[BT] PIN 错误 %u/%u\r\n", (unsigned)gPinErr, (unsigned)MAX_PIN_ERR);
    if (gPinErr >= MAX_PIN_ERR && gServer) {
      /* 错满次数直接踢掉连接，逼客户端重新配对 */
      appLog("[BT] PIN 错误过多，断开全部连接\r\n");
      NimBLEDevice::deinit(true);
      gServer = nullptr;
      bulkClose();
      gChr = nullptr;
      gDat = nullptr;
      gState = BT_STOPPED;
    }
    return;
  }
  gPinErr = 0;

  const char *action = doc["action"] | "";
  JsonObject params = doc["params"].as<JsonObject>();
  resp["action"] = action;

  if (!strcmp(action, "ping")) {
    resp["ok"] = true;
    resp["msg"] = "pong";

  } else if (!strcmp(action, "get")) {
    resp["ok"] = true;
    resp["params"]["tz"]          = btCfgGetTz();
    resp["params"]["tt_count"]    = ttCount();
    resp["params"]["week_parity"] = ttIsOddWeek() ? 1 : 2;
    resp["params"]["fs_free"]     = (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes());

  } else if (!strcmp(action, "settz")) {
    int tz = params["tz"] | 8;
    if (tz < -12 || tz > 14) { resp["ok"] = false; resp["msg"] = "时区范围 -12..14"; }
    else { btCfgSetTz(tz); resp["ok"] = true; resp["msg"] = "时区已保存"; }

  } else if (!strcmp(action, "settime")) {
    uint32_t ts = params["ts"] | 0;
    if (ts == 0) { resp["ok"] = false; resp["msg"] = "ts 无效"; }
    else if (appRtcWriteUnix(ts)) { resp["ok"] = true; resp["msg"] = "时间已写入"; }
    else { resp["ok"] = false; resp["msg"] = "写 RTC 失败"; }

  } else if (!strcmp(action, "gettime")) {
    uint32_t ts = 0;
    if (appRtcReadUnix(&ts)) { resp["ok"] = true; resp["params"]["ts"] = ts; }
    else { resp["ok"] = false; resp["msg"] = "读 RTC 失败"; }

  } else if (!strcmp(action, "fsstat")) {
    /* 上传前先看空间，避免传到一半 littlefs 写失败 */
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    resp["ok"] = true;
    resp["params"]["total"] = (uint32_t)total;
    resp["params"]["used"]  = (uint32_t)used;
    resp["params"]["free"]  = (uint32_t)(total > used ? total - used : 0);
    resp["params"]["heap"]  = (uint32_t)ESP.getFreeHeap();

  } else if (!strcmp(action, "blist")) {
    if (!LittleFS.exists(BOOK_DIR)) LittleFS.mkdir(BOOK_DIR);
    JsonArray arr = resp["params"]["books"].to<JsonArray>();
    int n = 0;
    File dir = LittleFS.open(BOOK_DIR);
    if (dir) {
      File f = dir.openNextFile();
      while (f && n < 16) {                 /* 与固件书目上限 RD_BOOK_MAX 一致 */
        if (!f.isDirectory()) {
          const char *nm = f.name();
          const char *slash = strrchr(nm, '/');
          const char *base = slash ? slash + 1 : nm;
          char bt[RD_TITLE_MAX];
          JsonObject o = arr.add<JsonObject>();
          o["name"] = base;
          o["size"] = (uint32_t)f.size();
          /* 容器元数据里的书名，设备书目显示的也是它；没有就不带这个字段 */
          rdBookMetaTitle(base, bt, sizeof(bt));
          if (bt[0]) o["t"] = bt;
          n++;
        }
        f.close();
        f = dir.openNextFile();
      }
      dir.close();
    }
    resp["ok"] = true;
    resp["params"]["count"] = n;

  } else if (!strcmp(action, "bopen")) {
    /* {"name":"x.ezzbk","size":123456,"chunk":32768} —— 开一个批量上传 */
    const char *name = params["name"] | "";
    uint32_t size  = params["size"] | 0;
    uint32_t chunk = params["chunk"] | 0;
    if (chunk >= 4096 && chunk <= 262144) gBulkChunk = chunk;
    if (!bookNameOk(name)) {
      resp["ok"] = false;
      resp["msg"] = "文件名不合法";
    } else {
      bulkClose();
      if (!LittleFS.exists(BOOK_DIR)) LittleFS.mkdir(BOOK_DIR);
      size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
      if (size && size > freeB) {
        resp["ok"] = false;
        resp["msg"] = "空间不足";
      } else {
        char path[64];
        snprintf(path, sizeof(path), BOOK_DIR "/%s", name);
        gUpFile = LittleFS.open(path, "w");
        if (!gUpFile) {
          resp["ok"] = false;
          resp["msg"] = "无法创建文件";
        } else {
          snprintf(gUpName, sizeof(gUpName), "%s", name);
          gUpLen = 0;  gUpHash = 2166136261u;
          gUpAckLen = 0; gUpAckHash = gUpHash;
          gUpExpect = size;
          resp["ok"] = true;
          resp["msg"] = "开始上传";
          resp["params"]["chunk"] = gBulkChunk;
        }
      }
    }

  } else if (!strcmp(action, "bsync")) {
    /* 手机等不到 ack 时来问一句：当前收了多少、校验是多少 */
    resp["ok"] = (bool)gUpFile;
    resp["msg"] = gUpFile ? "当前进度" : "没有进行中的上传";
    resp["params"]["len"]  = gUpLen;
    resp["params"]["hash"] = gUpHash;
    resp["params"]["expect"] = gUpExpect;

  } else if (!strcmp(action, "bbad")) {
    /* 手机发现校验对不上：退回到上一次确认的位置重传 */
    if (!gUpFile) {
      resp["ok"] = false;
      resp["msg"] = "没有进行中的上传";
    } else if (!gUpFile.seek(gUpAckLen)) {
      resp["ok"] = false;
      resp["msg"] = "回退失败";
    } else {
      gUpLen = gUpAckLen; gUpHash = gUpAckHash;
      resp["ok"] = true;
      resp["msg"] = "已回退";
      resp["params"]["len"] = gUpLen;
      resp["params"]["hash"] = gUpHash;
    }

  } else if (!strcmp(action, "bclose")) {
    /* {"size":N,"hash":H} —— 整本校验，通过才认 */
    uint32_t size = params["size"] | 0;
    uint32_t hash = params["hash"] | 0;
    if (!gUpFile) {
      resp["ok"] = false;
      resp["msg"] = "没有进行中的上传";
    } else {
      /* LittleFS 有写缓冲，没 flush 之前 size() 不准——之前就是在这里拿到
       * 偏小的值，明明校验全对却报「长度不符」。先 flush 再关，最后重新 stat。 */
      gUpFile.flush();
      gUpFile.close();
      uint32_t fsz = 0;
      {
        char bp2[64];
        snprintf(bp2, sizeof(bp2), BOOK_DIR "/%s", gUpName);
        File chk = LittleFS.open(bp2, "r");
        if (chk) { fsz = chk.size(); chk.close(); }
      }
      bool okSize = (size == 0 || size == gUpLen) && (fsz == gUpLen);
      bool okHash = (hash == 0 || hash == gUpHash);
      resp["ok"] = okSize && okHash;
      resp["msg"] = (okSize && okHash) ? "校验通过"
                  : (!okSize ? "长度不符" : "校验值不符");
      resp["params"]["size"] = gUpLen;
      resp["params"]["hash"] = gUpHash;
      if (okSize && okHash) {
        rdForget(gUpName);              /* 同名重传，旧进度作废 */
        rdScan();
      } else {
        char bp[64];                      /* 校验不过就整本丢弃，不留半截文件 */
        snprintf(bp, sizeof(bp), BOOK_DIR "/%s", gUpName);
        LittleFS.remove(bp);
      }
      appLog("[BT] bclose %s len=%u hash=%08X -> %s\r\n", gUpName,
             (unsigned)gUpLen, (unsigned)gUpHash,
             (okSize && okHash) ? "OK" : "FAIL");
    }

  } else if (!strcmp(action, "bput")) {
    /* {"name":"三体.txt","off":0,"data":"<base64>"} —— 每包原文 <= 256 字节 */
    const char *name = params["name"] | "";
    uint32_t off = params["off"] | 0;
    const char *data = params["data"] | "";
    if (!bookNameOk(name)) {
      resp["ok"] = false;
      resp["msg"] = "文件名不合法";
    } else {
      static uint8_t buf[320];
      int n = b64decode(data, buf, sizeof(buf));
      if (n <= 0) {
        resp["ok"] = false;
        resp["msg"] = "base64 解码失败";
      } else {
        if (!LittleFS.exists(BOOK_DIR)) LittleFS.mkdir(BOOK_DIR);
        char path[64];
        snprintf(path, sizeof(path), BOOK_DIR "/%s", name);
        File f = LittleFS.open(path, "r+");
        if (!f) f = LittleFS.open(path, "w+");
        if (!f) {
          resp["ok"] = false;
          resp["msg"] = "无法创建文件（空间不足？）";
        } else {
          f.seek(off);
          size_t wrote = f.write(buf, n);
          uint32_t sz = f.size();
          f.close();
          resp["ok"] = true;
          resp["params"]["next"] = off + (uint32_t)wrote;
          resp["params"]["size"] = sz;
        }
      }
    }

  } else if (!strcmp(action, "bcancel")) {
    /* 手机中途放弃：关句柄并把半截文件删掉，别占着空间也别卡住后面的删除 */
    if (gUpFile) {
      bulkClose();
      char bp[64];
      snprintf(bp, sizeof(bp), BOOK_DIR "/%s", gUpName);
      LittleFS.remove(bp);
      resp["msg"] = "已取消";
    } else {
      resp["msg"] = "没有进行中的上传";
    }
    gUpName[0] = 0;
    gUpLen = 0;
    gUpAckLen = 0;
    gUpExpect = 0;
    resp["ok"] = true;

  } else if (!strcmp(action, "bdel")) {
    const char *name = params["name"] | "";
    if (!bookNameOk(name)) {
      resp["ok"] = false;
      resp["msg"] = "文件名不合法";
    } else {
      /* 上次上传中途失败会把文件句柄占着，LittleFS 不肯删，先放开 */
      if (gUpFile) {
        bulkClose();
        gUpName[0] = 0;
        gUpLen = 0;
        gUpAckLen = 0;
        gUpExpect = 0;
      }
      char path[64];
      snprintf(path, sizeof(path), BOOK_DIR "/%s", name);
      bool ok = LittleFS.remove(path);
      if (!ok && !LittleFS.exists(path)) ok = true;   /* 已经不在了也算成功 */
      if (ok) rdForget(name);      /* 文件没了，阅读进度也一并清掉 */
      resp["ok"] = ok;
      resp["msg"] = ok ? "已删除" : "文件不存在";
    }

  } else if (!strcmp(action, "settt")) {
    if (params["reset"] | false) {
      ttTmpClear();                       /* 先把上次可能残留的暂存删掉再算空间 */
      size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
      if (freeB < TT_TMP_FREE_MIN) {
        resp["ok"] = false;
        resp["msg"] = "空间不足";
      } else {
        resp["ok"] = true;
        resp["msg"] = "暂存已清空";
        resp["params"]["len"] = 0;
      }

    } else if (params["end"] | false) {
      uint32_t size = params["size"] | 0;
      uint32_t hash = params["hash"] | 0;
      if (gTtBytes == 0) {
        ttTmpClear();
        resp["ok"] = false;
        resp["msg"] = "没有收到任何数据";
      } else if ((size && size != gTtBytes) || (hash && hash != gTtHash)) {
        /* 半路丢包/写坏都算到这里：整份丢弃，旧课表原样保留 */
        resp["ok"] = false;
        resp["msg"] = "长度或校验值不符，已丢弃";
        resp["params"]["len"]  = gTtBytes;
        resp["params"]["hash"] = gTtHash;
        ttTmpClear();
      } else {
        File f = LittleFS.open(TT_TMP, "r");
        bool haveFile = (bool)f;
        int n = haveFile ? ttLoadStream(f) : -1;   /* 边读边解，不再整表进内存 */
        if (haveFile) f.close();
        resp["ok"] = (n >= 0);
        resp["msg"] = (n >= 0) ? "课表已整体写入"
                              : (haveFile ? "JSON 解析失败或格式不对" : "暂存文件打不开");
        if (n >= 0) resp["params"]["count"] = n;
        ttTmpClear();
      }

    } else {
      const char *chunk = params["data"] | "";
      size_t clen = strlen(chunk);
      if (clen == 0) {
        resp["ok"] = false;
        resp["msg"] = "空片段";
      } else if (gTtBytes + clen > TT_TMP_MAX) {
        ttTmpClear();
        resp["ok"] = false;
        resp["msg"] = "课表超过 64KB，请减少条数";
      } else {
        bool wrote = false;
        {
          File f = LittleFS.open(TT_TMP, "a");     /* 追加写，写完就关，别跨命令持句柄 */
          if (f) {
            wrote = (f.write((const uint8_t *)chunk, clen) == clen);
            f.close();
          }
        }
        if (!wrote) {
          ttTmpClear();
          resp["ok"] = false;
          resp["msg"] = "写暂存文件失败（空间不足？）";
        } else {
          gTtBytes += (uint32_t)clen;
          gTtHash = fnv1a(gTtHash, (const uint8_t *)chunk, clen);
          resp["ok"] = true;
          resp["params"]["len"]  = gTtBytes;
          resp["params"]["hash"] = gTtHash;
        }
      }
    }

  } else if (!strcmp(action, "gettt")) {
    resp["ok"] = true;
    resp["params"]["count"]       = ttCount();
    resp["params"]["today"]       = localWeekday();
    resp["params"]["week_parity"] = ttIsOddWeek() ? 1 : 2;

  } else if (!strcmp(action, "listtt")) {
    /* 分页：一次最多 4 条，保证整包响应能塞进一次 notify（MTU 517 -> 514B） */
    int from = params["from"]  | 0;
    int cnt  = params["count"] | 4;
    if (from < 0) from = 0;
    if (cnt < 1) cnt = 1;
    if (cnt > 4) cnt = 4;
    int total = ttCount();
    JsonArray arr = resp["params"]["courses"].to<JsonArray>();
    for (int i = from; i < total && i < from + cnt; i++) {
      const TtCourse *c = ttGet(i);
      if (!c) break;
      JsonObject o = arr.add<JsonObject>();
      o["name"]   = c->name;
      o["day"]    = c->day;
      o["start"]  = c->start;
      o["end"]    = c->end;
      o["parity"] = c->parity;
      o["global"] = c->global;
      o["event"]  = (c->start == c->end) ? 1 : 0;
    }
    resp["ok"] = true;
    resp["params"]["total"] = total;
    resp["params"]["from"]  = from;

  } else if (!strcmp(action, "addcourse")) {
    const char *name = params["name"] | "";
    int day   = params["day"]    | 0;
    int start = params["start"]  | -1;
    int end   = params["end"]    | -1;
    int par   = params["parity"] | 0;
    int glob  = params["global"] | 0;          /* 事件：1=全局 */
    int idx = ttAddEx(name, day, start, end, par, glob);
    if (idx < 0) {
      resp["ok"]  = false;
      resp["msg"] = "保存失败：名称/星期/时间不合法，或已满 72 条";
    } else {
      resp["ok"]  = true;
      resp["msg"] = "已保存";
      resp["params"]["index"] = idx;
      resp["params"]["count"] = ttCount();
    }

  } else if (!strcmp(action, "delcourse")) {
    int idx = params["index"] | -1;
    bool ok = ttDel(idx);
    resp["ok"]  = ok;
    resp["msg"] = ok ? "已删除" : "索引无效";
    resp["params"]["count"] = ttCount();

  } else if (!strcmp(action, "fliptt")) {
    /* 开学第一周对表用：单周课变双周、双周课变单周，每周课不动 */
    int changed = ttFlipParity();
    resp["ok"]  = true;
    resp["msg"] = changed ? "单双周已互换" : "没有单双周课程需要互换";
    resp["params"]["changed"] = changed;
    resp["params"]["count"]   = ttCount();

  } else if (!strcmp(action, "cleartt")) {
    ttClear();
    resp["ok"]  = true;
    resp["msg"] = "课表已清空";
    resp["params"]["count"] = 0;

  } else if (!strcmp(action, "unpair")) {
    NimBLEDevice::deleteAllBonds();
    resp["ok"] = true;
    resp["msg"] = "配对已清除";
    reply(resp);
    btClearBonds();
    return;

  } else {
    resp["ok"] = false;
    resp["msg"] = "未知 action";
  }

  reply(resp);
}

/* ==================== BLE 回调 ==================== */
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    gState = BT_CONNECTED;
    gPinErr = 0;
    appLog("[BT] 已连接 %s\r\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    if (gState == BT_STOPPED) return;
    gState = BT_ADVERTISING;
    NimBLEDevice::startAdvertising();
    appLog("[BT] 已断开 reason=%d\r\n", reason);
  }
};

class ChrCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();          /* NimBLEValueAttribute::getValue() 是 public */
    appLog("[BT] <- %.*s\r\n", (int)v.length(), v.c_str());
    handleJson(v.c_str(), v.length());
  }
};

/* 数据通道：纯二进制，无应答写，边收边写盘 */
class DatCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    if (!gUpFile) return;
    NimBLEAttValue v = c->getValue();
    size_t n = v.length();
    if (!n) return;
    const uint8_t *d = (const uint8_t *)v.data();
    if (gUpFile.write(d, n) != n) { appLog("[BT] 写盘失败，空间不足？\r\n"); return; }
    gUpLen += (uint32_t)n;
    gUpHash = fnv1a(gUpHash, d, n);
    /* 满一段就回一次；最后一段通常不足 gBulkChunk，靠 gUpExpect 补齐，
     * 否则手机发完尾巴永远等不到回包（就是那个「等待校验回包超时」）。 */
    if (gUpLen - gUpAckLen >= gBulkChunk || (gUpExpect && gUpLen >= gUpExpect)) {
      gUpAckLen = gUpLen;
      gUpAckHash = gUpHash;
      bulkAck();
    }
  }
};

/* ==================== 启动任务 ==================== */
static void btStartTask(void *arg) {
  appLog("[BT] 任务开始，PIN=%s\r\n", gPin);

  appLog("[BT] init 开始\r\n");
  NimBLEDevice::init(BT_DEV_NAME);
  appLog("[BT] init 完成\r\n");
  NimBLEDevice::setMTU(517);
  /* 不要求连接加密/绑定：手机连上即可直接读写，不弹系统配对框。
   * 安全性由应用层 JSON 里的 pin 字段承担（6 位随机，错 3 次断开服务）。
   * 之前 setSecurityAuth(true,true,false) 强制 bonding+MITM，导致每次
   * 换固件后手机与设备的绑定表对不上，连接报 "Error occurred trying
   * to connect to remote device"。 */
  NimBLEDevice::setSecurityAuth(false, false, false);

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCb());

  appLog("[BT] server 已建\r\n");
  NimBLEService *svc = gServer->createService(BT_SVC_UUID);
  gChr = svc->createCharacteristic(BT_CHR_UUID,
             NIMBLE_PROPERTY::WRITE |
             NIMBLE_PROPERTY::WRITE_NR |
             NIMBLE_PROPERTY::NOTIFY |
             NIMBLE_PROPERTY::READ);
  gChr->setCallbacks(new ChrCb());

  gDat = svc->createCharacteristic(BT_DAT_UUID,
             NIMBLE_PROPERTY::WRITE |
             NIMBLE_PROPERTY::WRITE_NR);
  gDat->setCallbacks(new DatCb());
  svc->start();

  appLog("[BT] service 已启动\r\n");

  /* 广播包只有 31 字节，128 位 UUID 占 18，名字 12，合计 33 —— 溢出。
   * 拆开：广播包放名字（15 字节），扫描响应放 UUID（18 字节），互不挤压。
   *
   * 顺序很关键：NimBLEAdvertising::setName() 在扫描响应开启时会把名字
   * 写进 m_scanData 而不是 m_advData。之前先 setName 再关扫描响应，
   * 名字就随扫描响应一起被丢弃，手机只能看到 MAC、看不到设备名。
   * 所以必须：先关扫描响应 -> 设名字（落入广播包）-> 再单独配扫描响应。 */
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(false);        /* 关键：让 setName 走 m_advData */
  adv->setName(BT_DEV_NAME);

  NimBLEAdvertisementData scanData;
  scanData.addServiceUUID(NimBLEUUID(BT_SVC_UUID));
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);

  adv->setMinInterval(32);      /* 20ms，扫描更容易命中 */
  adv->setMaxInterval(48);      /* 30ms */
  bool started = adv->start();
  appLog("[BT] 广播 start=%d 名称=%s\r\n", started ? 1 : 0, BT_DEV_NAME);

  gState = BT_ADVERTISING;
  gTask = nullptr;
  vTaskDelete(NULL);
}

/* ==================== 对外接口 ==================== */
void btBegin() {
  if (gState != BT_STOPPED) return;
  gPinErr = 0;
  ttTmpClear();     /* 上次传课表如果没走完，暂存文件在这儿回收 */
  /* 100000-999999，保证首位非 0，手机配对框不会因为前导零判为不足 6 位 */
  snprintf(gPin, sizeof(gPin), "%06u", (unsigned)(100000u + (esp_random() % 900000u)));
  gState = BT_STARTING;
  appLog("[BT] 启动请求，PIN=%s\r\n", gPin);
  /* 栈给足：NimBLE init + GATT 建表会有较深的调用链 */
  if (xTaskCreate(btStartTask, "btconf", 10240, nullptr, 3, &gTask) != pdPASS) {
    gState = BT_STOPPED;
    appLog("[BT] 任务创建失败，栈不足？\r\n");
  }
}

void btEnd() {
  if (gState == BT_STOPPED) return;
  /* 等启动任务收尾，避免任务还在跑就 deinit */
  for (int i = 0; i < 40 && gTask; i++) vTaskDelay(pdMS_TO_TICKS(10));
  gState = BT_STOPPED;
  bulkClose();
  NimBLEDevice::deinit(true);
  gServer = nullptr;
  gChr = nullptr;
  gDat = nullptr;
  appLog("[BT] 已停止\r\n");
}

void btPoll() {
  /* 目前状态由回调直接维护，这里留作以后轮询超时等 */
}

bool btIsConnected() { return gState == BT_CONNECTED; }
bool btStarting()    { return gState == BT_STARTING; }

const char *btStateText() {
  switch (gState) {
    case BT_STARTING:    return "启动服务";
    case BT_ADVERTISING: return "等待连接";
    case BT_CONNECTED:   return "已连接";
    default:             return "已停止";
  }
}

const char *btPinText() { return gPin; }
uint32_t btPinErrors() { return gPinErr; }

void btClearBonds() {
  appLog("[BT] 清除配对并重启服务\r\n");
  btEnd();
  vTaskDelay(pdMS_TO_TICKS(50));
  btBegin();
}

