/*
 * wifidbg.cpp - USB 串口 WiFi 调试命令
 * 见 wifidbg.h。所有输出走 appLog()，同时镜像到 USB 与 UART0。
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include <lwip/ip_addr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <ping/ping_sock.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "wifidbg.h"
#include "btconf.h"

extern void appLog(const char *fmt, ...);

/* 少数 IDF 版本没导出的原因码，补上数值避免编译失败 */
#ifndef WIFI_REASON_NO_AP_FOUND
#define WIFI_REASON_NO_AP_FOUND 201
#endif
#ifndef WIFI_REASON_AUTH_FAIL
#define WIFI_REASON_AUTH_FAIL 202
#endif
#ifndef WIFI_REASON_ASSOC_FAIL
#define WIFI_REASON_ASSOC_FAIL 203
#endif
#ifndef WIFI_REASON_HANDSHAKE_TIMEOUT
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#endif
#ifndef WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#endif
#ifndef WIFI_REASON_BEACON_TIMEOUT
#define WIFI_REASON_BEACON_TIMEOUT 200
#endif
#ifndef WIFI_REASON_CONNECTION_FAIL
#define WIFI_REASON_CONNECTION_FAIL 205
#endif
#ifndef WIFI_REASON_AUTH_EXPIRE
#define WIFI_REASON_AUTH_EXPIRE 2
#endif
#ifndef WIFI_REASON_AUTH_LEAVE
#define WIFI_REASON_AUTH_LEAVE 3
#endif
#ifndef WIFI_REASON_STA_LEAVING
#define WIFI_REASON_STA_LEAVING 36
#endif
#ifndef WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210
#endif

static volatile uint8_t gReason   = 0;
static volatile bool    gReasonOk = false;
static volatile bool    gBusy     = false;

/* 事件计数：连不上时靠它判断「根本没启动」还是「启动了但被拒」 */
static volatile uint32_t gEvStart = 0, gEvConnected = 0, gEvDisconnected = 0, gEvGotIp = 0;

/* ==================== 事件：记下断开原因 ==================== */
static void onWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base != WIFI_EVENT && base != IP_EVENT) return;
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        gEvStart++;
        break;
      case WIFI_EVENT_STA_CONNECTED:
        gEvConnected++;
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        gReason   = e->reason;
        gReasonOk = true;
        gEvDisconnected++;
        break;
      }
      default: break;
    }
  } else if (id == IP_EVENT_STA_GOT_IP) {
    gEvGotIp++;
  }
}

static bool gEvtBound = false;

/* setup() 阶段默认事件循环还没建好，直接注册会拿到 ESP_ERR_INVALID_STATE，
 * 于是所有计数恒为 0 —— 上一版就是这样被我误读成「驱动没动作」。
 * 这里先补建事件循环；仍失败就留到第一次 wifi conn 时再绑（那时 WiFi 已初始化）。 */
static esp_err_t bindEvents() {
  if (gEvtBound) return ESP_OK;
  esp_event_loop_create_default();   /* 已存在会返回 INVALID_STATE，忽略 */
  esp_err_t e1 = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, NULL);
  esp_err_t e2 = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, NULL);
  gEvtBound = (e1 == ESP_OK);
  return gEvtBound ? ESP_OK : e1;
}

void wifiDbgBegin() {
  esp_err_t e = bindEvents();
  appLog("[wifi] 事件绑定: %s（失败会在首次 conn 时重试）\r\n",
         e == ESP_OK ? "OK" : esp_err_to_name(e));
}

static void printEventStats() {
  appLog("[wifi] 事件: START=%u CONNECTED=%u DISCONNECTED=%u GOT_IP=%u\r\n",
         (unsigned)gEvStart, (unsigned)gEvConnected, (unsigned)gEvDisconnected, (unsigned)gEvGotIp);
}

/* 内置公共 DNS：阿里 223.5.5.5 优先，114 兜底。
 * 路由器下发的 DNS 有时不可用（运营商劫持/未下发），这组在校园网和家里都能解析。 */
void wifiSetPublicDns() {
  IPAddress d1(223, 5, 5, 5);
  IPAddress d2(114, 114, 114, 114);
  if (WiFi.setDNS(d1, d2))
    appLog("[wifi] DNS 已内置: 223.5.5.5 (阿里) / 114.114.114.114\r\n");
  else
    appLog("[wifi] DNS 设置失败，仍用路由器下发的 %s\r\n", WiFi.dnsIP().toString().c_str());
}

const char *wifiDbgLastReason() {
  if (!gReasonOk) return "-";
  return WiFi.disconnectReasonName((wifi_err_reason_t)gReason);
}

static const char *reasonHint(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      return "找不到该 AP：核对 SSID 拼写；ESP32-C3 只支持 2.4GHz；隐藏 SSID 也扫不到";
    case WIFI_REASON_AUTH_EXPIRE:
      return "认证无应答（AUTH_EXPIRE）：AP 不回应认证请求。常见于密码错、AP 开了 MAC 过滤/黑名单，"
             "或 WPA2/WPA3 过渡模式与 PMF 不兼容。先拿另一台手机开热点验证设备";
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_STA_LEAVING:
      return "本地主动离开：上一轮连接被取消，属于重连过程中的正常收尾";
    case WIFI_REASON_AUTH_FAIL:
      return "认证失败：密码错误，或 AP 用了设备不支持的加密方式（WPA3-Enterprise 等）";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "四次握手超时：九成是密码错，少数是 AP 开了 802.11w/PMF 或信号太烂";
    case WIFI_REASON_ASSOC_FAIL:
      return "关联被拒：AP 达到最大连接数、MAC 过滤，或信道干扰";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "信标超时：信号太弱或 AP 掉线，靠远就换个位置";
    case WIFI_REASON_CONNECTION_FAIL:
      return "连接失败：AP 主动断链，检查路由器 DHCP/黑名单";
    default:
      return "对照上方 reason 名称查 ESP-IDF wifi_err_reason_t";
  }
}

static const char *statusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    case WL_STOPPED:         return "STOPPED";
    default:                 return "UNKNOWN";
  }
}

static const char *authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "OTHER";
  }
}

/* ==================== 子命令 ==================== */
static void cmdOn() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  delay(100);
  appLog("[wifi] 已开启 STA 模式  MAC=%s\r\n", WiFi.macAddress().c_str());
  appLog("[wifi] 提示：mode=STA 但未指定 AP，用 wifi conn <序号|SSID> 连接\r\n");
}

static void cmdOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  appLog("[wifi] 已关闭，射频释放\r\n");
}

static void cmdStatus() {
  wl_status_t s = WiFi.status();
  appLog("---- WiFi 状态 ----\r\n");
  appLog("模式      : %s\r\n", WiFi.getMode() == WIFI_MODE_STA ? "STA"
                                : WiFi.getMode() == WIFI_MODE_NULL ? "OFF" : "其他");
  appLog("状态      : %s (%d)\r\n", statusName(s), (int)s);
  appLog("MAC       : %s\r\n", WiFi.macAddress().c_str());
  if (s == WL_CONNECTED) {
    appLog("SSID      : %s\r\n", WiFi.SSID().c_str());
    appLog("IP/掩码   : %s / %s\r\n", WiFi.localIP().toString().c_str(),
           WiFi.subnetMask().toString().c_str());
    appLog("网关/DNS  : %s / %s\r\n", WiFi.gatewayIP().toString().c_str(),
           WiFi.dnsIP().toString().c_str());
    appLog("RSSI/信道 : %d dBm / %d\r\n", (int)WiFi.RSSI(), (int)WiFi.channel());
    appLog("BSSID     : %s\r\n", WiFi.BSSIDstr().c_str());
  } else {
    appLog("IP        : 未获取\r\n");
  }
  if (gReasonOk) {
    appLog("最近断开  : %u %s\r\n", (unsigned)gReason, WiFi.disconnectReasonName((wifi_err_reason_t)gReason));
    appLog("诊断      : %s\r\n", reasonHint(gReason));
  } else {
    appLog("最近断开  : 无记录\r\n");
  }
  appLog("已存 WiFi : %d 条（wifi list 查看）\r\n", btWifiCount());
  appLog("-------------------\r\n");
}

static void cmdScan() {
  appLog("[scan] 开始扫描（约 3 秒）…\r\n");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true, false, 300);
  if (n < 0) {
    appLog("[scan] 失败：scanNetworks 返回 %d（WiFi 未开启或射频忙）\r\n", n);
    return;
  }
  if (n == 0) {
    appLog("[scan] 一个 AP 都没扫到：确认路由器是 2.4GHz、附近有信号\r\n");
    return;
  }
  appLog("[scan] 共 %d 个 AP（* = 已保存）\r\n", n);
  appLog("  #  RSSI  CH 加密        保存  SSID\r\n");
  for (int i = 0; i < n; i++) {
    char ssid[40], pass[68];
    bool saved = false;
    for (int k = 0; k < btWifiCount(); k++) {
      if (btWifiGet(k, ssid, sizeof(ssid), pass, sizeof(pass)) && WiFi.SSID(i) == ssid) { saved = true; break; }
    }
    appLog("  %2d %4d  %2d %-12s  %s   %s\r\n",
           i, (int)WiFi.RSSI(i), (int)WiFi.channel(i),
           authName(WiFi.encryptionType(i)), saved ? "*" : " ",
           WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "(隐藏)");
  }
  WiFi.scanDelete();
  appLog("[scan] 完成\r\n");
}

static void cmdList() {
  int n = btWifiCount();
  appLog("[wifi] 已保存 %d 条\r\n", n);
  for (int i = 0; i < n; i++) {
    char s[40], p[68];
    if (btWifiGet(i, s, sizeof(s), p, sizeof(p)))
      appLog("  %d) %-32s 密码%s\r\n", i, s, p[0] ? "已存" : "空");
  }
}

/* 连接：序号或 SSID + 可选密码 */
static void cmdConn(char *arg) {
  if (!arg || !*arg) { appLog("[wifi] 用法: wifi conn <序号|SSID> [密码]\r\n"); return; }

  /* 只拿第一个 token 判断是不是序号。之前扫了整串，于是
   * "0 cooltel123!" 会被当成 SSID "0" —— 这是上一版 connect 失败的真正原因之一。 */
  char first[64] = {0};
  size_t fi = 0;
  for (const char *p = arg; *p && *p != ' ' && fi < sizeof(first) - 1; p++) first[fi++] = *p;
  first[fi] = 0;

  char ssid[40] = {0}, pass[68] = {0};
  bool isNum = (fi > 0);
  for (char *p = first; *p; p++)
    if (*p < '0' || *p > '9') { isNum = false; break; }

  if (isNum) {
    int idx = atoi(first);
    if (!btWifiGet(idx, ssid, sizeof(ssid), pass, sizeof(pass))) {
      appLog("[wifi] 序号 %d 无效（已存 %d 条）\r\n", idx, btWifiCount());
      return;
    }
    appLog("[wifi] 使用已保存的第 %d 条: %s\r\n", idx, ssid);
  } else {
    char *sp = strchr(arg, ' ');
    if (sp) { *sp = 0; snprintf(pass, sizeof(pass), "%s", sp + 1); }
    snprintf(ssid, sizeof(ssid), "%s", arg);
    appLog("[wifi] 使用命令行给定: %s（密码长度 %u）\r\n", ssid, (unsigned)strlen(pass));
  }
  if (!ssid[0]) { appLog("[wifi] SSID 为空\r\n"); return; }

  /* 先扫一眼，把「AP 在不在、信号多少、什么加密」讲清楚 */
  if (WiFi.getMode() != WIFI_MODE_STA) WiFi.mode(WIFI_STA);
  /* 上一次没走完的连接会让 scan 直接返回 -2（WIFI_SCAN_FAILED），先断开再扫 */
  WiFi.disconnect(false);
  delay(150);
  appLog("[wifi] 预扫描，确认目标 AP 可见…\r\n");
  int n = WiFi.scanNetworks(false, true, false, 300);
  if (n < 0) {
    appLog("[wifi] 预扫描返回 %d，等 400ms 重试一次\r\n", n);
    delay(400);
    n = WiFi.scanNetworks(false, true, false, 300);
  }
  bool found = false;
  if (n < 0) {
    appLog("[wifi] 预扫描仍失败 (%d)：射频可能卡住了，建议先 wifi reset\r\n", n);
  } else {
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == ssid) {
        appLog("[wifi] 命中 %s  RSSI=%d dBm  信道=%d  加密=%s\r\n",
               ssid, (int)WiFi.RSSI(i), (int)WiFi.channel(i), authName(WiFi.encryptionType(i)));
        found = true;
      }
    }
    WiFi.scanDelete();
    if (!found)
      appLog("[wifi] 警告：扫描列表里没有 %s（隐藏 SSID / 5GHz / 距离远），仍尝试连接\r\n", ssid);
  }

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.disconnect();
  delay(100);

  if (!gEvtBound) {
    esp_err_t e = bindEvents();
    appLog("[wifi] 事件绑定重试: %s\r\n", e == ESP_OK ? "成功" : esp_err_to_name(e));
  }

  gReasonOk = false;
  gEvStart = gEvConnected = gEvDisconnected = gEvGotIp = 0;
  appLog("[wifi] WiFi.begin(\"%s\") …\r\n", ssid);
  uint32_t t0 = millis();
  bool began = WiFi.begin(ssid, pass);
  appLog("[wifi] begin()=%s  模式=%d  状态=%s(%d)\r\n",
         began ? "true" : "false", (int)WiFi.getMode(),
         statusName(WiFi.status()), (int)WiFi.status());
  if (!began) {
    /* Arduino 把真正的失败原因打在 log_e 里，而 CORE_DEBUG_LEVEL=0 时全被吞掉，
     * 所以这里直接把内部状态倒出来，并让用户复位射频 */
    appLog("[wifi] begin() 直接返回 false：WiFi 栈没起来，不是密码问题\r\n");
    WiFi.printDiag(Serial);
    appLog("[wifi] 处理：先执行 wifi reset，再 wifi conn 重试\r\n");
    return;
  }

  wl_status_t last = WL_IDLE_STATUS;
  uint32_t lastPrint = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) {
    wl_status_t st = WiFi.status();
    uint32_t el = millis() - t0;
    if (st != last || el - lastPrint >= 3000) {
      appLog("[wifi] %5u ms  状态=%s(%d)%s\r\n", (unsigned)el, statusName(st), (int)st,
             (gReasonOk && st != WL_CONNECTED) ? "  已收到断开事件" : "");
      last = st;
      lastPrint = el;
    }
    delay(200);
  }

  uint32_t el = millis() - t0;
  if (WiFi.status() == WL_CONNECTED) {
    appLog("[wifi] 连接成功，耗时 %u ms\r\n", (unsigned)el);
    appLog("[wifi] IP=%s  掩码=%s\r\n", WiFi.localIP().toString().c_str(),
           WiFi.subnetMask().toString().c_str());
    appLog("[wifi] 网关=%s  DNS=%s\r\n", WiFi.gatewayIP().toString().c_str(),
           WiFi.dnsIP().toString().c_str());
    appLog("[wifi] RSSI=%d dBm  信道=%d  BSSID=%s\r\n",
           (int)WiFi.RSSI(), (int)WiFi.channel(), WiFi.BSSIDstr().c_str());
    wifiSetPublicDns();
    appLog("[wifi] 生效 DNS : %s\r\n", WiFi.dnsIP().toString().c_str());
    if (WiFi.dnsIP() == IPAddress(0, 0, 0, 0))
      appLog("[wifi] 警告：DNS 为 0.0.0.0，域名解析会失败（路由器没下发 DNS）\r\n");
    return;
  }

  appLog("[wifi] 连接失败，耗时 %u ms，最终状态=%s(%d)\r\n", (unsigned)el,
         statusName(WiFi.status()), (int)WiFi.status());
  printEventStats();
  /* 注意：预扫描时 STA 已经启动过，begin() 前又清了计数，
   * 所以「START=0」不能推出「射频没启动」——只有连断开事件都没有时才是真的没动作。 */
  if (gEvDisconnected == 0) {
    appLog("[wifi] 连一次断开事件都没收到：射频根本没发起过连接\r\n");
    appLog("[wifi] 处理：wifi reset 复位后再试\r\n");
  } else if (!gReasonOk) {
    appLog("[wifi] 有 STA_START 但没有断开事件：多半还卡在扫描/关联阶段\r\n");
    appLog("[wifi] 检查：SSID 是否 2.4GHz、信号是否够（< -85 基本连不上）\r\n");
  } else {
    appLog("[wifi] 断开原因: %u (%s)\r\n", (unsigned)gReason,
           WiFi.disconnectReasonName((wifi_err_reason_t)gReason));
    appLog("[wifi] 诊断    : %s\r\n", reasonHint(gReason));
  }
}

/* 射频复位：驱动卡死时先来一发，再重连 */
static void cmdReset() {
  appLog("[wifi] 复位射频：disconnect(true) -> mode(OFF) -> 500ms -> mode(STA)\r\n");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
  bool ok = WiFi.mode(WIFI_STA);
  delay(300);
  gReasonOk = false;
  appLog("[wifi] mode(STA) %s，当前模式=%d 状态=%s(%d)\r\n", ok ? "成功" : "失败",
         (int)WiFi.getMode(), statusName(WiFi.status()), (int)WiFi.status());
  printEventStats();
  appLog("[wifi] 现在可以 wifi scan / wifi conn 重试\r\n");
}

/* ---- DNS ---- */
/* lwIP 没提供 gai_strerror，自己给几个常见码配中文 */
static const char *dnsErrText(int rc) {
  if (rc == 0) return "OK";
#ifdef EAI_NONAME
  if (rc == EAI_NONAME) return "域名不存在或 DNS 无响应";
#endif
#ifdef EAI_AGAIN
  if (rc == EAI_AGAIN) return "DNS 暂时不可用";
#endif
#ifdef EAI_FAIL
  if (rc == EAI_FAIL) return "DNS 服务器返回失败";
#endif
#ifdef EAI_MEMORY
  if (rc == EAI_MEMORY) return "解析时内存不足";
#endif
#ifdef EAI_SERVICE
  if (rc == EAI_SERVICE) return "服务名不支持";
#endif
  return "解析失败";
}

static bool resolveHost(const char *host, IPAddress *out, uint32_t *ms, int *rc) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  uint32_t t0 = millis();
  *rc = getaddrinfo(host, NULL, &hints, &res);
  *ms = millis() - t0;
  if (*rc != 0 || !res) return false;
  struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
  *out = IPAddress((uint32_t)sa->sin_addr.s_addr);
  freeaddrinfo(res);
  return true;
}

static void cmdDns(char *arg) {
  if (!arg || !*arg) { appLog("[wifi] 用法: wifi dns <主机>\r\n"); return; }
  if (WiFi.status() != WL_CONNECTED) { appLog("[wifi] 未连接 WiFi，先用 wifi conn 连上\r\n"); return; }
  IPAddress ip; uint32_t ms; int rc;
  if (resolveHost(arg, &ip, &ms, &rc)) {
    appLog("[dns] %s -> %s  (%u ms)\r\n", arg, ip.toString().c_str(), (unsigned)ms);
  } else {
    appLog("[dns] 解析失败 rc=%d (%s) 耗时 %u ms\r\n", rc, dnsErrText(rc), (unsigned)ms);
    appLog("[dns] 检查 DNS 服务器: %s\r\n", WiFi.dnsIP().toString().c_str());
  }
}

/* ---- ping ---- */
static volatile bool gPingDone = false;
static volatile int  gPingRecv = 0;

static void pingSuccess(esp_ping_handle_t hdl, void *args) {
  (void)args;
  uint32_t gap = 0; uint8_t ttl = 0; ip_addr_t addr;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
  esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
  char ip[40] = {0};
  ipaddr_ntoa_r(&addr, ip, sizeof(ip));
  appLog("[ping] 来自 %s 时间=%u ms TTL=%u\r\n", ip, (unsigned)gap, (unsigned)ttl);
  gPingRecv++;
}
static void pingTimeout(esp_ping_handle_t hdl, void *args) {
  (void)hdl; (void)args;
  appLog("[ping] 请求超时\r\n");
}
static void pingEnd(esp_ping_handle_t hdl, void *args) {
  (void)hdl; (void)args;
  gPingDone = true;
}

static void cmdPing(char *arg) {
  if (!arg || !*arg) { appLog("[wifi] 用法: wifi ping <主机> [次数]\r\n"); return; }
  if (WiFi.status() != WL_CONNECTED) { appLog("[wifi] 未连接 WiFi\r\n"); return; }

  char host[80] = {0};
  int count = 4;
  char *sp = strchr(arg, ' ');
  if (sp) { *sp = 0; count = atoi(sp + 1); }
  snprintf(host, sizeof(host), "%s", arg);
  if (count <= 0 || count > 20) count = 4;

  IPAddress ip; uint32_t ms; int rc;
  if (!resolveHost(host, &ip, &ms, &rc)) {
    appLog("[ping] 解析 %s 失败 rc=%d (%s)\r\n", host, rc, dnsErrText(rc));
    return;
  }
  appLog("[ping] %s -> %s，%d 次，每次超时 2s\r\n", host, ip.toString().c_str(), count);

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count = (uint32_t)count;
  cfg.interval_ms = 800;
  cfg.timeout_ms = 2000;
  cfg.task_stack_size = 3072;
  ipaddr_aton(ip.toString().c_str(), &cfg.target_addr);

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_ping_success = pingSuccess;
  cbs.on_ping_timeout = pingTimeout;
  cbs.on_ping_end     = pingEnd;

  gPingDone = false;
  gPingRecv = 0;
  esp_ping_handle_t hdl = NULL;
  esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
  if (err != ESP_OK) {
    appLog("[ping] 创建会话失败: %s\r\n", esp_err_to_name(err));
    return;
  }
  esp_ping_start(hdl);

  uint32_t t0 = millis();
  while (!gPingDone && millis() - t0 < (uint32_t)count * 3000 + 5000) delay(50);
  esp_ping_delete_session(hdl);

  if (!gPingDone) appLog("[ping] 会话未正常结束（超时兜底）\r\n");
  appLog("[ping] 完成：收 %d / 发 %d\r\n", gPingRecv, count);
  if (gPingRecv == 0)
    appLog("[ping] 一个都没回：ICMP 可能被 AP/路由器拦截，试试 wifi curl 走 TCP\r\n");
}

/* ---- curl ---- */
struct Url { bool https; char host[80]; uint16_t port; char path[160]; };

static bool parseUrl(const char *in, Url *u) {
  const char *p = in;
  memset(u, 0, sizeof(*u));
  u->port = 80;
  if (!strncasecmp(p, "https://", 8)) { u->https = true; u->port = 443; p += 8; }
  else if (!strncasecmp(p, "http://", 7)) { p += 7; }

  const char *slash = strchr(p, '/');
  size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
  if (hlen == 0 || hlen >= sizeof(u->host)) return false;
  memcpy(u->host, p, hlen);
  u->host[hlen] = 0;

  char *colon = strrchr(u->host, ':');
  if (colon) {
    int pr = atoi(colon + 1);
    if (pr > 0 && pr < 65536) { u->port = (uint16_t)pr; *colon = 0; }
  }
  snprintf(u->path, sizeof(u->path), "%s", slash ? slash : "/");
  return u->host[0] != 0;
}

static void cmdCurl(char *arg) {
  if (!arg || !*arg) { appLog("[wifi] 用法: wifi curl [-I] <http(s)://主机/路径>\r\n"); return; }
  if (WiFi.status() != WL_CONNECTED) { appLog("[wifi] 未连接 WiFi\r\n"); return; }

  bool headOnly = false;
  char *u = arg;
  while (*u == ' ') u++;
  if (!strncasecmp(u, "-I", 2)) { headOnly = true; u += 2; while (*u == ' ') u++; }
  if (!*u) { appLog("[wifi] 缺少 URL\r\n"); return; }

  Url url;
  if (!parseUrl(u, &url)) { appLog("[wifi] URL 解析失败: %s\r\n", u); return; }
  appLog("[curl] %s %s://%s:%u%s\r\n", headOnly ? "HEAD" : "GET",
         url.https ? "https" : "http", url.host, (unsigned)url.port, url.path);

  IPAddress ip; uint32_t ms; int rc;
  if (!resolveHost(url.host, &ip, &ms, &rc)) {
    appLog("[curl] DNS 解析失败 rc=%d (%s)\r\n", rc, dnsErrText(rc));
    appLog("[curl] 检查 DNS=%s 或直接换成 IP 试试\r\n", WiFi.dnsIP().toString().c_str());
    return;
  }
  appLog("[curl] DNS %s -> %s  (%u ms)\r\n", url.host, ip.toString().c_str(), (unsigned)ms);

  NetworkClient plain;
  NetworkClientSecure secure;
  NetworkClient *c = &plain;
  if (url.https) {
    secure.setInsecure();               /* 设备没有证书库，调试命令不校验证书 */
    secure.setHandshakeTimeout(12);
    c = &secure;
  }

  uint32_t t0 = millis();
  bool ok = c->connect(url.host, url.port);
  uint32_t tcpMs = millis() - t0;
  if (!ok) {
    char errbuf[96] = {0};
    if (url.https) secure.lastError(errbuf, sizeof(errbuf));
    appLog("[curl] TCP 连接失败 (%u ms) errno=%d (%s)\r\n", (unsigned)tcpMs, errno, strerror(errno));
    if (errbuf[0]) appLog("[curl] TLS/底层错误: %s\r\n", errbuf);
    if (errno == ECONNREFUSED) appLog("[curl] 端口没开或被拒，换 80/443 或确认服务在跑\r\n");
    if (errno == EHOSTUNREACH) appLog("[curl] 路由不可达：网关=%s 是否正确\r\n", WiFi.gatewayIP().toString().c_str());
    return;
  }
  appLog("[curl] %s 连接成功 (%u ms)%s\r\n", url.https ? "TLS" : "TCP", (unsigned)tcpMs,
         url.https ? "（证书未校验）" : "");

  char req[320];
  snprintf(req, sizeof(req),
           "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: oled-c3/1.0\r\n"
           "Accept: */*\r\nConnection: close\r\n\r\n",
           headOnly ? "HEAD" : "GET", url.path, url.host);
  uint32_t t1 = millis();
  c->write((const uint8_t *)req, strlen(req));

  while (!c->available() && c->connected() && millis() - t1 < 15000) delay(20);
  if (!c->available()) {
    char errbuf[96] = {0};
    if (url.https) secure.lastError(errbuf, sizeof(errbuf));
    appLog("[curl] 15s 内没收到任何响应%s%s\r\n", errbuf[0] ? " " : "", errbuf);
    c->stop();
    return;
  }
  appLog("[curl] 首字节 %u ms\r\n", (unsigned)(millis() - t1));

  String status = c->readStringUntil('\n');
  status.trim();
  appLog("[curl] %s\r\n", status.c_str());

  int hdr = 0;
  while (c->connected() || c->available()) {
    String line = c->readStringUntil('\n');
    if (line.length() == 0 || line == "\r") break;
    if (++hdr > 40) break;
    if (headOnly || hdr <= 6) { line.trim(); appLog("[curl]   %s\r\n", line.c_str()); }
  }
  if (hdr > 6 && !headOnly) appLog("[curl]   …（其余 %d 行响应头已省略）\r\n", hdr - 6);

  size_t total = 0;
  char   head[257];
  size_t headN = 0;
  uint32_t t2 = millis();
  while (millis() - t2 < 20000) {
    while (c->available()) {
      int ch = c->read();
      if (ch < 0) break;
      total++;
      if (headN < 256) {
        head[headN++] = (ch >= 32 && ch < 127) || ch == '\n' || ch == '\r' ? (char)ch : '.';
      }
    }
    if (!c->connected() && !c->available()) break;
    delay(10);
  }
  c->stop();
  head[headN] = 0;
  appLog("[curl] 正文 %u 字节，总耗时 %u ms\r\n", (unsigned)total, (unsigned)(millis() - t0));
  if (!headOnly && headN) {
    appLog("[curl] 正文前 %u 字节:\r\n", (unsigned)headN);
    appLog("%s\r\n", head);
  }
}

/* ---- ntp ---- */
static void cmdNtp(char *arg) {
  char srv[68] = "ntp.aliyun.com";
  if (arg && *arg) snprintf(srv, sizeof(srv), "%s", arg);
  if (WiFi.status() != WL_CONNECTED) { appLog("[wifi] 未连接 WiFi\r\n"); return; }

  IPAddress ip; uint32_t ms; int rc;
  if (!resolveHost(srv, &ip, &ms, &rc)) {
    appLog("[ntp] DNS 解析 %s 失败 rc=%d (%s)\r\n", srv, rc, dnsErrText(rc));
    return;
  }
  appLog("[ntp] %s -> %s (%u ms)\r\n", srv, ip.toString().c_str(), (unsigned)ms);

  WiFiUDP udp;
  if (!udp.begin(0)) { appLog("[ntp] UDP 绑定本地端口失败\r\n"); return; }

  uint8_t pkt[48];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x1B;   /* LI=0 VN=3 Mode=3(client) */
  uint32_t t0 = millis();
  udp.beginPacket(ip, 123);
  udp.write(pkt, sizeof(pkt));
  if (!udp.endPacket()) { appLog("[ntp] 发送失败\r\n"); udp.stop(); return; }

  uint32_t t1 = millis();
  int len = 0;
  while (millis() - t1 < 5000) {
    len = udp.parsePacket();
    if (len >= 48) break;
    delay(20);
  }
  if (len < 48) {
    appLog("[ntp] 5s 无有效应答（收到 %d 字节）\r\n", len);
    appLog("[ntp] 常见原因：路由器/运营商封 UDP 123、DNS 指到了不可用的服务器\r\n");
    udp.stop();
    return;
  }
  uint8_t buf[48];
  udp.read(buf, sizeof(buf));
  uint32_t rtt = millis() - t0;

  uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                  ((uint32_t)buf[42] << 8) | buf[43];
  uint32_t frac = ((uint32_t)buf[44] << 24) | ((uint32_t)buf[45] << 16) |
                  ((uint32_t)buf[46] << 8) | buf[47];
  time_t utc = (time_t)((int64_t)secs - 2208988800LL);

  struct tm tmv;
  gmtime_r(&utc, &tmv);
  appLog("[ntp] 应答 %d 字节  RTT=%u ms  层数=%u  精度=%d\r\n",
         len, (unsigned)rtt, (unsigned)buf[1], (int8_t)buf[3]);
  appLog("[ntp] UTC  %04d-%02d-%02d %02d:%02d:%02d\r\n",
         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  uint32_t rtcTs = 0;
  if (appRtcReadUnix(&rtcTs))
    appLog("[ntp] RTC  比 NTP %s %ld 秒\r\n",
           rtcTs < (uint32_t)utc ? "慢" : "快",
           (long)((int32_t)rtcTs - (int32_t)utc));
  (void)frac;
  udp.stop();
}

/* ==================== 分发 ==================== */
void wifiDbgHelp() {
  appLog("\r\nWiFi 调试命令：\r\n"
         "  wifi on                       打开 STA（不连接）\r\n"
         "  wifi off                      关闭射频\r\n"
         "  wifi reset                    射频卡住时复位（关->开 STA）\r\n"
         "  wifi status                   模式/状态/IP/信号/最近断开原因\r\n"
         "  wifi scan                     扫描周边 AP（* 表示已保存）\r\n"
         "  wifi list                     列出已保存的 WiFi\r\n"
         "  wifi conn <序号|SSID> [密码]   连接并打印每一步状态与失败原因\r\n"
         "  wifi ping <主机> [次数]        ICMP ping，默认 4 次\r\n"
         "  wifi curl [-I] <URL>           HTTP(S) 请求，打印 DNS/连接/首字节/状态码\r\n"
         "  wifi dns <主机>                只做域名解析，看 DNS 通不通\r\n"
         "  wifi ntp [服务器]              单次 SNTP 查询，对比 RTC 快慢\r\n"
         "  说明：命令在独立任务执行，执行期间串口其他输出会穿插\r\n\r\n");
}

static void dispatch(char *line) {
  while (*line == ' ') line++;
  char *sp = strchr(line, ' ');
  if (sp) { *sp = 0; sp++; while (*sp == ' ') sp++; }
  char *arg = sp;

  if (!*line || !strcasecmp(line, "help")) wifiDbgHelp();
  else if (!strcasecmp(line, "on"))     cmdOn();
  else if (!strcasecmp(line, "off"))    cmdOff();
  else if (!strcasecmp(line, "reset"))  cmdReset();
  else if (!strcasecmp(line, "status")) cmdStatus();
  else if (!strcasecmp(line, "scan"))   cmdScan();
  else if (!strcasecmp(line, "list"))   cmdList();
  else if (!strcasecmp(line, "conn") || !strcasecmp(line, "connect")) cmdConn(arg);
  else if (!strcasecmp(line, "ping"))   cmdPing(arg);
  else if (!strcasecmp(line, "curl"))   cmdCurl(arg);
  else if (!strcasecmp(line, "dns"))    cmdDns(arg);
  else if (!strcasecmp(line, "ntp"))    cmdNtp(arg);
  else {
    appLog("[wifi] 未知子命令: %s\r\n", line);
    wifiDbgHelp();
  }
}

struct CmdJob { char line[192]; };
static CmdJob gJob;

static void wifiTask(void *arg) {
  (void)arg;
  char line[192];
  strcpy(line, gJob.line);
  dispatch(line);
  gBusy = false;
  vTaskDelete(NULL);
}

bool wifiDbgRun(const char *args) {
  if (gBusy) { appLog("[wifi] 上一条命令还在跑，稍等（或看 wifi status）\r\n"); return true; }
  snprintf(gJob.line, sizeof(gJob.line), "%s", args ? args : "");
  gBusy = true;
  if (xTaskCreate(wifiTask, "wifidbg", 10240, NULL, 3, NULL) != pdPASS) {
    gBusy = false;
    appLog("[wifi] 创建任务失败：内存不足\r\n");
  }
  return true;
}
