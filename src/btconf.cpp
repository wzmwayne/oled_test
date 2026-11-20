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
static void cfgGet(char *ssid, size_t ssidLen, char *pass, size_t passLen,
                   char *ntp, size_t ntpLen, int *tz) {
  Preferences p;
  p.begin(NVS_NS, true);
  String s = p.getString("ssid", "");
  String w = p.getString("pass", "");
  String n = p.getString("ntp", "ntp.aliyun.com");
  int    z = p.getInt("tz", 8);
  p.end();
  strncpy(ssid, s.c_str(), ssidLen - 1); ssid[ssidLen - 1] = 0;
  strncpy(pass, w.c_str(), passLen - 1); pass[passLen - 1] = 0;
  strncpy(ntp,  n.c_str(), ntpLen - 1);  ntp[ntpLen - 1] = 0;
  *tz = z;
}

static void cfgSetWifi(const char *ssid, const char *pass) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

static void cfgSetNtp(const char *server, int tz) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("ntp", server);
  p.putInt("tz", tz);
  p.end();
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
    char ssid[40], pass[68], ntp[68]; int tz = 8;
    cfgGet(ssid, sizeof(ssid), pass, sizeof(pass), ntp, sizeof(ntp), &tz);
    resp["ok"] = true;
    resp["params"]["ssid"] = ssid;
    resp["params"]["pass"] = pass;
    resp["params"]["ntp"]  = ntp;
    resp["params"]["tz"]   = tz;

  } else if (!strcmp(action, "setwifi")) {
    const char *ssid = params["ssid"] | "";
    const char *pass = params["pass"] | "";
    if (!*ssid) { resp["ok"] = false; resp["msg"] = "ssid 为空"; }
    else { cfgSetWifi(ssid, pass); resp["ok"] = true; resp["msg"] = "已保存"; }

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
  NimBLEDevice::setSecurityAuth(true, true, false);   /* bonding, mitm, 不强制 SC */
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey((uint32_t)atoi(gPin));

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

