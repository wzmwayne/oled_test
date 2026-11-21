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

/* ---------- UUID ---------- */
#define BT_SVC_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BT_CHR_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
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
static TaskHandle_t           gTask   = nullptr;

extern void appLog(const char *fmt, ...);

/* ==================== 配置存取 ==================== */
static void cfgSetNtp(const char *server, int tz) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("ntp", server);
  p.putInt("tz", tz);
  p.end();
}

bool btCfgGetNtp(char *srv, size_t sl, int *tz) {
  Preferences p;
  p.begin(NVS_NS, true);
  String n = p.getString("ntp", "ntp.aliyun.com");
  int    z = p.getInt("tz", 8);
  p.end();
  snprintf(srv, sl, "%s", n.c_str());
  if (!*srv) snprintf(srv, sl, "ntp.aliyun.com");
  if (z < -12 || z > 14) z = 8;
  *tz = z;
  return true;
}

/* ==================== WiFi 列表 ==================== */
/* NVS 布局：wcnt=条数，ws<i>/wp<i>=第 i 条的 SSID/密码。
 * 老固件只存了单条 ssid/pass，首次读到 wcnt 缺失时自动迁移成第 0 条。 */
int btWifiCount() {
  Preferences p;
  p.begin(NVS_NS, true);
  int n = p.getInt("wcnt", -1);
  if (n < 0) {
    String s = p.getString("ssid", "");
    String w = p.getString("pass", "");
    p.end();
    p.begin(NVS_NS, false);
    if (s.length()) {
      p.putString("ws0", s);
      p.putString("wp0", w);
      p.putInt("wcnt", 1);
      p.end();
      return 1;
    }
    p.putInt("wcnt", 0);
    p.end();
    return 0;
  }
  p.end();
  return n > BT_WIFI_MAX ? BT_WIFI_MAX : n;
}

bool btWifiGet(int i, char *ssid, size_t sl, char *pass, size_t pl) {
  int n = btWifiCount();
  if (i < 0 || i >= n) return false;
  char k[8];
  Preferences p;
  p.begin(NVS_NS, true);
  snprintf(k, sizeof(k), "ws%d", i);
  String s = p.getString(k, "");
  snprintf(k, sizeof(k), "wp%d", i);
  String w = p.getString(k, "");
  p.end();
  snprintf(ssid, sl, "%s", s.c_str());
  if (pass) snprintf(pass, pl, "%s", w.c_str());
  return true;
}

int btWifiAdd(const char *ssid, const char *pass) {
  if (!ssid || !*ssid) return -1;
  char s[40], w[68];
  int  n = btWifiCount();
  for (int i = 0; i < n; i++) {
    if (btWifiGet(i, s, sizeof(s), w, sizeof(w)) && !strcmp(s, ssid)) {
      Preferences p;
      p.begin(NVS_NS, false);
      char k[8];
      snprintf(k, sizeof(k), "wp%d", i);
      p.putString(k, pass ? pass : "");
      p.end();
      return i;
    }
  }
  if (n >= BT_WIFI_MAX) return -1;
  Preferences p;
  p.begin(NVS_NS, false);
  char k[8];
  snprintf(k, sizeof(k), "ws%d", n); p.putString(k, ssid);
  snprintf(k, sizeof(k), "wp%d", n); p.putString(k, pass ? pass : "");
  p.putInt("wcnt", n + 1);
  p.end();
  return n;
}

bool btWifiDel(const char *ssid) {
  if (!ssid || !*ssid) return false;
  int n = btWifiCount();
  if (n <= 0) return false;
  char ss[BT_WIFI_MAX][40], pp[BT_WIFI_MAX][68];
  for (int i = 0; i < n; i++) btWifiGet(i, ss[i], sizeof(ss[i]), pp[i], sizeof(pp[i]));
  int idx = -1;
  for (int i = 0; i < n; i++) if (!strcmp(ss[i], ssid)) { idx = i; break; }
  if (idx < 0) return false;
  Preferences p;
  p.begin(NVS_NS, false);
  char k[8];
  for (int i = idx; i < n - 1; i++) {
    snprintf(k, sizeof(k), "ws%d", i); p.putString(k, ss[i + 1]);
    snprintf(k, sizeof(k), "wp%d", i); p.putString(k, pp[i + 1]);
  }
  snprintf(k, sizeof(k), "ws%d", n - 1); p.remove(k);
  snprintf(k, sizeof(k), "wp%d", n - 1); p.remove(k);
  p.putInt("wcnt", n - 1);
  p.end();
  return true;
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
  return true;
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
      gChr = nullptr;
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
    char ntp[68]; int tz = 8;
    btCfgGetNtp(ntp, sizeof(ntp), &tz);
    resp["ok"] = true;
    resp["params"]["ntp"]        = ntp;
    resp["params"]["tz"]         = tz;
    resp["params"]["wifi_count"] = btWifiCount();
    resp["params"]["tt_count"]   = ttCount();
    resp["params"]["week_parity"] = ttIsOddWeek() ? 1 : 2;

  } else if (!strcmp(action, "listwifi")) {
    int n = btWifiCount();
    JsonArray arr = resp["params"]["list"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      char s[40], w[68];
      btWifiGet(i, s, sizeof(s), w, sizeof(w));
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = s;
      o["pass"] = w;
    }
    resp["ok"] = true;
    resp["params"]["count"] = n;

  } else if (!strcmp(action, "setwifi")) {
    const char *ssid = params["ssid"] | "";
    const char *pass = params["pass"] | "";
    if (!*ssid) { resp["ok"] = false; resp["msg"] = "ssid 为空"; }
    else {
      int idx = btWifiAdd(ssid, pass);
      if (idx < 0) { resp["ok"] = false; resp["msg"] = "列表已满(最多5个)"; }
      else {
        resp["ok"] = true;
        resp["msg"]  = "已保存";
        resp["params"]["index"] = idx;
        resp["params"]["count"] = btWifiCount();
      }
    }

  } else if (!strcmp(action, "delwifi")) {
    const char *ssid = params["ssid"] | "";
    if (!*ssid) { resp["ok"] = false; resp["msg"] = "ssid 为空"; }
    else {
      bool ok = btWifiDel(ssid);
      resp["ok"]  = ok;
      resp["msg"] = ok ? "已删除" : "未找到";
      resp["params"]["count"] = btWifiCount();
    }

  } else if (!strcmp(action, "setntp")) {
    const char *server = params["server"] | "ntp.aliyun.com";
    int tz = params["tz"] | 8;
    cfgSetNtp(server, tz);
    resp["ok"] = true;
    resp["msg"] = "已保存";

  } else if (!strcmp(action, "settime")) {
    uint32_t ts = params["ts"] | 0;
    if (ts == 0) { resp["ok"] = false; resp["msg"] = "ts 无效"; }
    else if (appRtcWriteUnix(ts)) { resp["ok"] = true; resp["msg"] = "时间已写入"; }
    else { resp["ok"] = false; resp["msg"] = "写 RTC 失败"; }

  } else if (!strcmp(action, "gettime")) {
    uint32_t ts = 0;
    if (appRtcReadUnix(&ts)) { resp["ok"] = true; resp["params"]["ts"] = ts; }
    else { resp["ok"] = false; resp["msg"] = "读 RTC 失败"; }

  } else if (!strcmp(action, "blist")) {
    if (!LittleFS.exists(BOOK_DIR)) LittleFS.mkdir(BOOK_DIR);
    JsonArray arr = resp["params"]["books"].to<JsonArray>();
    int n = 0;
    File dir = LittleFS.open(BOOK_DIR);
    if (dir) {
      File f = dir.openNextFile();
      while (f && n < 8) {
        if (!f.isDirectory()) {
          const char *nm = f.name();
          const char *slash = strrchr(nm, '/');
          JsonObject o = arr.add<JsonObject>();
          o["name"] = slash ? slash + 1 : nm;
          o["size"] = (uint32_t)f.size();
          n++;
        }
        f.close();
        f = dir.openNextFile();
      }
      dir.close();
    }
    resp["ok"] = true;
    resp["params"]["count"] = n;

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

  } else if (!strcmp(action, "bdel")) {
    const char *name = params["name"] | "";
    if (!bookNameOk(name)) {
      resp["ok"] = false;
      resp["msg"] = "文件名不合法";
    } else {
      char path[64];
      snprintf(path, sizeof(path), BOOK_DIR "/%s", name);
      bool ok = LittleFS.remove(path);
      resp["ok"] = ok;
      resp["msg"] = ok ? "已删除" : "文件不存在";
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
    int idx = ttAdd(name, day, start, end, par);
    if (idx < 0) {
      resp["ok"]  = false;
      resp["msg"] = "保存失败：名称/星期/时间不合法，或已满 60 条";
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
  NimBLEDevice::deinit(true);
  gServer = nullptr;
  gChr = nullptr;
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

