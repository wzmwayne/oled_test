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

#define TT_MAX       60
#define TT_NAME_MAX  24     /* UTF-8 字节数，约 8 个汉字 */

typedef struct {
  char     name[TT_NAME_MAX];
  uint8_t  day;      /* 1=周一 .. 7=周日 */
  uint16_t start;    /* 距 00:00 的分钟数 */
  uint16_t end;
  uint8_t  parity;   /* 0=每周 1=单周 2=双周 */
} TtCourse;

void  ttBegin();                                  /* LittleFS 挂载后调用，读 /tt.json */
int   ttCount();
const TtCourse *ttGet(int i);

int   ttAdd(const char *name, int day, int start, int end, int parity);
bool  ttDel(int i);
void  ttClear();

/* 单双周：按 ISO 周数奇偶判定（奇数周=单周） */
bool  ttIsOddWeek();
bool  ttMatches(const TtCourse *c);               /* 该课本周是否要上 */

/* 某天的课，按开始时间排序，写索引到 out，返回条数 */
int   ttDayList(int day, int *out, int max);
int   ttNowIdx(int day, int nowMin);              /* 正在上的课，-1 无 */
int   ttNextIdx(int day, int nowMin);             /* 今日下一节，-1 无 */
bool  ttDayEnd(int day, int *endMin);             /* 今日最后一节的下课时间 */
