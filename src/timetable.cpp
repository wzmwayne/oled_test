/*
 * timetable.cpp - 课表存储与查询
 * 详见 timetable.h。这里只做三件事：读写 /tt.json、按天过滤排序、算单双周。
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include "timetable.h"

static TtCourse gTt[TT_MAX];
static int      gTtN = 0;

/* ==================== 日期工具（Howard Hinnant 算法） ==================== */
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
static void civilFromDays(int32_t z, int *yy, unsigned *mm, unsigned *dd) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int      y   = (int)yoe + (int)era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp  = (5u * doy + 2u) / 153u;
  const unsigned d   = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m   = mp + (mp < 10u ? 3u : (unsigned)-9);
  *yy = y + (m <= 2);
  *mm = m;
  *dd = d;
}

/* ISO 8601 周数：先找本周四，周四所在的年份决定第几周 */
static int isoWeekNumber(int y, int m, int d) {
  int32_t days = daysFromCivil(y, (unsigned)m, (unsigned)d);
  int wd = (int)(((days % 7) + 7 + 3) % 7);      /* 0=周一，1970-01-01 是周四 */
  int32_t thu = days + (3 - wd);
  int ty; unsigned tm, td;
  civilFromDays(thu, &ty, &tm, &td);
  int32_t jan1 = daysFromCivil(ty, 1, 1);
  return (int)((thu - jan1) / 7) + 1;
}

/* ==================== 持久化 ==================== */
static bool ttSave() {
  JsonDocument doc;
  JsonArray arr = doc["c"].to<JsonArray>();
  for (int i = 0; i < gTtN; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["n"] = gTt[i].name;
    o["d"] = gTt[i].day;
    o["s"] = gTt[i].start;
    o["e"] = gTt[i].end;
    o["p"] = gTt[i].parity;
    if (gTt[i].global) o["g"] = 1;
  }
  File f = LittleFS.open("/tt.json", "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  return n > 0;
}

void ttBegin() {
  gTtN = 0;
  if (!LittleFS.exists("/tt.json")) return;
  File f = LittleFS.open("/tt.json", "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  for (JsonObject o : doc["c"].as<JsonArray>()) {
    if (gTtN >= TT_MAX) break;
    TtCourse c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", (const char *)(o["n"] | ""));
    c.day     = (uint8_t)(o["d"] | 0);
    c.start   = (uint16_t)(o["s"] | 0);
    c.end     = (uint16_t)(o["e"] | 0);
    c.parity  = (uint8_t)(o["p"] | 0);
    c.global  = (uint8_t)((o["g"] | 0) ? 1 : 0);
    if (!c.name[0] || c.day < 1 || c.day > 7) continue;
    if (c.start > c.end || c.end > 1439 || c.parity > 2) continue;   /* s==e 是事件 */
    gTt[gTtN++] = c;
  }

}

int  ttCount() { return gTtN; }
const TtCourse *ttGet(int i) { return (i >= 0 && i < gTtN) ? &gTt[i] : nullptr; }

/* ==================== 增删改 ==================== */
int ttAdd(const char *name, int day, int start, int end, int parity) {
  return ttAddEx(name, day, start, end, parity, 0);
}

int ttAddEx(const char *name, int day, int start, int end, int parity, int global) {
  if (!name || !*name) return -1;
  if (gTtN >= TT_MAX) return -1;
  if (day < 1 || day > 7) return -1;
  if (start < 0 || end < start || end > 1439) return -1;   /* end==start 是事件 */
  if (parity < 0 || parity > 2) return -1;

  /* 插入到按 (星期, 开始时间) 有序的位置，列表天然好看 */
  int pos = gTtN;
  for (int i = 0; i < gTtN; i++) {
    if (gTt[i].day > day || (gTt[i].day == day && gTt[i].start > start)) { pos = i; break; }
  }
  if (pos < gTtN)
    memmove(&gTt[pos + 1], &gTt[pos], sizeof(TtCourse) * (size_t)(gTtN - pos));
  TtCourse &c = gTt[pos];
  memset(&c, 0, sizeof(c));
  snprintf(c.name, sizeof(c.name), "%s", name);
  c.day = (uint8_t)day; c.start = (uint16_t)start; c.end = (uint16_t)end;
  c.parity = (uint8_t)parity;
  c.global = global ? 1 : 0;
  gTtN++;
  ttSave();
  return pos;
}

/* 把解析好的 DOM 落进 gTt：逐条校验、排序、整表落盘。
 * 任何一条不合法就跳过它（不整表失败），条数上限 TT_MAX。 */
static int ttCommit(JsonDocument &doc) {
  JsonArray arr = doc["c"].as<JsonArray>();
  int k = 0;
  for (JsonObject o : arr) {
    if (k >= TT_MAX) break;
    TtCourse c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", (const char *)(o["n"] | ""));
    c.day    = (uint8_t)(o["d"] | 0);
    c.start  = (uint16_t)(o["s"] | 0);
    c.end    = (uint16_t)(o["e"] | 0);
    c.parity = (uint8_t)(o["p"] | 0);
    c.global = (uint8_t)((o["g"] | 0) ? 1 : 0);
    if (!c.name[0] || c.day < 1 || c.day > 7) continue;
    if (c.start > c.end || c.end > 1439 || c.parity > 2) continue;   /* s==e 是事件 */
    gTt[k++] = c;
  }
  gTtN = k;

  for (int i = 1; i < gTtN; i++) {
    TtCourse v = gTt[i];
    int j = i - 1;
    while (j >= 0 && (gTt[j].day > v.day || (gTt[j].day == v.day && gTt[j].start > v.start))) {
      gTt[j + 1] = gTt[j];
      j--;
    }
    gTt[j + 1] = v;
  }
  ttSave();
  return gTtN;
}

/* 整表替换：先解析再写 gTt，最后统一排序落盘，避免写一半断电 */
int ttLoadJson(const char *json, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return -1;
  return ttCommit(doc);
}

/* 流式整表替换：文件当输入，边读边解，字符串由 ArduinoJson 各自分配 */
int ttLoadStream(Stream &s) {
  JsonDocument doc;
  if (deserializeJson(doc, s)) return -1;
  return ttCommit(doc);
}

bool ttDel(int i) {
  if (i < 0 || i >= gTtN) return false;
  if (i < gTtN - 1)
    memmove(&gTt[i], &gTt[i + 1], sizeof(TtCourse) * (size_t)(gTtN - i - 1));
  gTtN--;
  ttSave();
  return true;
}

void ttClear() {
  gTtN = 0;
  ttSave();
}

int ttFlipParity() {
  int changed = 0;
  for (int i = 0; i < gTtN; i++) {
    if (gTt[i].parity == 1)      { gTt[i].parity = 2; changed++; }
    else if (gTt[i].parity == 2) { gTt[i].parity = 1; changed++; }
  }
  if (changed) ttSave();
  return changed;
}

/* ==================== 查询 ==================== */
/* 由主固件提供，避免这里反向依赖 .ino 里的 static 变量 */
extern bool appNowYmd(int *y, int *m, int *d);

bool ttIsOddWeek() {
  int y = 0, m = 1, d = 1;
  if (!appNowYmd(&y, &m, &d)) return true;   /* 没读到 RTC 时按单周处理 */
  return (isoWeekNumber(y, m, d) & 1) != 0;
}

bool ttMatches(const TtCourse *c) {
  if (!c) return false;
  if (c->parity == 0) return true;
  bool odd = ttIsOddWeek();
  return (c->parity == 1) ? odd : !odd;
}

int ttDayList(int day, int *out, int max) {
  int n = 0;
  int  idx[TT_MAX];
  for (int i = 0; i < gTtN; i++)
    if (gTt[i].day == day && ttMatches(&gTt[i])) idx[n++] = i;
  /* 按开始时间插入排序（最多 60 条，够用） */
  for (int i = 1; i < n; i++) {
    int v = idx[i], j = i - 1;
    while (j >= 0 && gTt[idx[j]].start > gTt[v].start) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = v;
  }
  if (n > max) n = max;
  for (int i = 0; i < n; i++) out[i] = idx[i];
  return n;
}

int ttNowIdx(int day, int nowMin) {
  int idx[TT_MAX];
  int n = ttDayList(day, idx, TT_MAX);
  for (int i = 0; i < n; i++) {
    const TtCourse *c = &gTt[idx[i]];
    if (nowMin >= c->start && nowMin < c->end) return idx[i];
  }
  return -1;
}

int ttNextIdx(int day, int nowMin) {
  int idx[TT_MAX];
  int n = ttDayList(day, idx, TT_MAX);
  for (int i = 0; i < n; i++) {
    if (gTt[idx[i]].start > nowMin) return idx[i];
  }
  return -1;
}

bool ttDayEnd(int day, int *endMin) {
  int idx[TT_MAX];
  int n = ttDayList(day, idx, TT_MAX);
  if (n <= 0) return false;
  int e = 0;
  for (int i = 0; i < n; i++) if (gTt[idx[i]].end > e) e = gTt[idx[i]].end;
  if (endMin) *endMin = e;
  return true;
}
