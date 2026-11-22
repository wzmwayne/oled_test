/*
 * ============================================================
 *  桌面时钟 / 课表终端
 *  ESP32-C3 SuperMini + SSD1306 128x64 + DS3231 + 4键
 * ------------------------------------------------------------
 *  按键   K1 = * (GPIO5)   K2 = 0 (GPIO10)
 *         K3 = 1 (GPIO20)  K4 = # (GPIO4)
 *
 *  屏幕 4 个虚拟行   ROW0=0  ROW1=16  ROW2=32  ROW3=48
 *
 *  按键总规则
 *      长按 *  : 任何页面回主页；主页长按 * 进上次应用（没有则提示）
 *      短按 *  : 由各页面自己定义；主页短按 * = 息屏
 *      息屏    : 长按 0 或 1 唤醒；30 秒无操作自动息屏
 *
 *  分区 no_ota:  app 2MB @0x10000 / data 1.875MB @0x210000
 * ============================================================
 */

#include <Wire.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_partition.h>
#include "src/font_wqy14.h"
#include "src/btconf.h"
#include "src/timetable.h"
#include "src/reader.h"
#include <time.h>
#include <Preferences.h>

#define OLED_SDA 6
#define OLED_SCL 7
#define TTL_TX_PIN 21

#define KEY_N 4
static const uint8_t KEY_PIN[KEY_N] = {5, 10, 20, 4};
enum { KEY_STAR = 0, KEY_0 = 1, KEY_1 = 2, KEY_HASH = 3 };

#define DS3231_ADDR 0x68
#define SCREEN_W 128
#define SCREEN_H 64
#define ROW0 0
#define ROW1 16
#define ROW2 32
#define ROW3 48

/* 时区（小时）。RTC 硬件里存的是本地时间，对外的 Unix 接口一律 UTC：
 * 写 RTC 时 +gTzHour，读 RTC 时 -gTzHour。这样显示端直接读 RTC 就是
 * 本地时刻，而 BLE 的 settime/gettime 仍保持标准 UTC 语义。 */
static int gTzHour = 8;
#define I2C_CHUNK 64

#define LONG_MS       700
#define REPEAT_MS     120
#define DEBOUNCE_MS   25
#define SCREEN_OFF_MS 30000UL

/* C3 SuperMini 板载 LED 在 GPIO8，低电平点亮。
 * 启动后闪 3 秒作为开机指示，然后熄灭。 */
/* SSD1306 对比度 0x00-0xFF。0xCF 偏亮刺眼，降到 0x7F（手册默认）。
 * 嫌暗就调 0x9F，嫌亮就调 0x5F。 */
#define OLED_CONTRAST 0x7F
#define BR_MIN 16
#define BR_MAX 255
#define BR_STEP 24

static uint8_t  gContrast    = OLED_CONTRAST;
static bool     gContrastDirty = false;
static uint32_t gContrastSaveAt = 0;

#define LED_PIN   8
#define LED_ON    LOW
#define LED_OFF   HIGH

static uint8_t  gBuf[SCREEN_W * SCREEN_H / 8];
static uint8_t  gOledAddr = 0x3C;
static bool     gOledOk = false;
static bool     gRtcOk  = false;
static bool     gFsOk   = false;
static uint32_t gI2cErr = 0;
static uint32_t gClock  = 400000;
static uint32_t gLoopMaxMs = 0;
static bool     gVerbose = false;
static uint16_t gAnimTick = 0;      /* 加载动画帧计数 */

static bool     gScreenOff = false;
static uint32_t gLastActivity = 0;
static uint32_t gWakeLocks = 0;      /* 唤醒锁位掩码，非 0 = 有后台任务要保持活跃 */
static uint32_t gLedStopAt = 0, gLedToggleAt = 0;
static bool     gLedPhase = false;
static bool     gTimerExpired = false;

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
};

/* 唤醒事件来源。必须在第一个函数之前定义：
 * Arduino 会把所有函数原型插到文件开头，类型晚定义会让原型里的类型未声明。 */
enum WakeSource { WAKE_KEY = 0, WAKE_TIMER_DONE, WAKE_RTC_ALARM, WAKE_SQW, WAKE_EXT };
static const char *WAKE_NAME[] = {"按键", "倒计时到点", "RTC闹钟", "SQW引脚", "外部"};

/* ==================== 日志 ====================
 * 原来「缓冲不够就整行丢掉」：扫描 20 个 AP 只打出 5 行，诊断信息也会被吞。
 * 改成先等最多 50ms 让缓冲排空，仍然没有位置才放弃这一路。 */
static void toast(const char *m, uint32_t ms);

/* 运行时可调的日志级别：0=关 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=VERBOSE */
static int gLogLevel = 2;

static esp_log_level_t logLevelEnum() {
  switch (gLogLevel) {
    case 0:  return ESP_LOG_NONE;
    case 1:  return ESP_LOG_ERROR;
    case 2:  return ESP_LOG_WARN;
    case 3:  return ESP_LOG_INFO;
    case 4:  return ESP_LOG_DEBUG;
    default: return ESP_LOG_VERBOSE;
  }
}

static void logSetLevel(int lv) {
  if (lv < 0) lv = 0;
  if (lv > 5) lv = 5;
  gLogLevel = lv;
  esp_log_level_set("*", logLevelEnum());
}

static void logWrite(const char *buf, size_t len) {
  /* USB CDC 的一个坑：插着数据线但没打开串口软件时，USB 是枚举成功的
   * （(bool)Serial 为真），可主机不取数据，发送环形缓冲永远是满的。
   * 原来每行硬等 50ms，setup 几十行下来就是几秒的「假卡死」；
   * 一开串口主机开始取数据，立刻恢复正常。
   *
   * 现在：能塞多少塞多少，最多等 3ms；连续几次都塞不下就当没人看，
   * 静音 3 秒（期间只做非阻塞写），3 秒后再试探一次。
   * 纯供电线根本枚举不到 USB，(bool)Serial 为假，完全不进这段。 */
  static uint32_t muteUntil = 0;
  static uint8_t  stuckHits = 0;

  if (Serial) {
    if ((int32_t)(millis() - muteUntil) < 0) {
      int room = Serial.availableForWrite();
      if (room > 0)
        Serial.write((const uint8_t *)buf, len <= (size_t)room ? len : (size_t)room);
    } else {
      uint32_t t0 = millis();
      while (Serial.availableForWrite() < (int)len && millis() - t0 < 3) delay(1);
      int room = Serial.availableForWrite();
      if (room >= (int)len) {
        Serial.write((const uint8_t *)buf, len);
        stuckHits = 0;
      } else {
        if (room > 0) Serial.write((const uint8_t *)buf, (size_t)room);
        if (++stuckHits >= 6) {          /* 攒够 6 次，判定没人读 */
          muteUntil = millis() + 3000;
          stuckHits = 0;
        }
      }
    }
  }
  {
    uint32_t t0 = millis();
    while (Serial0.availableForWrite() < (int)len && millis() - t0 < 50) delay(1);
    if (Serial0.availableForWrite() >= (int)len)
      Serial0.write((const uint8_t *)buf, len);
  }
}

static void logf(const char *fmt, ...) {
  char buf[288];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
  logWrite(buf, len);
}

/* ==================== 供 btconf.cpp 调用 ==================== */
void appLog(const char *fmt, ...) {
  char buf[288];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
  logWrite(buf, len);
}
void appToast(const char *msg, uint32_t ms) { toast(msg, ms); }

/* ==================== I2C / OLED ==================== */
static bool i2cWrite(uint8_t addr, uint8_t ctrl, const uint8_t *d, size_t n) {
  bool ok = true;
  for (size_t off = 0; off < n; off += I2C_CHUNK) {
    size_t m = (n - off < I2C_CHUNK) ? (n - off) : I2C_CHUNK;
    Wire.beginTransmission(addr);
    Wire.write(ctrl);
    Wire.write(d + off, m);
    if (Wire.endTransmission() != 0) { ok = false; gI2cErr++; }
  }
  return ok;
}
static void oledCmd(uint8_t c) { i2cWrite(gOledAddr, 0x00, &c, 1); }
static void clearBuf() { memset(gBuf, 0, sizeof(gBuf)); }

static void setPixel(int x, int y, bool on) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
  uint16_t i = (uint16_t)y / 8 * SCREEN_W + x;
  uint8_t  b = 1 << (y & 7);
  if (on) gBuf[i] |= b; else gBuf[i] &= (uint8_t)~b;
}
static void hline(int x0, int x1, int y) { for (int x = x0; x <= x1; x++) setPixel(x, y, true); }
static void fillRect(int x0, int y0, int x1, int y1) { for (int y = y0; y <= y1; y++) hline(x0, x1, y); }
static void vline(int y0, int y1, int x) { for (int y = y0; y <= y1; y++) setPixel(x, y, true); }
static void drawRect(int x0, int y0, int x1, int y1) {
  hline(x0, x1, y0); hline(x0, x1, y1);
  vline(y0, y1, x0); vline(y0, y1, x1);
}

static void flush() {                       /* 全屏，用于初始化/清屏 */
  oledCmd(0x21); oledCmd(0x00); oledCmd(0x7F);
  oledCmd(0x22); oledCmd(0x00); oledCmd(0x07);
  i2cWrite(gOledAddr, 0x40, gBuf, sizeof(gBuf));
}

/* 局部刷新：只发送和上一帧不同的页段。
 * 全屏一帧 1024 字节，400kHz 下约 24ms，最多 40fps；
 * 精确时钟只有毫秒几位在变，用 diff 后通常只剩 100 字节左右，约 2ms。 */
static uint8_t gPrevBuf[SCREEN_W * SCREEN_H / 8];
static bool    gPrevValid = false;

static void flushDiff() {
  if (!gPrevValid) {
    flush();
    memcpy(gPrevBuf, gBuf, sizeof(gBuf));
    gPrevValid = true;
    return;
  }
  int pMin = 8, pMax = -1, xMin = SCREEN_W, xMax = -1;
  for (int p = 0; p < 8; p++) {
    const uint8_t *a = &gBuf[p * SCREEN_W];
    const uint8_t *b = &gPrevBuf[p * SCREEN_W];
    for (int x = 0; x < SCREEN_W; x++) {
      if (a[x] != b[x]) {
        if (p < pMin) pMin = p;
        if (p > pMax) pMax = p;
        if (x < xMin) xMin = x;
        if (x > xMax) xMax = x;
      }
    }
  }
  if (pMax < 0) return;                    /* 画面没变，什么都不发 */
  int len = xMax - xMin + 1;
  for (int p = pMin; p <= pMax; p++) {
    oledCmd(0x22); oledCmd((uint8_t)p); oledCmd((uint8_t)p);
    oledCmd(0x21); oledCmd((uint8_t)xMin); oledCmd((uint8_t)xMax);
    i2cWrite(gOledAddr, 0x40, &gBuf[p * SCREEN_W + xMin], len);
  }
  memcpy(gPrevBuf, gBuf, sizeof(gBuf));
}

static bool oledInit() {
  static const uint8_t seq[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
    0x81, OLED_CONTRAST, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF
  };
  if (!i2cWrite(gOledAddr, 0x00, seq, sizeof(seq))) return false;
  clearBuf(); flush();
  gPrevValid = false;
  return true;
}

/* ==================== 字库渲染 ====================
 * 字库不再编进固件，而是烧在独立的 font 分区（128KB），
 * 启动时 mmap 到地址空间，之后访问和普通 const 数组一样快。
 */
const wqy14_glyph_t *gWqy14 = nullptr;
static esp_partition_mmap_handle_t gFontMap = 0;

static bool fontMount() {
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "font");
  if (!p) {
    logf("[字库] 找不到 font 分区（分区表或字库没刷）\r\n");
    return false;
  }
  const void *ptr = nullptr;
  esp_err_t err = esp_partition_mmap(p, 0, p->size,
                                     ESP_PARTITION_MMAP_DATA, &ptr, &gFontMap);
  if (err != ESP_OK || ptr == nullptr) {
    logf("[字库] mmap 失败 err=%d\r\n", (int)err);
    return false;
  }
  gWqy14 = (const wqy14_glyph_t *)ptr;

  /* 简单完整性检查：首字形必须是空格 0x20，末字形必须有序 */
  uint16_t first = gWqy14[0].rune;
  uint16_t last  = gWqy14[WQY14_COUNT - 1].rune;
  bool ok = (first == 0x0020) && (last > first);
  logf("[字库] mmap %u KB @0x%06X, %d 字形 0x%04X..0x%04X %s\r\n",
       (unsigned)(p->size / 1024), (unsigned)p->address,
       WQY14_COUNT, first, last, ok ? "OK" : "内容异常");
  return ok;
}

static const wqy14_glyph_t *findGlyph(uint32_t cp) {
  if (!gWqy14) return nullptr;
  int lo = 0, hi = WQY14_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t r = gWqy14[mid].rune;
    if (r == cp) return &gWqy14[mid];
    if (r < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

/* 供 reader.cpp 排版用：拿不到字形就按 8px 算 */
int appCpWidth(uint32_t cp) {
  const wqy14_glyph_t *g = findGlyph(cp);
  return g ? g->w : 8;
}

static uint32_t utf8Next(const char *&s) {
  uint8_t c = (uint8_t)*s++;
  if (c < 0x80) return c;
  uint32_t cp; int n;
  if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; n = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
  else return '?';
  while (n-- > 0) {
    if ((*s & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | ((uint8_t)*s++ & 0x3F);
  }
  return cp;
}

static int drawTextMode(int x, int y, const char *s, uint8_t scale, bool mode) {
  int cx = x;
  const char *p = s;
  while (*p) {
    uint32_t cp = utf8Next(p);
    const wqy14_glyph_t *g = findGlyph(cp);
    if (!g) { cx += 8 * scale; continue; }
    for (int row = 0; row < WQY14_HEIGHT; row++) {
      uint16_t bits = g->rows[row];
      if (!bits) continue;
      for (int col = 0; col < g->w && col < 16; col++) {
        if (!(bits & (1u << col))) continue;
        if (scale == 1) setPixel(cx + col, y + row, mode);
        else for (int dy = 0; dy < scale; dy++)
               for (int dx = 0; dx < scale; dx++)
                 setPixel(cx + col * scale + dx, y + row * scale + dy, mode);
      }
    }
    cx += g->w * scale;
  }
  return cx;
}

static int textWidthScaled(const char *s, uint8_t scale) {
  int w = 0; const char *p = s;
  while (*p) { const wqy14_glyph_t *g = findGlyph(utf8Next(p)); w += (g ? g->w : 8) * scale; }
  return w;
}

static void drawText(int x, int y, const char *s) { drawTextMode(x, y, s, 1, true); }
static void drawCenter(int y, const char *s) { drawText((SCREEN_W - textWidthScaled(s,1))/2, y, s); }

/* 标题栏：复刻 Pi 面板 drawTitleBar（padX=2, titleY=0, titleH=14） */
static void drawTitleBar(const char *title) {
  drawText(2, 0, title);
  hline(0, SCREEN_W - 1, 14);
}

/* 加载占位：复刻 Pi 面板 uikit.go 的 drawLoading
 *   标题栏 + 说明文字(y=22) + 轨道(y=44..49) + 26px 光条每帧右移 5px
 *   Pi 上 120fps 约 0.26s 一圈；这里 30ms 一帧约 0.5s 一圈，观感接近。 */
static void drawLoading(const char *title, const char *msg, uint16_t tick) {
  clearBuf();
  if (title && *title) drawTitleBar(title);
  drawText(2, 22, msg);

  const int trackY = 44, trackH = 6;
  drawRect(0, trackY, SCREEN_W - 1, trackY + trackH - 1);

  const int barW = 26;
  int span = SCREEN_W + barW;
  int x  = (tick * 5) % span;
  int bx = x - barW;
  for (int i = 0; i < barW; i++) {
    int px = bx + i;
    if (px < 1 || px > SCREEN_W - 2) continue;
    fillRect(px, trackY + 1, px, trackY + trackH - 2);
  }
}
/* ---------- 屏幕亮度（对比度 0x81） ---------- */
static void contrastApply() {
  oledCmd(0x81);
  oledCmd(gContrast);
}

static void contrastLoad() {
  Preferences p;
  p.begin("cfg", true);
  int v = p.getInt("bl", -1);
  p.end();
  if (v >= BR_MIN && v <= BR_MAX) gContrast = (uint8_t)v;
}

static void contrastSave() {
  Preferences p;
  p.begin("cfg", false);
  p.putInt("bl", gContrast);
  p.end();
  gContrastDirty = false;
}

/* 0 键减、1 键加；连按或长按都按步长走，松手 0.7 秒后落盘 */
static void contrastStep(int dir) {
  int v = (int)gContrast + dir * BR_STEP;
  if (v < BR_MIN) v = BR_MIN;
  if (v > BR_MAX) v = BR_MAX;
  if (v == (int)gContrast) return;
  gContrast = (uint8_t)v;
  contrastApply();
  gContrastDirty  = true;
  gContrastSaveAt = millis() + 700;
  char b[24];
  snprintf(b, sizeof(b), "亮度 %d%%", gContrast * 100 / 255);
  appToast(b, 900);
}

static void drawCenterScaled(int y, const char *s, uint8_t sc) {
  int x = (SCREEN_W - textWidthScaled(s, sc)) / 2;
  drawTextMode(x < 0 ? 0 : x, y, s, sc, true);
}

/* ==================== DS3231 ==================== */
static inline uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static int weekdayFromDate(int y, int m, int d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
static const char *WEEK_CN[7] = {"日","一","二","三","四","五","六"};

/* weekdayFromDate 返回 0=周日，而课表/APP 的 day 是 1=周一…7=周日。
   这里统一换算，别在各处再拍 +1。 */
static int weekdayIso(int y, int m, int d) {
  int w = weekdayFromDate(y, m, d);
  return w == 0 ? 7 : w;
}

static bool dsReadRaw(uint8_t reg, uint8_t *out, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)DS3231_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
  return true;
}
static bool dsWriteRaw(uint8_t reg, const uint8_t *in, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(in[i]);
  return Wire.endTransmission() == 0;
}
static bool dsReadTime(RtcTime &t) {
  uint8_t b[7];
  if (!dsReadRaw(0x00, b, 7)) return false;
  t.second = bcd2dec(b[0] & 0x7F);
  t.minute = bcd2dec(b[1] & 0x7F);
  t.hour   = bcd2dec(b[2] & 0x3F);
  t.day    = bcd2dec(b[4] & 0x3F);
  t.month  = bcd2dec(b[5] & 0x1F);
  t.year   = 2000 + bcd2dec(b[6]);
  return true;
}
/* Unix 时间戳（客户端已按本地时区加好偏移）<-> DS3231 本地时间 */
static bool dsWriteTimeRaw(const RtcTime &t);
static bool dsReadTime(RtcTime &t);
static bool dsReadTimeFwd(RtcTime &t);

bool appRtcWriteUnix(uint32_t ts) {
  /* 入参是 UTC 秒；RTC 存本地时间，所以先加上时区偏移 */
  time_t tt = (time_t)((int32_t)ts + (int32_t)gTzHour * 3600);
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  RtcTime rt;
  rt.year   = (uint16_t)(tmv.tm_year + 1900);
  rt.month  = (uint8_t)(tmv.tm_mon + 1);
  rt.day    = (uint8_t)tmv.tm_mday;
  rt.hour   = (uint8_t)tmv.tm_hour;
  rt.minute = (uint8_t)tmv.tm_min;
  rt.second = (uint8_t)tmv.tm_sec;
  bool ok = dsWriteTimeRaw(rt);
  return ok;
}

/* newlib 没暴露 timegm，自己按 Howard Hinnant 算法算距 1970-01-01 的天数 */
static int32_t daysFromEpoch(int y, int m, int d) {
  y -= (m <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int)doe - 719468;
}

bool appRtcReadUnix(uint32_t *ts) {
  RtcTime t;
  if (!dsReadTimeFwd(t)) return false;
  int32_t days = daysFromEpoch(t.year, t.month, t.day);
  /* RTC 里是本地时间，减掉时区偏移还原成 UTC 秒 */
  int64_t local = (int64_t)days * 86400
                + (int64_t)t.hour * 3600
                + (int64_t)t.minute * 60
                + (int64_t)t.second;
  int64_t utc = local - (int64_t)gTzHour * 3600;
  if (utc < 0) utc = 0;
  *ts = (uint32_t)utc;
  return true;
}

static bool dsReadTimeFwd(RtcTime &t) { return dsReadTime(t); }

static bool dsWriteTimeRaw(const RtcTime &t) {
  uint8_t b[7];
  b[0] = dec2bcd(t.second);
  b[1] = dec2bcd(t.minute);
  b[2] = dec2bcd(t.hour);
  b[3] = weekdayIso(t.year, t.month, t.day);
  b[4] = dec2bcd(t.day);
  b[5] = dec2bcd(t.month);
  b[6] = dec2bcd(t.year % 100);
  if (!dsWriteRaw(0x00, b, 7)) return false;
  uint8_t st = 0;
  dsWriteRaw(0x0F, &st, 1);
  return true;
}

/* ==================== 唤醒锁 ====================
 * 计时类任务运行时持锁，表示"现在不能进深度休眠"。
 * 当前固件没有 MCU 休眠，锁有三个用处：
 *   1. 将来接 light sleep 时作为进入条件（只在 !wakeLockHeld() 时睡）
 *   2. info 里可见，便于排查"为什么没睡"
 *   3. 与息屏解耦：锁只管 MCU，不管屏幕；屏幕该黑还是黑
 */
#define WL_STOPWATCH  0x01u
#define WL_TIMER      0x02u

static void wakeLockAcquire(uint32_t bit, const char *why) {
  if (gWakeLocks & bit) return;
  gWakeLocks |= bit;
  logf("[锁] +%s -> 0x%02X\r\n", why, (unsigned)gWakeLocks);
}
static void wakeLockRelease(uint32_t bit, const char *why) {
  if (!(gWakeLocks & bit)) return;
  gWakeLocks &= ~bit;
  logf("[锁] -%s -> 0x%02X\r\n", why, (unsigned)gWakeLocks);
}
static bool wakeLockHeld() { return gWakeLocks != 0; }

/* ==================== 按键事件 ==================== */
struct KeyEvt { bool shortPress; bool longPress; uint16_t repeat; };
static KeyEvt  gEvt[KEY_N];
static bool    gKeyPrev[KEY_N];
static bool    gKeyStable[KEY_N];
static uint32_t gKeyT[KEY_N], gKeyDownAt[KEY_N], gKeyRepeatAt[KEY_N];
static bool    gKeyLongFired[KEY_N];

static void keysInit() {
  for (int i = 0; i < KEY_N; i++) {
    pinMode(KEY_PIN[i], INPUT_PULLUP);
    gKeyPrev[i] = gKeyStable[i] = (digitalRead(KEY_PIN[i]) == LOW);
    gKeyT[i] = millis();
    gKeyLongFired[i] = false;
  }
}
static void clearEvents() {
  for (int i = 0; i < KEY_N; i++) { gEvt[i].shortPress = false; gEvt[i].longPress = false; gEvt[i].repeat = 0; }
}
static void keysPoll() {
  uint32_t now = millis();
  for (int i = 0; i < KEY_N; i++) {
    bool raw = (digitalRead(KEY_PIN[i]) == LOW);
    if (raw != gKeyPrev[i]) { gKeyPrev[i] = raw; gKeyT[i] = now; continue; }
    if (now - gKeyT[i] < DEBOUNCE_MS) continue;
    if (raw != gKeyStable[i]) {
      gKeyStable[i] = raw;
      if (raw) { gKeyDownAt[i] = now; gKeyLongFired[i] = false; }
      else     { if (!gKeyLongFired[i]) gEvt[i].shortPress = true; gKeyLongFired[i] = false; }
    }
    if (gKeyStable[i]) {
      if (!gKeyLongFired[i] && now - gKeyDownAt[i] >= LONG_MS) {
        gKeyLongFired[i] = true; gEvt[i].longPress = true; gKeyRepeatAt[i] = now;
      } else if (gKeyLongFired[i] && now - gKeyRepeatAt[i] >= REPEAT_MS) {
        gKeyRepeatAt[i] = now; gEvt[i].repeat++;
      }
    }
  }
}

/* ==================== 计时器（后台常驻） ==================== */
static uint32_t gSwAccum = 0, gSwStart = 0;
static bool     gSwRun = false;
static uint32_t gTdTotal = 0, gTdStart = 0;
static bool     gTdRun = false;

static uint32_t swElapsed() { return gSwAccum + (gSwRun ? (millis() - gSwStart) : 0); }
static uint32_t tdRemain() {
  if (!gTdRun) return gTdTotal;
  uint32_t used = millis() - gTdStart;
  return (used >= gTdTotal) ? 0 : (gTdTotal - used);
}
static void swToggle() {
  if (gSwRun) { gSwAccum += millis() - gSwStart; gSwRun = false; wakeLockRelease(WL_STOPWATCH, "正计时"); }
  else        { gSwStart = millis(); gSwRun = true;  wakeLockAcquire(WL_STOPWATCH, "正计时"); }
}
static void swReset() { gSwRun = false; gSwAccum = 0; wakeLockRelease(WL_STOPWATCH, "正计时"); }
static void tdToggle() {
  if (gTdRun) { gTdTotal = tdRemain(); gTdRun = false; wakeLockRelease(WL_TIMER, "倒计时"); }
  else if (gTdTotal > 0) { gTdStart = millis(); gTdRun = true; wakeLockAcquire(WL_TIMER, "倒计时"); }
}
static void tdStopClear() { gTdRun = false; gTdTotal = 0; wakeLockRelease(WL_TIMER, "倒计时"); }

/* ==================== 界面状态 ==================== */
enum AppId {
  APP_HOME = 0, APP_PRECISE, APP_STOPWATCH, APP_TIMER, APP_SCHEDULE,  /* 菜单可见 */
  APP_READER, APP_BTCONF,                                            /* 隐藏，靠 ID 进 */
  APP_COUNT
};
#define MENU_COUNT 5          /* 菜单只列前 5 项 */
#define ID_MAX_BITS 8         /* 编号最多 8 位二进制 */
enum Screen { SCR_APP, SCR_MENU, SCR_ID };

static const char *APP_NAME[APP_COUNT] = {
  "主页", "精确时钟", "正计时", "倒计时", "课表",
  "阅读器", "蓝牙配置"
};

static int      gApp = APP_HOME;
static int      gLastApp = -1;          /* -1 表示还没有"上一次应用" */
static enum Screen gScr = SCR_APP;
static int      gMenuSel = 0;
static char     gIdBuf[ID_MAX_BITS + 1] = {0};
static uint8_t  gIdLen = 0;

static char     gToast[40] = {0};
static uint32_t gToastUntil = 0;
static void toast(const char *m, uint32_t ms) { strncpy(gToast, m, sizeof(gToast) - 1); gToastUntil = millis() + ms; }

/* 时间缓存：1 Hz 读 RTC，毫秒用 millis 补 */
static RtcTime  gNow;
static bool     gNowOk = false;
static uint32_t gNowMs = 0;
static void rtcTick() {
  RtcTime t;
  if (dsReadTime(t)) { gNow = t; gNowOk = true; gNowMs = millis(); }
  else gNowOk = false;
}
static uint32_t subMilli() { return (millis() - gNowMs) % 1000; }

/* 供 timetable.cpp 算 ISO 周数用，避免它直接依赖这里的 static 变量 */
bool appNowYmd(int *y, int *m, int *d) {
  if (!gNowOk) return false;
  *y = gNow.year; *m = gNow.month; *d = gNow.day;
  return true;
}

/* ---------------- 唤醒后的按键锁 ----------------
 * 长按 0/1 唤醒时手指还按着，按键的 repeat 事件会持续产生。
 * 直接放开的话，倒计时页会被一路加秒。
 * 所以唤醒后先锁住全部按键：等所有键松开，再等 200ms 才解锁。 */
static bool     gKeyLocked = false;
static uint32_t gKeyUnlockAt = 0;

static void keyLock() { gKeyLocked = true; gKeyUnlockAt = 0; }

/* 返回 true 表示仍处于锁定状态，调用方应忽略本帧全部按键 */
static bool keyLockPoll() {
  if (!gKeyLocked) return false;
  bool anyDown = false;
  for (int i = 0; i < KEY_N; i++) if (gKeyStable[i]) { anyDown = true; break; }
  if (anyDown) { gKeyUnlockAt = 0; return true; }
  if (gKeyUnlockAt == 0) { gKeyUnlockAt = millis() + 200; return true; }
  if ((int32_t)(millis() - gKeyUnlockAt) < 0) return true;
  gKeyLocked = false;
  logf("[按键] 解锁\r\n");
  return false;
}

/* ---------------- 息屏 ---------------- */
static void screenOff() {
  if (gScreenOff) return;
  gScreenOff = true;
  clearBuf(); flush();
  oledCmd(0xAE);
  logf("[屏] 息屏\r\n");
}
static void screenWake() {
  if (!gScreenOff) return;
  gScreenOff = false;
  oledCmd(0xAF);
  gPrevValid = false;      /* 息屏时屏幕被清空过，唤醒必须整屏重画 */
  keyLock();               /* 唤醒用的长按不能漏进应用 */
  gLastActivity = millis();
  logf("[屏] 唤醒\r\n");
}

static void wakeUp(WakeSource src) {
  if (gScreenOff) {
    screenWake();
    gLastActivity = millis();
    logf("[唤醒] 来源 %s\r\n", WAKE_NAME[src]);
  }
  if (src == WAKE_TIMER_DONE) toast("时间到", 4000);
}

/* ---------------- 走马灯 ----------------
 * 课表那行文案经常超过 128px（"下节 高等数学 还有45分 · 16:40放学"），
 * 超过 128px 就横向滚动：首尾各停 800ms，中间每 45ms 走 2px；
 * 放得下就居中静态显示，不折腾。 */
static char     gMqText[64] = "";
static int16_t  gMqOff = 0;
static uint32_t gMqNext = 0;

static void drawMarquee(int y, const char *s) {
  int w = textWidthScaled(s, 1);
  if (w <= SCREEN_W) {
    gMqText[0] = 0; gMqOff = 0;
    drawCenter(y, s);
    return;
  }
  if (strcmp(gMqText, s) != 0) {          /* 文案变了就从头滚 */
    snprintf(gMqText, sizeof(gMqText), "%s", s);
    gMqOff = 0;
    gMqNext = millis() + 800;
    drawText(0, y, s);
    return;
  }
  uint32_t now = millis();
  if ((int32_t)(now - gMqNext) >= 0) {
    gMqOff += 2;
    if (gMqOff > w + 20) { gMqOff = 0; gMqNext = now + 800; }
    else                 { gMqNext = now + 45; }
  }
  drawText(-gMqOff, y, s);
}

static void hhmm(char *buf, size_t n, uint16_t min) {
  snprintf(buf, n, "%u:%02u", (unsigned)(min / 60), (unsigned)(min % 60));
}

/* 还有多久：2小时15分 / 45分 / 不到1分 */
static void durText(char *buf, size_t n, int min) {
  if (min <= 0)      snprintf(buf, n, "不到1分");
  else if (min < 60) snprintf(buf, n, "%d分", min);
  else               snprintf(buf, n, "%d小时%d分", min / 60, min % 60);
}

/* 主页第 4 行：正在上的课 → 下节课 → 已放学 → 今日无课 */
static void drawHomeSchedule() {
  if (!gNowOk) { drawCenter(ROW3, "--"); return; }

  char line[64], t1[8], t2[8], d1[16];
  int  today  = weekdayIso(gNow.year, gNow.month, gNow.day);
  int  nowMin = (int)gNow.hour * 60 + gNow.minute;
  int  endMin = 0;
  bool hasEnd = ttDayEnd(today, &endMin);

  int cur = ttNowIdx(today, nowMin);
  int nxt = ttNextIdx(today, nowMin);

  if (cur >= 0) {
    const TtCourse *c = ttGet(cur);
    if (hasEnd) {
      durText(d1, sizeof(d1), endMin - nowMin);
      snprintf(line, sizeof(line), "正在上 %s 剩%d分 · 放学还有%s", c->name, (int)c->end - nowMin, d1);
    } else {
      snprintf(line, sizeof(line), "正在上 %s 剩%d分", c->name, (int)c->end - nowMin);
    }
  } else if (nxt >= 0) {
    const TtCourse *c = ttGet(nxt);
    hhmm(t1, sizeof(t1), c->start);
    durText(d1, sizeof(d1), (int)c->start - nowMin);
    snprintf(line, sizeof(line), "下节 %s %s · 还有%s", c->name, t1, d1);
  } else if (hasEnd) {
    hhmm(t2, sizeof(t2), (uint16_t)endMin);
    snprintf(line, sizeof(line), "今日课已结束 · %s放学", t2);
  } else {
    snprintf(line, sizeof(line), "今日无课");
  }
  drawMarquee(ROW3, line);
}

/* ---------------- 各页面绘制 ---------------- */
static void drawHome() {
  char l1[32], l2[16];
  if (gNowOk) {
    snprintf(l1, sizeof(l1), "%02u.%02u.%02u-周%s", (unsigned)(gNow.year % 100),
             gNow.month, gNow.day, WEEK_CN[weekdayFromDate(gNow.year, gNow.month, gNow.day)]);
    drawText(0, ROW0, l1);
    snprintf(l2, sizeof(l2), "%02u:%02u:%02u", gNow.hour, gNow.minute, gNow.second);
    drawCenterScaled(ROW1 + 1, l2, 2);
  } else {
    drawText(0, ROW0, "--.--.--");
    drawCenterScaled(ROW1 + 1, "--:--:--", 2);
  }
  drawHomeSchedule();
}

static void drawPrecise() {
  char l2[16], l3[12];
  if (gNowOk) {
    snprintf(l2, sizeof(l2), "%02u:%02u:%02u", gNow.hour, gNow.minute, gNow.second);
    drawCenterScaled(ROW1 - 2, l2, 2);
    snprintf(l3, sizeof(l3), ".%03u", (unsigned)subMilli());
    drawCenter(ROW3, l3);
  } else {
    drawCenterScaled(ROW1, "--:--:--", 2);
    drawCenter(ROW3, ".---");
  }
}

static void drawStopwatch() {
  uint32_t ms = swElapsed();
  uint32_t totalSec = ms / 1000;
  char l0[24], l2[16], l3[12];
  snprintf(l0, sizeof(l0), "正计时 %s", gSwRun ? "运行中" : "已暂停");
  drawText(0, ROW0, l0);
  snprintf(l2, sizeof(l2), "%02u:%02u:%02u", (unsigned)(totalSec / 3600),
           (unsigned)((totalSec / 60) % 60), (unsigned)(totalSec % 60));
  drawCenterScaled(ROW1 + 1, l2, 2);
  snprintf(l3, sizeof(l3), ".%03u", (unsigned)(ms % 1000));   /* 与精确时钟同精度 */
  drawCenter(ROW3, l3);
}

static void drawTimer() {
  uint32_t rem = tdRemain();
  uint32_t totalSec = rem / 1000;
  char l0[24], l2[16], l3[20];
  if (gTdRun)   snprintf(l0, sizeof(l0), "倒计时 运行中");
  else if (rem) snprintf(l0, sizeof(l0), "倒计时 已暂停");
  else          snprintf(l0, sizeof(l0), "倒计时 待设定");
  drawText(0, ROW0, l0);

  snprintf(l2, sizeof(l2), "%02u:%02u:%02u", (unsigned)(totalSec / 3600),
           (unsigned)((totalSec / 60) % 60), (unsigned)(totalSec % 60));
  drawCenterScaled(ROW1 + 1, l2, 2);

  if (gTdTotal == 0) drawCenter(ROW3, "0加1分 1加1秒");
  else {
    snprintf(l3, sizeof(l3), "%u分%u秒", (unsigned)(gTdTotal / 60000),
             (unsigned)((gTdTotal / 1000) % 60));
    drawCenter(ROW3, l3);
  }
}

static const char *DAY_CN[7] = {"一", "二", "三", "四", "五", "六", "日"};

static int  gSchSel = 0;    /* 列表光标 */
static int  gSchDay = 0;    /* 0=跟随今天，1..7=指定星期 */

static int schShowDay() {
  if (gSchDay >= 1 && gSchDay <= 7) return gSchDay;
  if (!gNowOk) return 1;
  return weekdayIso(gNow.year, gNow.month, gNow.day);
}

/* 列表行：8:05-9:40 高等数学
 * 128px 里时间要占 63~77px，课名放不下就截断；选中行反白。 */
static void drawSchedule() {
  int  idx[TT_MAX];
  int  day = schShowDay();
  int  n   = ttDayList(day, idx, TT_MAX);
  char buf[64], t1[8], t2[8];

  snprintf(buf, sizeof(buf), "课表 %s%s", (gSchDay == 0) ? "今天·" : "", DAY_CN[day - 1]);
  drawTitleBar(buf);

  if (n == 0) {
    drawCenter(ROW2, "这天没有课");
    return;
  }
  if (gSchSel >= n) gSchSel = n - 1;
  if (gSchSel < 0)  gSchSel = 0;

  int first = (gSchSel >= 3) ? gSchSel - 2 : 0;
  for (int i = 0; i < 3; i++) {
    int k = first + i;
    if (k >= n) break;
    const TtCourse *c = ttGet(idx[k]);
    hhmm(t1, sizeof(t1), c->start);
    hhmm(t2, sizeof(t2), c->end);
    snprintf(buf, sizeof(buf), "%s-%s %s", t1, t2, c->name);
    int y = ROW1 + i * 16;
    if (k == gSchSel) {
      fillRect(0, y, SCREEN_W - 1, y + 14);
      drawTextMode(0, y, buf, 1, false);
    } else {
      drawText(0, y, buf);
    }
  }
  if (first > 0)     fillRect(124, 2, 127, 5);
  if (first + 3 < n) fillRect(124, 58, 127, 61);
}

static void drawMenu() {
  int first = (gMenuSel >= 4) ? gMenuSel - 3 : 0;
  for (int i = 0; i < 4; i++) {
    int idx = first + i;
    if (idx >= MENU_COUNT) break;
    int y = ROW0 + i * 16;
    if (idx == gMenuSel) {
      fillRect(0, y, SCREEN_W - 1, y + 14);
      int x = (SCREEN_W - textWidthScaled(APP_NAME[idx], 1)) / 2;
      drawTextMode(x, y, APP_NAME[idx], 1, false);
    } else {
      drawCenter(y, APP_NAME[idx]);
    }
  }
  if (first > 0) fillRect(124, 2, 127, 5);
  if (first + 4 < MENU_COUNT) fillRect(124, 58, 127, 61);
}

static void drawIdInput() {
  char l2[ID_MAX_BITS + 2];
  if (gIdLen == 0) { l2[0] = '_'; l2[1] = 0; }
  else { memcpy(l2, gIdBuf, gIdLen); l2[gIdLen] = 0; }
  drawCenterScaled(8, l2, 2);             /* 2 倍，上移到 y=8：占 8~35，给底部提示留空 */
  drawCenter(ROW3, "#确认 *返回");
}

/* 编号 → 应用。编号是二进制位串，长度不限（最多 8 位）
 *   0/00   主页        1/01  精确时钟    10  正计时    11  倒计时
 *   111    课表        100   阅读器      101 蓝牙配置  110 同步
 */
static int idToApp(const char *bits, int len) {
  if (len <= 0) return -1;
  int v = 0;
  for (int i = 0; i < len; i++) v = (v << 1) | (bits[i] - '0');
  switch (v) {
    case 0: return APP_HOME;
    case 1: return APP_PRECISE;
    case 2: return APP_STOPWATCH;
    case 3: return APP_TIMER;
    case 4: return APP_READER;
    case 5: return APP_BTCONF;
    case 7: return APP_SCHEDULE;
    default: return -1;
  }
}

/* 蓝牙配置页：启动中显示加载动画，就绪后显示 PIN 与状态 */
static void drawBtConf() {
  if (btStarting()) { drawLoading("蓝牙配置", "启动服务", gAnimTick); return; }
  clearBuf();
  drawTitleBar("蓝牙配置");
  drawCenterScaled(18, btPinText(), 2);      /* 6 位 PIN ×2 倍 = 96px */
  drawCenter(50, btStateText());
}

/* ---------------- 阅读器 ---------------- */
static int gRdView = 0;   /* 0=书目 1=正文 2=目录 */
static int gRdSel  = 0;
static int gTocSel = 0;

/* 按像素宽度截断，超出补省略号（不切断 UTF-8） */
static void fitText(char *dst, size_t dn, const char *src, int maxW) {
  int w = 0;
  size_t o = 0;
  const char *p = src;
  while (*p) {
    const char *q = p;
    uint32_t cp = utf8Next(q);
    const wqy14_glyph_t *g = findGlyph(cp);
    int gw = g ? g->w : 8;
    if (w + gw > maxW - 8) break;
    size_t len = (size_t)(q - p);
    if (o + len + 4 >= dn) break;
    memcpy(dst + o, p, len);
    o += len;
    w += gw;
    p = q;
  }
  if (*p && o + 3 < dn) {
    dst[o++] = (char)0xE2; dst[o++] = (char)0x80; dst[o++] = (char)0xA6;
  }
  dst[o] = 0;
}

static void drawBookList() {
  drawTitleBar("阅读器");
  int n = rdBookCount();
  if (n == 0) {
    drawCenter(ROW2, "还没有书");
    drawCenter(ROW3 + 2, "App 上传 txt");
    return;
  }
  if (gRdSel >= n) gRdSel = n - 1;
  if (gRdSel < 0)  gRdSel = 0;

  int first = (gRdSel >= 3) ? gRdSel - 2 : 0;
  for (int i = 0; i < 3; i++) {
    int k = first + i;
    if (k >= n) break;
    char pct[8], buf[48];
    snprintf(pct, sizeof(pct), "%d%%", rdBookPercent(k));
    int pw = textWidthScaled(pct, 1);
    fitText(buf, sizeof(buf), rdBookName(k), SCREEN_W - pw - 6);
    int y = ROW1 + i * 16;
    if (k == gRdSel) {
      fillRect(0, y, SCREEN_W - 1, y + 14);
      drawTextMode(2, y, buf, 1, false);
      drawTextMode(SCREEN_W - pw - 2, y, pct, 1, false);
    } else {
      drawText(2, y, buf);
      drawText(SCREEN_W - pw - 2, y, pct);
    }
  }
  if (first > 0)     fillRect(124, 2, 127, 5);
  if (first + 3 < n) fillRect(124, 58, 127, 61);
}

/* 目录选中行的走马灯状态 */
static char     gTocMqText[48] = {0};
static int      gTocMqOff = 0;
static uint32_t gTocMqNext = 0;

static void drawToc() {
  drawTitleBar("目录");
  int n = rdTocCount();
  if (n == 0) {
    drawCenter(ROW2, "这本书没有目录");
    return;
  }
  if (gTocSel >= n) gTocSel = n - 1;
  if (gTocSel < 0)  gTocSel = 0;

  int first = (gTocSel >= 3) ? gTocSel - 2 : 0;
  for (int i = 0; i < 3; i++) {
    int k = first + i;
    if (k >= n) break;
    int y = ROW1 + i * 16;
    const char *full = rdTocName(k);

    if (k == gTocSel) {
      /* 选中行（焦点）：放不下就静止 1 秒再横向滚，滚完停 1 秒重来 */
      int tw = textWidthScaled(full, 1);
      int x = 2;
      if (tw > SCREEN_W - 4) {
        uint32_t now = millis();
        if (strcmp(gTocMqText, full) != 0) {
          snprintf(gTocMqText, sizeof(gTocMqText), "%s", full);
          gTocMqOff = 0;
          gTocMqNext = now + 1000;
        }
        if ((int32_t)(now - gTocMqNext) >= 0) {
          gTocMqOff += 2;
          if (gTocMqOff > tw - (SCREEN_W - 4) + 16) { gTocMqOff = 0; gTocMqNext = now + 1000; }
          else                                      { gTocMqNext = now + 45; }
        }
        x = 2 - gTocMqOff;
      } else {
        snprintf(gTocMqText, sizeof(gTocMqText), "%s", full);
        gTocMqOff = 0;
      }
      fillRect(0, y, SCREEN_W - 1, y + 14);
      drawTextMode(x, y, full, 1, false);
    } else {
      char buf[48];
      fitText(buf, sizeof(buf), full, SCREEN_W - 3);
      drawText(2, y, buf);
    }
  }
  if (first > 0)     fillRect(124, 2, 127, 5);
  if (first + 3 < n) fillRect(124, 58, 127, 61);
}

static void drawReader() {
  if (gRdView == 2 && rdLoaded()) { drawToc(); return; }
  if (gRdView == 0 || !rdLoaded()) { drawBookList(); return; }

  /* 正文：无标题栏，4 行铺满，底部 1px 进度条。
   * 卷/章标题行反白显示；页首那一行是"焦点"，太宽时先停 1 秒再走马灯。 */
  for (int i = 0; i < RD_LINES; i++) {
    const char *s = rdLine(i);
    int y = i * 15;
    if (!*s) continue;

    if (rdLineLevel(i) == 0) { drawText(0, y, s); continue; }

    fillRect(0, y, SCREEN_W - 1, y + 13);
    drawTextMode(0, y, s, 1, false);
  }
  int w = SCREEN_W * rdPercent() / 100;
  if (w < 1) w = 1;
  hline(0, w - 1, 63);
}

static void drawStub(const char *title) {
  drawText(0, ROW0, title);
  drawCenterScaled(ROW1 + 1, "未实现", 2);
}

/* toast 覆盖第 4 行 */
static void drawToast() {
  if (millis() >= gToastUntil) return;
  fillRect(0, ROW3, SCREEN_W - 1, ROW3 + 14);
  int x = (SCREEN_W - textWidthScaled(gToast, 1)) / 2;
  drawTextMode(x > 0 ? x : 0, ROW3, gToast, 1, false);
}


/* 菜单项：选中时整行反白 */
static void drawMenuItem(int y, const char *s, bool sel) {
  if (sel) {
    fillRect(0, y, SCREEN_W - 1, y + 14);
    drawTextMode(2, y, s, 1, false);
  } else {
    drawText(2, y, s);
  }
}

/* ==================== 导航 ==================== */
static void goHome() {
  if (gApp == APP_BTCONF) btEnd();          /* 离开蓝牙页立即关服务 */
  if (gApp == APP_READER) rdSaveProgress(); /* 离开阅读器先记下读到的位置 */
  if (gApp != APP_HOME) gLastApp = gApp;
  gApp = APP_HOME;
  gScr = SCR_APP;
  gLastActivity = millis();
}
static void enterApp(int id) {
  if (id < 0 || id >= APP_COUNT) return;
  if (gApp == APP_BTCONF && id != APP_BTCONF) btEnd();
  if (gApp != APP_HOME && gApp != id) gLastApp = gApp;
  gApp = id;
  gScr = SCR_APP;
  gLastActivity = millis();
  logf("[界面] -> %s\r\n", APP_NAME[id]);
  if (id == APP_BTCONF) btBegin();          /* 进页面异步启动蓝牙 */
  if (id == APP_READER) { gRdView = 0; gRdSel = 0; rdScan(); }  /* 先进书目 */
}

/* ==================== 按键分发 ==================== */
static void handleKeys() {
  if (keyLockPoll()) return;    /* 唤醒后等松手 +200ms 再收按键 */

  /* 息屏：只有长按 0 / 1 能唤醒 */
  if (gScreenOff) {
    /* 息屏只禁用按键输入，RTC / 计时器 / 后台全部照常跑 */
    if (gEvt[KEY_0].longPress || gEvt[KEY_1].longPress) wakeUp(WAKE_KEY);
    return;
  }

  bool any = false;
  for (int i = 0; i < KEY_N; i++)
    if (gEvt[i].shortPress || gEvt[i].longPress || gEvt[i].repeat) { any = true; break; }
  if (any) gLastActivity = millis();

  bool starS = gEvt[KEY_STAR].shortPress, starL = gEvt[KEY_STAR].longPress;
  bool hashS = gEvt[KEY_HASH].shortPress, hashL = gEvt[KEY_HASH].longPress;
  bool zeroS = gEvt[KEY_0].shortPress,  oneS  = gEvt[KEY_1].shortPress;
  uint16_t zeroR = gEvt[KEY_0].repeat,  oneR  = gEvt[KEY_1].repeat;

  /* ---- 长按 * ：全局规则 ---- */
  if (starL) {
    if (gScr != SCR_APP) { goHome(); return; }
    if (gApp == APP_HOME) {
      if (gLastApp >= 0 && gLastApp != APP_HOME) enterApp(gLastApp);
      else toast("没有上次应用", 1500);
    } else {
      goHome();
    }
    return;
  }

  /* ---- 长按 # ：主页进 ID 输入 ---- */
  if (hashL && gApp == APP_HOME && gScr == SCR_APP) {
    gScr = SCR_ID;
    gIdLen = 0;
    logf("[界面] ID 输入\r\n");
    return;
  }

  /* ---- 菜单 ---- */
  if (gScr == SCR_MENU) {
    if (zeroS && gMenuSel > 0) gMenuSel--;
    if (oneS  && gMenuSel < MENU_COUNT - 1) gMenuSel++;
    if (hashS) enterApp(gMenuSel);
    if (starS) goHome();
    return;
  }

  /* ---- ID 输入 ---- */
  if (gScr == SCR_ID) {
    if (zeroS && gIdLen < ID_MAX_BITS) gIdBuf[gIdLen++] = '0';
    if (oneS  && gIdLen < ID_MAX_BITS) gIdBuf[gIdLen++] = '1';
    if (hashS) {
      int id = idToApp(gIdBuf, gIdLen);
      logf("[界面] ID=%s (%d 位) -> %s\r\n", gIdLen ? gIdBuf : "(空)", gIdLen,
           id >= 0 ? APP_NAME[id] : "无效");
      if (id >= 0) enterApp(id);
      else { goHome(); toast("无效编号", 1500); }
      return;
    }
    if (starS) goHome();
    return;
  }

  /* ---- 各应用 ---- */
  switch (gApp) {
    case APP_HOME:
      if (starS) { screenOff(); break; }      /* 主页短按 * = 息屏 */
      if (zeroS || zeroR) contrastStep(-1);   /* 0 键调暗 */
      if (oneS  || oneR)  contrastStep(+1);   /* 1 键调亮 */
      if (hashS) { gScr = SCR_MENU; gMenuSel = 0; }
      break;

    case APP_PRECISE:
      if (starS) goHome();
      break;

    case APP_STOPWATCH:
      if (hashS) swToggle();
      if (starS) swReset();
      break;

    case APP_TIMER: {
      /* 只有暂停时才接受递增，运行中调总量会让剩余时间跳变 */
      if (!gTdRun) {
        uint32_t adds  = (zeroS ? 1 : 0) + zeroR;
        uint32_t addss = (oneS ? 1 : 0) + oneR;
        if (adds)  gTdTotal += adds * 60000UL;
        if (addss) gTdTotal += addss * 1000UL;
        if (gTdTotal > 99UL * 3600000UL) gTdTotal = 99UL * 3600000UL;
      }
      if (hashS) tdToggle();
      if (starS) tdStopClear();
      break;
    }

    case APP_SCHEDULE: {
      /* 0/1 翻课；# 短按换星期（今天→周一…周日），长按回今天；短按 * 回主页 */
      int idx[TT_MAX];
      int n = ttDayList(schShowDay(), idx, TT_MAX);
      if ((zeroS || zeroR) && gSchSel > 0)     gSchSel--;
      if ((oneS  || oneR)  && gSchSel < n - 1) gSchSel++;
      if (hashL) { gSchDay = 0; gSchSel = 0; }
      else if (hashS) { gSchDay = (gSchDay + 1) % 8; gSchSel = 0; }
      if (starS) goHome();
      break;
    }

    case APP_READER:
      if (gRdView == 0) {
        /* 书目：0/1 选，短按 # 打开，短按 * 回主页 */
        int n = rdBookCount();
        if ((zeroS || zeroR) && gRdSel > 0)     gRdSel--;
        if ((oneS  || oneR)  && gRdSel < n - 1) gRdSel++;
        if (hashS && n > 0) {
          if (rdOpenIndex(gRdSel)) { gRdView = 1; gTocSel = 0; }
          else appToast("打开失败", 1500);
        }
        if (starS) goHome();
      } else if (gRdView == 2) {
        /* 目录：0/1 长按连续翻，短按 # 跳转，* 回正文 */
        int n = rdTocCount();
        if ((zeroS || zeroR) && gTocSel > 0)     gTocSel--;
        if ((oneS  || oneR)  && gTocSel < n - 1) gTocSel++;
        if (hashS) {
          if (rdGotoToc(gTocSel)) gRdView = 1;
          else appToast("跳转失败", 1500);
        }
        if (starS) gRdView = 1;
      } else {
        /* 正文：0/1 翻页，# 目录，* 回书目 */
        if (oneS  || oneR)  rdNext();
        if (zeroS || zeroR) rdPrev();
        if (hashS) {
          if (rdTocCount() > 0) {
            gTocSel = rdCurrentTocIndex();   /* 直接定位到正在读的这章 */
            gRdView = 2;
          } else appToast("无目录", 1200);
        }
        if (starS) { rdSaveProgress(); gRdView = 0; rdScan(); }
      }
      break;

    case APP_BTCONF:
      /* 短按 * 清配对并重启服务（长按 * 是全局回主页） */
      if (starS) { btClearBonds(); appToast("已清除配对", 1800); }
      break;
  }
}


/* ==================== 命令行 ==================== */
static void printPartitions() {
  logf("---- 分区 ----\r\n");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it) {
    const esp_partition_t *p = esp_partition_get(it);
    logf("  %-10s @0x%06X %6u KB\r\n", p->label, (unsigned)p->address, (unsigned)(p->size / 1024));
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}

static void printInfo() {
  logf("\r\n========== 运行信息 ==========\r\n");
  logf("芯片    : %s rev%d %u MHz\r\n", ESP.getChipModel(), ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz());
  logf("固件    : %u KB / 2048 KB\r\n", (unsigned)(ESP.getSketchSize() / 1024));
  logf("屏幕    : SSD1306 0x%02X %dx%d  I2C错误 %u\r\n", gOledAddr, SCREEN_W, SCREEN_H, (unsigned)gI2cErr);
  if (gNowOk)
    logf("时钟    : %04u-%02u-%02u %02u:%02u:%02u 周%s\r\n", gNow.year, gNow.month, gNow.day,
         gNow.hour, gNow.minute, gNow.second,
         WEEK_CN[weekdayFromDate(gNow.year, gNow.month, gNow.day)]);
  else
    logf("时钟    : 读取失败\r\n");
  logf("界面    : %s / %s / 屏%s\r\n", APP_NAME[gApp],
       gScr == SCR_MENU ? "菜单" : gScr == SCR_ID ? "ID输入" : "应用",
       gScreenOff ? "灭" : "亮");
  logf("上次应用: %s\r\n", gLastApp >= 0 ? APP_NAME[gLastApp] : "无");
  logf("正计时  : %u ms %s\r\n", (unsigned)swElapsed(), gSwRun ? "运行" : "停");
  logf("倒计时  : 总 %u ms 剩 %u ms %s\r\n", (unsigned)gTdTotal, (unsigned)tdRemain(), gTdRun ? "运行" : "停");
  logf("存储    : %s\r\n", gFsOk ? "已挂载" : "未挂载");
  logf("唤醒锁  : 0x%02X (%s%s)\r\n", (unsigned)gWakeLocks,
       (gWakeLocks & WL_STOPWATCH) ? "正计时 " : "",
       (gWakeLocks & WL_TIMER) ? "倒计时" : "");
  logf("字库    : %s\r\n", gWqy14 ? "已 mmap" : "未加载");
  logf("课表    : %d 条，本周%s\r\n", ttCount(), ttIsOddWeek() ? "单周" : "双周");
  logf("循环    : 最坏 %u ms\r\n", (unsigned)gLoopMaxMs);
  logf("自由堆  : %u B\r\n", (unsigned)ESP.getFreeHeap());
  printPartitions();
  logf("==============================\r\n\r\n");
}

static void handleCmd(char *cmd) {
  while (*cmd == ' ') cmd++;
  char *sp = strchr(cmd, ' ');
  if (sp) *sp = 0;
  char *args = sp ? sp + 1 : NULL;

  if (!strcasecmp(cmd, "info")) printInfo();
  else if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
    logf("\r\n========== 串口命令 ==========\r\n");
    logf("基础\r\n");
    logf("  help | ?                         本帮助\r\n");
    logf("  info                             芯片/屏幕/字库/课表/内存/分区\r\n");
    logf("  light on|off                     强制亮屏 / 息屏\r\n");
    logf("  contrast 0x7F                    对比度 0-255\r\n");
    logf("  settime YYYY-MM-DD HH:MM:SS      写 DS3231（本地时间）\r\n");
    logf("  verbose on|off                   心跳日志（排查卡顿用）\r\n");
    logf("  loglevel 0..5                    核心库日志级别（默认2=仅警告）\r\n");
    logf("  ls                               列 LittleFS 文件\r\n");
    logf("==============================\r\n\r\n");
  }
  else if (!strcasecmp(cmd, "light") && args) {
    if (!strcasecmp(args, "on")) screenWake(); else screenOff();
  }
  else if (!strcasecmp(cmd, "settime") && args) {
    int y, mo, d, hh, mi, ss;
    if (sscanf(args, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &hh, &mi, &ss) == 6) {
      RtcTime t; t.year = y; t.month = mo; t.day = d; t.hour = hh; t.minute = mi; t.second = ss;
      logf("[命令] %s\r\n", dsWriteTimeRaw(t) ? "写入成功" : "写入失败");
      rtcTick();
    } else logf("[命令] 格式: settime YYYY-MM-DD HH:MM:SS\r\n");
  }
  else if (!strcasecmp(cmd, "contrast") && args) {
    int v = (int)strtol(args, NULL, 0);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    gContrast = (uint8_t)v;
    contrastApply();
    contrastSave();
    logf("[命令] 对比度设为 0x%02X\r\n", (unsigned)v);
  }
  else if (!strcasecmp(cmd, "loglevel") && args) {
    int lv = atoi(args);
    logSetLevel(lv);
    logf("[命令] 日志级别 %d（0关 1错 2警告 3信息 4调试 5详细）\r\n", gLogLevel);
  }
  else if (!strcasecmp(cmd, "verbose") && args) {
    gVerbose = !strcasecmp(args, "on");
    logf("[命令] 心跳 %s\r\n", gVerbose ? "开" : "关");
  }
  else if (!strcasecmp(cmd, "ls")) {
    if (!gFsOk) { logf("[命令] 未挂载\r\n"); return; }
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    if (!f) logf("[命令] 空\r\n");
    while (f) { logf("  %-20s %6u B\r\n", f.path(), (unsigned)f.size()); f = root.openNextFile(); }
  }
  else if (*cmd) logf("[命令] 未知: %s\r\n", cmd);
}

/* 命令行要放得下 URL（wifi curl https://…），64 字节不够 */
static char gCmdBuf[192];
static uint16_t gCmdLen = 0;
static void cmdPoll(Stream &s) {
  while (s.available()) {
    char c = (char)s.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (gCmdLen) { gCmdBuf[gCmdLen] = 0; handleCmd(gCmdBuf); gCmdLen = 0; }
    } else if (gCmdLen < sizeof(gCmdBuf) - 1) gCmdBuf[gCmdLen++] = c;
  }
}

/* ==================== setup / loop ==================== */
void setup() {
  Serial.begin(115200);
  /* 核心与驱动的 log_x 默认走 UART0；插上 USB 时没人看得到。
   * 打开后改走 USB CDC，配合 build_opt.h 的 CORE_DEBUG_LEVEL=5 才有效。 */
  Serial.setDebugOutput(true);
  Serial0.begin(115200, SERIAL_8N1, -1, TTL_TX_PIN);
  uint32_t tUsb = millis();
  while (!Serial && millis() - tUsb < 1200) delay(10);
  delay(200);

  logf("\r\n===== 桌面时钟终端 =====\r\n");
  logf("按键 K1=* K2=0 K3=1 K4=#\r\n");
  logf("[调试] CORE_DEBUG_LEVEL=%d（0=关 5=最详细）\r\n", CORE_DEBUG_LEVEL);
  /* Arduino 的 log_x 已由 CORE_DEBUG_LEVEL=5 打开；这里再把 IDF 组件的运行时
   * 过滤阈值放到最低。是否真有输出取决于 sdkconfig 的 LOG_MAXIMUM_LEVEL，
   * 预编译库若在更高级别把它编掉了，这一行不会有额外输出，也无副作用。 */
  /* 默认只留 WARN 及以上：核心库的 I2C 每帧十几条 VERBOSE 日志会把
   * USB CDC 塞满，既慢又看不清自己的输出。需要细查时串口敲
   * loglevel 5 当场放开，不用重新编译。 */
  esp_log_level_set("*", logLevelEnum());

  /* 板载 LED：先熄灭，再由 loop 闪 3 秒 */
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  gLedStopAt = millis() + 3000;
  gLedToggleAt = millis();

  keysInit();
  Wire.begin(OLED_SDA, OLED_SCL, 100000);
  Wire.setClock(gClock);

  Wire.beginTransmission(0x3D);
  if (Wire.endTransmission() == 0) gOledAddr = 0x3D;

  gOledOk = oledInit();
  contrastLoad();          /* 先读回上次的亮度，再盖掉初始化里的默认值 */
  if (gOledOk) contrastApply();
  logf("[屏幕] %s @0x%02X 亮度 0x%02X\r\n", gOledOk ? "OK" : "失败", gOledAddr, gContrast);

  fontMount();          /* 字库从 font 分区 mmap，失败的话屏幕会显示方块 */

  /* 时区必须在任何 RTC 读写之前载入，否则手机下发的时间会差几个小时 */
  gTzHour = btCfgGetTz();
  logf("[配置] 时区 UTC%+d\r\n", gTzHour);

  uint8_t st = 0xFF;
  gRtcOk = dsReadRaw(0x0F, &st, 1);
  if (gRtcOk) {
    logf("[时钟] DS3231 在线 0x%02X\r\n", st);
    if (st & 0x80) {
      static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
      RtcTime t = {0}; char mb[4] = {0}; int d = 0, y = 0, hh = 0, mi = 0, ss = 0;
      if (sscanf(__DATE__, "%3s %d %d", mb, &d, &y) == 3 &&
          sscanf(__TIME__, "%d:%d:%d", &hh, &mi, &ss) == 3) {
        for (int i = 0; i < 12; i++) if (!strcmp(mb, mon[i])) { t.month = i + 1; break; }
        t.year = y; t.day = d; t.hour = hh; t.minute = mi; t.second = ss;
        dsWriteTimeRaw(t);
        logf("[时钟] OSF 置位，已用编译时间初始化\r\n");
      }
    }
  } else {
    logf("[时钟] DS3231 无应答\r\n");
  }
  rtcTick();

  gFsOk = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  logf("[存储] %s %u KB\r\n", gFsOk ? "OK" : "失败", gFsOk ? (unsigned)(LittleFS.totalBytes() / 1024) : 0u);
  if (gFsOk) {
    ttBegin();
    logf("[课表] %d 条，本周%s\r\n", ttCount(), ttIsOddWeek() ? "单周" : "双周");
    rdInit();
  }

  gApp = APP_HOME;
  gScr = SCR_APP;
  gLastApp = -1;
  gLastActivity = millis();
  logf("[就绪] info 查看详情\r\n\r\n");
}

void loop() {
  uint32_t tLoop = micros();
  static uint32_t lastRtc = 0, lastDraw = 0;
  static bool toastPrev = false;

  keysPoll();
  if (Serial) cmdPoll(Serial);
  btPoll();

  uint32_t now = millis();

  /* 开机指示：闪 3 秒后熄灭 */
  if (gLedStopAt) {
    if ((int32_t)(now - gLedStopAt) >= 0) {
      digitalWrite(LED_PIN, LED_OFF);
      gLedStopAt = 0;
      logf("[LED] 启动指示结束，熄灭\r\n");
    } else if ((int32_t)(now - gLedToggleAt) >= 0) {
      gLedToggleAt = now + 250;
      gLedPhase = !gLedPhase;
      digitalWrite(LED_PIN, gLedPhase ? LED_ON : LED_OFF);
    }
  }

  if (gContrastDirty && (int32_t)(now - gContrastSaveAt) >= 0) contrastSave();

  if (now - lastRtc >= 1000) { lastRtc = now; rtcTick(); }

  /* 倒计时归零 */
  if (gTdRun && tdRemain() == 0) {
    gTdRun = false;
    gTdTotal = 0;
    wakeLockRelease(WL_TIMER, "倒计时");
    wakeUp(WAKE_TIMER_DONE);
    logf("[计时] 倒计时归零\r\n");
  }

  /* 30 秒无操作自动息屏 */
  /* 持锁时屏幕常亮：计时跑着的时候不该黑屏 */
  if (!gScreenOff && !wakeLockHeld() && now - gLastActivity >= SCREEN_OFF_MS) screenOff();

  if (!gScreenOff) {
    /* toast 出现/消失时强制补画一帧 */
    bool toastNow = (now < gToastUntil);
    if (toastNow != toastPrev) { toastPrev = toastNow; lastDraw = 0; }

    /* 走毫秒的页面都要高帧率；局部刷新后单帧 2-3ms，60fps 撑得住 */
    uint32_t interval = 120;
    if (gScr == SCR_APP && gApp == APP_HOME)      interval = 60;   /* 走马灯 45ms 一步 */
    if (gScr == SCR_APP && (gApp == APP_PRECISE || gApp == APP_STOPWATCH)) interval = 16;
    if (gScr == SCR_APP && gApp == APP_BTCONF) interval = 30;   /* 加载动画 33fps */
    if (now - lastDraw >= interval) {
      lastDraw = now;
      clearBuf();
      if (gScr == SCR_MENU)    drawMenu();
      else if (gScr == SCR_ID) drawIdInput();
      else {
        switch (gApp) {
          case APP_HOME:      drawHome(); break;
          case APP_PRECISE:   drawPrecise(); break;
          case APP_STOPWATCH: drawStopwatch(); break;
          case APP_TIMER:     drawTimer(); break;
          case APP_SCHEDULE:  drawSchedule(); break;
          case APP_READER:    drawReader(); break;
          case APP_BTCONF:    drawBtConf(); break;
        }
      }
      drawToast();
      if (gOledOk) flushDiff();

      gAnimTick++;

      if (gVerbose)
        logf("[t] %s 屏%s err %u loop %ums\r\n", APP_NAME[gApp], gScreenOff ? "灭" : "亮",
             (unsigned)gI2cErr, (unsigned)gLoopMaxMs);
    }
  }

  handleKeys();
  clearEvents();

  uint32_t dt = (micros() - tLoop) / 1000;
  if (dt > gLoopMaxMs) gLoopMaxMs = dt;
}

