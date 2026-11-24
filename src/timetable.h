/*
 * timetable.h - 课表存储与查询
 *
 * 数据量很小（最多 60 条），整体以 JSON 存在 LittleFS 的 /tt.json：
 *   {"c":[{"n":"高等数学","d":3,"s":480,"e":580,"p":0}, ...]}
 *   n=课程名  d=星期(1..7)  s/e=距 00:00 的分钟  p=0每周 1单周 2双周
 *
 * 为什么用 LittleFS 而不是 NVS：NVS 单个 String 有长度上限，且课表是
 * 整表覆盖写，放文件里更直观、也方便以后导出来看。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TT_MAX       72     /* 12 个节次 x 6 天也放得下；含午自习/午休/午餐 */
#define TT_NAME_MAX  24     /* UTF-8 字节数，约 8 个汉字 */

typedef struct {
  char     name[TT_NAME_MAX];
  uint8_t  day;      /* 1=周一 .. 7=周日 */
  uint16_t start;    /* 距 00:00 的分钟数 */
  uint16_t end;      /* start == end 表示"事件"（单点） */
  uint8_t  parity;   /* 0=每周 1=单周 2=双周 */
  uint8_t  global;   /* 事件才有意义：1=全局（追加在主页行尾） 0=普通 */
} TtCourse;

void  ttBegin();                                  /* LittleFS 挂载后调用，读 /tt.json */
int   ttCount();
const TtCourse *ttGet(int i);

/* 整表替换：解析 {"c":[{"n","d","s","e","p"}]}，成功返回条数，失败 -1。
 * 这是「发课表」的唯一入口，解析成功后一次性落盘。 */
int   ttLoadJson(const char *json, size_t len);

int   ttAdd(const char *name, int day, int start, int end, int parity);
/* end==start 表示事件（单点）；global=1 时该事件追加在主页行尾 */
int   ttAddEx(const char *name, int day, int start, int end, int parity, int global);
bool  ttDel(int i);
void  ttClear();

/* 全体单双周互换（1<->2，每周不动），返回改动条数。
 * 用途：单双周按 ISO 周数判定，学校口径若整体反相，开学第一周点一下即可对齐。 */
int   ttFlipParity();

/* 单双周：按 ISO 周数奇偶判定（奇数周=单周） */
bool  ttIsOddWeek();
bool  ttMatches(const TtCourse *c);               /* 该课本周是否要上 */

/* 某天的课，按开始时间排序，写索引到 out，返回条数 */
int   ttDayList(int day, int *out, int max);
int   ttNowIdx(int day, int nowMin);              /* 正在上的课，-1 无 */
int   ttNextIdx(int day, int nowMin);             /* 今日下一节，-1 无 */
bool  ttDayEnd(int day, int *endMin);             /* 今日最后一节的下课时间 */
