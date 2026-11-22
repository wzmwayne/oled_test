/*
 * wifiap.cpp - WiFi 配置热点 + 网页配置
 * 见 wifiap.h。设备只做 AP，从不连路由器。
 */
#include "wifiap.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include "btconf.h"
#include "reader.h"
#include "timetable.h"

extern void appLog(const char *fmt, ...);
extern bool appRtcWriteUnix(uint32_t ts);
extern bool appRtcReadUnix(uint32_t *ts);

#define WA_BOOK_DIR "/books"
#define WA_CHANNEL   1      /* 1/6/11 里挑最靠前的，兼容性最好 */

static WebServer *gWeb = nullptr;
static bool       gOn  = false;
static char       gSsid[24] = "";
static char       gPass[12] = "";
static char       gLast[48] = "";
static uint32_t   gUpBytes = 0;
static int        gUpFiles = 0;
static int        gChan = 6;      /* 1/6/11 互不重叠，默认 6 */
static uint8_t    gProto = 0;     /* 0=用驱动默认(11b/g/n)，否则显式设协议 */
static bool       gOpen = false;   /* 开放网络（无密码）诊断用 */

/* 上传中的临时状态 */
static File     gUpFile;
static char     gUpName[40] = "";
static bool     gUpBad = true;
static uint32_t gUpOkSize = 0;

static const char IDX_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OLED 时钟 · 配置</title><style>
*{box-sizing:border-box}body{margin:0;background:#111;color:#eee;font:15px/1.5 system-ui,-apple-system,"PingFang SC",sans-serif}
header{padding:14px 16px 6px;font-weight:600;font-size:17px}
nav{display:flex;gap:6px;padding:6px 12px 12px}
nav button{flex:1;padding:9px 0;border:1px solid #333;background:#1b1b1b;color:#eee;border-radius:8px}
nav button.on{background:#3b82f6;border-color:#3b82f6;color:#fff}
main{padding:0 12px 40px}
.card{background:#1b1b1b;border:1px solid #2a2a2a;border-radius:10px;padding:12px;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:7px 0;border-bottom:1px solid #262626}
.row:last-child{border-bottom:none}.name{font-weight:600}.sub{color:#999;font-size:12px}
input,textarea{width:100%;padding:9px;margin:6px 0;background:#0d0d0d;color:#eee;border:1px solid #333;border-radius:8px;font:inherit}
button.act{padding:10px 14px;background:#3b82f6;color:#fff;border:none;border-radius:8px;font-weight:600;margin-right:8px}
button.ghost{background:transparent;border:1px solid #444;color:#eee;padding:8px 12px;border-radius:8px;margin-right:8px}
button.danger{background:transparent;border:1px solid #ef4444;color:#ef4444;padding:6px 10px;border-radius:8px}
.bar{height:6px;background:#333;border-radius:3px;overflow:hidden;margin-top:10px}
.bar i{display:block;height:100%;width:0;background:#3b82f6;transition:width .15s}
#msg{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#000d;padding:9px 16px;border-radius:8px;opacity:0;transition:.2s;pointer-events:none}
#msg.on{opacity:1}
</style></head><body>
<header>OLED 时钟 · 配置</header>
<nav id="nav"><button data-t="0" class="on">书库</button><button data-t="1">课表</button><button data-t="2">时间</button><button data-t="3">设备</button></nav>
<main>
<section id="p0"><div class="card">
<div class="row"><span class="name">上传到设备</span><span class="sub" id="free">—</span></div>
<input type="file" id="file" accept=".txt,.ezbk,.ezzbk">
<div class="sub">.txt / .ezbk / .ezzbk 原样存进设备，不转码。</div>
<button class="act" id="up">开始上传</button>
<div class="bar"><i id="pb"></i></div>
</div><div class="card" id="books"><div class="sub">读取中…</div></div></section>
<section id="p1" hidden><div class="card">
<div class="row"><span class="name">课表 JSON</span><span class="sub" id="ttinfo">—</span></div>
<textarea id="tt" rows="12" spellcheck="false"></textarea>
<div class="sub">n=课名 d=星期1..7 s/e=距0点的分钟（也可写 08:00）p=0每周 1单周 2双周</div>
<button class="act" id="ttsave">整体覆盖</button>
<button class="ghost" id="ttload">读取当前</button>
<button class="ghost" id="ttflip">单双周互换</button>
</div></section>
<section id="p2" hidden><div class="card">
<div class="row"><span class="name">设备时间</span><span class="sub" id="rtc">—</span></div>
<input type="datetime-local" id="dt" step="1">
<button class="act" id="tset">写入这个时间</button>
<button class="ghost" id="tnow">用手机当前时间</button>
</div></section>
<section id="p3" hidden><div class="card">
<div class="row"><span>热点</span><span class="sub" id="i_ssid">—</span></div>
<div class="row"><span>已连设备</span><span class="sub" id="i_cli">—</span></div>
<div class="row"><span>空闲内存</span><span class="sub" id="i_heap">—</span></div>
<div class="row"><span>存储</span><span class="sub" id="i_fs">—</span></div>
<div class="row"><span>课表</span><span class="sub" id="i_tt">—</span></div>
<div class="row"><span>时区</span><span class="sub" id="i_tz">—</span></div>
</div></section>
</main><div id="msg"></div>
<script>
var $=function(i){return document.getElementById(i)};
function toast(t){var m=$('msg');m.textContent=t;m.className='on';clearTimeout(m._t);m._t=setTimeout(function(){m.className=''},2000)}
function fmt(n){return n>=1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(0)+' KB'}
function api(u,o){return fetch(u,o||{}).then(function(r){return r.json()})}
var nav=$('nav');
nav.addEventListener('click',function(e){var b=e.target.closest('button');if(!b)return;var t=b.dataset.t;
[].forEach.call(nav.children,function(x){x.className=(x===b)?'on':''});
[0,1,2,3].forEach(function(i){$('p'+i).hidden=(String(i)!==t)});
if(t==='0')loadBooks();if(t==='1')loadTt();if(t==='2')syncRtc();if(t==='3')loadInfo()});
function loadBooks(){api('/api/books').then(function(d){$('free').textContent='剩余 '+fmt(d.free);
var box=$('books');box.innerHTML='';
if(!d.books.length){box.innerHTML='<div class="sub">还没有书</div>';return}
d.books.forEach(function(b){var r=document.createElement('div');r.className='row';
var l=document.createElement('div');var p=document.createElement('div');p.className='name';p.textContent=b.n;
var s=document.createElement('div');s.className='sub';s.textContent=fmt(b.s)+' · 已读 '+b.p+'%';
l.appendChild(p);l.appendChild(s);
var btn=document.createElement('button');btn.className='danger';btn.textContent='删除';
btn.onclick=function(){if(!confirm('删除 '+b.n+' ?'))return;
api('/api/book/del',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(b.n)}).then(function(x){toast(x.ok?'已删除':x.msg);loadBooks()})};
r.appendChild(l);r.appendChild(btn);box.appendChild(r)})})}
$('up').onclick=function(){var f=$('file').files[0];if(!f){toast('先选文件');return}
var xhr=new XMLHttpRequest();xhr.open('POST','/api/book');
var fd=new FormData();fd.append('f',f,f.name);
xhr.upload.onprogress=function(e){if(e.lengthComputable)$('pb').style.width=(e.loaded*100/e.total).toFixed(1)+'%'};
xhr.onload=function(){$('pb').style.width='0';var r={};try{r=JSON.parse(xhr.responseText)}catch(_){}
toast(r.ok?('已上传 '+r.name):(r.msg||'上传失败'));loadBooks()};
xhr.onerror=function(){toast('传输中断');$('pb').style.width='0'};
xhr.send(fd)};
function loadTt(){fetch('/api/tt').then(function(r){return r.text()}).then(function(t){$('tt').value=t||'';
try{var d=JSON.parse(t);$('ttinfo').textContent=((d.c||[]).length)+' 条'}catch(_){$('ttinfo').textContent='空'}})}
$('ttload').onclick=loadTt;
$('ttsave').onclick=function(){var v=$('tt').value.trim();if(!v){toast('内容为空');return}
api('/api/tt',{method:'POST',headers:{'Content-Type':'application/json'},body:v}).then(function(r){toast(r.ok?('已写入 '+r.count+' 条'):r.msg);loadTt()})};
$('ttflip').onclick=function(){api('/api/tt/flip',{method:'POST'}).then(function(r){toast('已互换 '+(r.count||0)+' 条');loadTt()})};
function syncRtc(){api('/api/info').then(function(d){$('rtc').textContent=d.rtc_ok?new Date(d.rtc*1000).toLocaleString():'读取失败'})}
function pad(n){return (n<10?'0':'')+n}
$('tnow').onclick=function(){var d=new Date();
$('dt').value=d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'T'+pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds())};
$('tset').onclick=function(){var v=$('dt').value;if(!v){toast('先选时间');return}
var ts=Math.floor(new Date(v).getTime()/1000);
api('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ts='+ts}).then(function(r){toast(r.ok?'已写入':r.msg);syncRtc()})};
function loadInfo(){api('/api/info').then(function(d){
$('i_ssid').textContent=d.ssid+' · '+d.ip;$('i_cli').textContent=d.clients+' 台';
$('i_heap').textContent=(d.heap/1024).toFixed(0)+' KB';
$('i_fs').textContent=fmt(d.fs_used)+' / '+fmt(d.fs_total)+'（余 '+fmt(d.fs_free)+'）';
$('i_tt').textContent=d.tt_count+' 条';$('i_tz').textContent='UTC'+(d.tz>=0?'+':'')+d.tz})}
setInterval(function(){if(!$('p3').hidden)loadInfo()},5000);
loadBooks();
</script></body></html>)HTML";

static void waMsg(const char *m) { snprintf(gLast, sizeof(gLast), "%s", m); }

static size_t fsFree() {
  size_t t = LittleFS.totalBytes(), u = LittleFS.usedBytes();
  return t > u ? t - u : 0;
}

static void waMakePass() {
  static const char cs[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  for (int i = 0; i < 8; i++) gPass[i] = cs[esp_random() % (sizeof(cs) - 1)];
  gPass[8] = 0;
}

/* SSID 跟着密码走：密码一换 SSID 就换。
 * 手机是按 SSID 记住密码的，SSID 不变的话它会拿上一次的旧密码去连，
 * 于是表现为「扫到了但连接被拒绝」。换名等于让手机当成新网络重新输入。 */
static void waMakeSsid() {
  snprintf(gSsid, sizeof(gSsid), "OLED-%c%c%c%c", gPass[0], gPass[1], gPass[2], gPass[3]);
}

/* 只收简单文件名，且必须是阅读器认得的扩展名 */
static bool waNameOk(const char *n) {
  if (!n || !*n) return false;
  size_t len = strlen(n);
  if (len > 32) return false;
  for (const char *p = n; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':') return false;
  if (len > 4 && !strcasecmp(n + len - 4, ".txt"))   return true;
  if (len > 6 && !strcasecmp(n + len - 6, ".ezzbk")) return true;
  if (len > 5 && !strcasecmp(n + len - 5, ".ezbk"))  return true;
  return false;
}

static int bookPercent(const char *name) {
  for (int i = 0; i < rdBookCount(); i++)
    if (!strcmp(rdBookName(i), name)) return rdBookPercent(i);
  return 0;
}

/* AP 事件：手机连不上时，串口能看出是没关联还是关联后掉线 */
static void waOnApEvent(WiFiEvent_t e) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_AP_START:           appLog("[AP] 热点已启动\r\n"); break;
    case ARDUINO_EVENT_WIFI_AP_STOP:            appLog("[AP] 热点已停止\r\n"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:    appLog("[AP] 手机已关联\r\n"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: appLog("[AP] 手机断开\r\n"); break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:   appLog("[AP] 已分配 IP\r\n"); break;
    default: break;
  }
}

static void sendJson(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  gWeb->sendHeader("Cache-Control", "no-store");
  gWeb->send(200, "application/json; charset=utf-8", out);
}

/* ==================== 接口 ==================== */

static void hIndex() { gWeb->send_P(200, "text/html; charset=utf-8", IDX_HTML); }

static void hInfo() {
  JsonDocument doc;
  uint32_t ts = 0;
  bool rtcOk = appRtcReadUnix(&ts);
  doc["ssid"]     = gSsid;
  doc["ip"]       = WiFi.softAPIP().toString();
  doc["clients"]  = WiFi.softAPgetStationNum();
  doc["heap"]     = (uint32_t)ESP.getFreeHeap();
  doc["fs_total"] = (uint32_t)LittleFS.totalBytes();
  doc["fs_used"]  = (uint32_t)LittleFS.usedBytes();
  doc["fs_free"]  = (uint32_t)fsFree();
  doc["tt_count"] = ttCount();
  doc["tz"]       = btCfgGetTz();
  doc["rtc"]      = rtcOk ? (uint32_t)ts : 0;
  doc["rtc_ok"]   = rtcOk;
  doc["up_bytes"] = gUpBytes;
  doc["up_files"] = gUpFiles;
  sendJson(doc);
}

static void hBooks() {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  if (!LittleFS.exists(WA_BOOK_DIR)) LittleFS.mkdir(WA_BOOK_DIR);
  File dir = LittleFS.open(WA_BOOK_DIR);
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        const char *nm = f.name();
        const char *slash = strrchr(nm, '/');
        nm = slash ? slash + 1 : nm;
        JsonObject o = arr.add<JsonObject>();
        o["n"] = nm;
        o["s"] = (uint32_t)f.size();
        o["p"] = bookPercent(nm);
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }
  doc["free"] = (uint32_t)fsFree();
  sendJson(doc);
}

/* 上传：multipart 带 filename，直接流式写 LittleFS */
static void hBookUpload() {
  HTTPUpload &up = gWeb->upload();
  if (up.status == UPLOAD_FILE_START) {
    gUpBytes = 0;
    gUpOkSize = 0;
    gUpBad = true;
    snprintf(gUpName, sizeof(gUpName), "%s", up.filename.c_str());
    if (!waNameOk(gUpName)) { waMsg("文件名不合法"); return; }
    size_t need = (size_t)gWeb->clientContentLength();
    if (need > fsFree()) { waMsg("空间不足"); return; }
    if (!LittleFS.exists(WA_BOOK_DIR)) LittleFS.mkdir(WA_BOOK_DIR);
    char path[64];
    snprintf(path, sizeof(path), WA_BOOK_DIR "/%s", gUpName);
    gUpFile = LittleFS.open(path, "w");
    if (!gUpFile) { waMsg("无法创建文件"); return; }
    gUpBad = false;

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gUpBad || !gUpFile) return;
    size_t w = gUpFile.write(up.buf, up.currentSize);
    if (w != up.currentSize) { gUpBad = true; waMsg("写入失败（空间不足）"); return; }
    gUpBytes += (uint32_t)w;

  } else if (up.status == UPLOAD_FILE_END) {
    if (gUpFile) gUpFile.close();
    if (!gUpBad && gUpBytes > 0) {
      gUpOkSize = gUpBytes;
      gUpFiles++;
      rdForget(gUpName);        /* 同名重传，旧进度作废 */
      rdScan();
      char b[48];
      snprintf(b, sizeof(b), "已接收 %s", gUpName);
      waMsg(b);
      appLog("[AP] 上传 %s  %u B\r\n", gUpName, (unsigned)gUpBytes);
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (gUpFile) gUpFile.close();
    char path[64];
    snprintf(path, sizeof(path), WA_BOOK_DIR "/%s", gUpName);
    LittleFS.remove(path);
    gUpBad = true;
    waMsg("传输中断");
  }
}

static void hBookPost() {
  JsonDocument doc;
  if (gUpBad || gUpOkSize == 0) {
    doc["ok"] = false;
    doc["msg"] = gLast[0] ? gLast : "上传失败";
  } else {
    doc["ok"] = true;
    doc["name"] = gUpName;
    doc["size"] = gUpOkSize;
  }
  sendJson(doc);
}

static void hBookDel() {
  JsonDocument doc;
  const char *name = gWeb->arg("name").c_str();
  if (!waNameOk(name)) {
    doc["ok"] = false;
    doc["msg"] = "文件名不合法";
  } else {
    char path[64];
    snprintf(path, sizeof(path), WA_BOOK_DIR "/%s", name);
    bool ok = LittleFS.remove(path);
    if (ok) { rdForget(name); rdScan(); }
    doc["ok"] = ok;
    doc["msg"] = ok ? "已删除" : "文件不存在";
  }
  sendJson(doc);
}

static void hTtGet() {
  if (!LittleFS.exists("/tt.json")) { gWeb->send(200, "text/plain; charset=utf-8", ""); return; }
  File f = LittleFS.open("/tt.json", "r");
  if (!f) { gWeb->send(500, "text/plain", "open fail"); return; }
  gWeb->sendHeader("Cache-Control", "no-store");
  gWeb->streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

static void hTtPost() {
  JsonDocument doc;
  String body = gWeb->arg("plain");
  int n = body.length() ? ttLoadJson(body.c_str(), body.length()) : -1;
  if (n < 0) { doc["ok"] = false; doc["msg"] = "JSON 解析失败或格式不对"; }
  else { doc["ok"] = true; doc["count"] = n; doc["msg"] = "课表已整体写入"; }
  sendJson(doc);
}

static void hTtFlip() {
  JsonDocument doc;
  int n = ttFlipParity();
  doc["ok"] = true;
  doc["count"] = n;
  sendJson(doc);
}

static void hTime() {
  JsonDocument doc;
  uint32_t ts = (uint32_t)strtoul(gWeb->arg("ts").c_str(), nullptr, 10);
  if (ts == 0) { doc["ok"] = false; doc["msg"] = "ts 无效"; }
  else if (appRtcWriteUnix(ts)) { doc["ok"] = true; doc["msg"] = "时间已写入"; }
  else { doc["ok"] = false; doc["msg"] = "写 RTC 失败"; }
  sendJson(doc);
}

/* ==================== 开关 ==================== */

/* 这里刻意不再调用 esp_wifi_set_config / esp_wifi_set_bandwidth：
 * 起完 AP 再改配置，驱动会把接口 Stop/Start 好几遍（串口日志里能看到抖动），
 * 热点容易停在一个半启动状态——手机既扫不到也连不上。
 * 所有参数一次交给 softAP()。 */
static void waStart(bool newPass) {
  if (gOn) return;
  if (newPass || !gPass[0]) { waMakePass(); waMakeSsid(); }

  gUpBytes  = 0;
  gUpFiles  = 0;
  gUpOkSize = 0;
  gUpBad    = true;
  waMsg(gOpen ? "开放热点，无密码" : "等待浏览器");

  btEnd();                       /* 单射频芯片：蓝牙先让开 */
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  delay(50);
  static bool evHooked = false;
  if (!evHooked) { WiFi.onEvent(waOnApEvent); evHooked = true; }

  WiFi.setSleep(false);
  /* 发射功率拉满：单位 0.25dBm，84=21dBm。C3 芯片上限就是 21dBm，
   * 驱动会按芯片能力夹一次（读回来通常是 20dBm），这里给到最大请求值。 */
  esp_wifi_set_max_tx_power(84);
  { int8_t p = 0; esp_wifi_get_max_tx_power(&p);
    appLog("[AP] 发射功率 %d (%.2f dBm)\r\n", (int)p, p * 0.25); }

  bool ok = false;
  for (int i = 0; i < 2 && !ok; i++) {
    ok = gOpen
       ? WiFi.softAP(gSsid, NULL, gChan, 0, 4, false, WIFI_AUTH_OPEN,    WIFI_CIPHER_TYPE_NONE)
       : WiFi.softAP(gSsid, gPass, gChan, 0, 4, false, WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);
    if (!ok) {
      appLog("[AP] softAP 第 %d 次失败，复位射频重试\r\n", i + 1);
      WiFi.mode(WIFI_OFF);
      delay(250);
      WiFi.mode(WIFI_AP);
      delay(80);
    }
  }
  if (!ok) {
    waMsg("热点启动失败");
    appLog("[AP] softAP 失败\r\n");
    return;
  }

  /* 起完再对一次账：驱动偶尔会停在 NULL/STA 模式——softAP 返回成功、
   * 事件也报 AP_START，但 beacon 一个都不发，手机完全扫不到。 */
  {
    wifi_mode_t wm = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wm);
    uint8_t chk = 0;
    wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&chk, &sc);
    appLog("[AP] 模式=%d(2=AP) 信道=%u\r\n", (int)wm, (unsigned)chk);
    if (wm != WIFI_MODE_AP) {
      appLog("[AP] 模式不对，强制重设并重启接口\r\n");
      esp_wifi_set_mode(WIFI_MODE_AP);
      esp_wifi_start();
      delay(100);
      esp_wifi_get_mode(&wm);
      esp_wifi_get_channel(&chk, &sc);
      appLog("[AP] 重设后 模式=%d 信道=%u\r\n", (int)wm, (unsigned)chk);
    }
  }

  if (gProto) {
    esp_wifi_set_protocol(WIFI_IF_AP, gProto);
    appLog("[AP] 协议已设为 0x%02X\r\n", (unsigned)gProto);
  }

  gWeb = new WebServer(80);
  gWeb->on("/",             HTTP_GET,  hIndex);
  gWeb->on("/api/info",     HTTP_GET,  hInfo);
  gWeb->on("/api/books",    HTTP_GET,  hBooks);
  gWeb->on("/api/book",     HTTP_POST, hBookPost, hBookUpload);
  gWeb->on("/api/book/del", HTTP_POST, hBookDel);
  gWeb->on("/api/tt",       HTTP_GET,  hTtGet);
  gWeb->on("/api/tt",       HTTP_POST, hTtPost);
  gWeb->on("/api/tt/flip",  HTTP_POST, hTtFlip);
  gWeb->on("/api/time",     HTTP_POST, hTime);
  gWeb->begin();

  gOn = true;
  appLog("[AP] 热点 %s %s 信道 %d  http://%s\r\n", gSsid,
         gOpen ? "(开放)" : gPass, gChan, WiFi.softAPIP().toString().c_str());
}

void waBegin() { waStart(true); }

/* 换信道 / 切开放网络：保留当前密码，省得手机上重新输 */
void waSetChannel(int ch) {
  if (ch < 1 || ch > 13 || ch == gChan) return;
  gChan = ch;
  waEnd();
  waStart(false);
}

void waSetOpen(bool open) {
  if (open == gOpen) return;
  gOpen = open;
  waEnd();
  waStart(false);
}

int  waChannel() { return gChan; }
bool waIsOpen()  { return gOpen; }

/* 只开 11b / 11b+g：OFDM 发射有问题时，低速 DSSS 的 beacon 往往还能出来 */
void waSetProto(uint8_t proto) {
  if (proto == gProto) return;
  gProto = proto;
  waEnd();
  waStart(false);
}

void waEnd() {
  if (!gOn) return;
  gOn = false;
  if (gUpFile) gUpFile.close();
  if (gWeb) { gWeb->stop(); delete gWeb; gWeb = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  appLog("[AP] 已关闭热点\r\n");
}

void waPoll() {
  if (gOn && gWeb) gWeb->handleClient();
}

/* 串口 ap 命令用：把热点实际状态打出来 */
void waPrintInfo() {
  if (!gOn) { appLog("[AP] 未开启\r\n"); return; }
  /* 注意：esp_wifi_get_channel 的两个参数都不能是 NULL，
   * 传 NULL 会直接返回 INVALID_ARG，通道号保持初值 0（看上去像没设信道）。 */
  uint8_t chan = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_err_t rc = esp_wifi_get_channel(&chan, &sec);
  int8_t pwr = 0;
  esp_wifi_get_max_tx_power(&pwr);
  uint8_t proto = 0;
  esp_wifi_get_protocol(WIFI_IF_AP, &proto);
  wifi_bandwidth_t bw = WIFI_BW_HT20;
  esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);

  appLog("[AP] SSID=\"%s\" %s MAC=%s\r\n", gSsid,
         gOpen ? "开放(无密码)" : gPass, WiFi.softAPmacAddress().c_str());
  appLog("[AP] IP=%s 已连 %d 台\r\n", WiFi.softAPIP().toString().c_str(),
         (int)WiFi.softAPgetStationNum());
  appLog("[AP] 信道=%u(rc=%d 期望%u) 次信道=%d 带宽=%s 协议=0x%02X 功率=%.2fdBm\r\n",
         (unsigned)chan, (int)rc, (unsigned)gChan, (int)sec,
         bw == WIFI_BW_HT40 ? "HT40" : "HT20", (unsigned)proto, pwr * 0.25);
  appLog("[AP] 已传 %d 个文件 %u 字节，剩余空间 %u\r\n", gUpFiles,
         (unsigned)gUpBytes, (unsigned)fsFree());
  appLog("[AP] 芯片 rev=%u  flash=%uKB  空闲堆=%uB\r\n",
         (unsigned)ESP.getChipRevision(), (unsigned)(ESP.getFlashChipSize() / 1024),
         (unsigned)ESP.getFreeHeap());
  appLog("[AP] 提示：如果手机扫不到，试试 ap ch 6 / ap ch 11，或用 ap open 排除密码因素\r\n");
}

bool waRunning() { return gOn; }
const char *waSsid() { return gSsid; }
const char *waPass() { return gOpen ? "" : gPass; }
const char *waIp() {
  static char ip[16] = "192.168.4.1";
  if (gOn) snprintf(ip, sizeof(ip), "%s", WiFi.softAPIP().toString().c_str());
  return ip;
}
int waClients() { return gOn ? WiFi.softAPgetStationNum() : 0; }
uint32_t waUpBytes() { return gUpBytes; }
int waUpFiles() { return gUpFiles; }
const char *waLast() { return gLast; }
