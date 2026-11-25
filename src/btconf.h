/*
 * btconf.h - 蓝牙配置模块
 *
 * 两个特征：命令通道走 JSON（一问一答），数据通道走纯二进制（传书）。
 *   手机 WRITE  {"pin":"483920","action":"settz","params":{"tz":8}}
 *   设备 NOTIFY {"ok":true,"action":"settz","msg":"时区已保存"}
 * 全部动作见 docs/ble-protocol.md。
 *
 * 生命周期：进入蓝牙配置页 -> btBegin()；离开 -> btEnd()
 * PIN 每次 btBegin/btClearBonds 时重新生成 6 位随机数
 */
#pragma once
#include <stdint.h>
#include <stddef.h>   /* size_t */

void btBegin();          /* 启动 BLE，生成新 PIN，开始广播 */
void btEnd();            /* 关闭 BLE 并释放内存（deinit true） */
void btPoll();           /* 主循环里调用，处理状态变化 */
bool btIsConnected();
bool btStarting();           /* 正在初始化，界面显示加载动画 */
const char *btStateText();   /* "等待连接" / "已连接" / "已停止" */
const char *btPinText();     /* 当前 6 位 PIN */
void btClearBonds();         /* 清全部配对 + 重启服务 + 换 PIN */
uint32_t btPinErrors();      /* PIN 错误次数，用于诊断 */

/* ---- 时区（NVS "cfg"）----
 * 设备不联网，只用它把手机给的 UTC 时间戳换算成本地时间写进 DS3231。 */
int  btCfgGetTz();               /* -12..14，默认 8 */
void btCfgSetTz(int tz);

/* ---- 由主固件（oled_test.ino）提供 ---- */
extern bool appNowYmd(int *y, int *m, int *d);   /* 本地日期，课表算 ISO 周用 */
extern void appToast(const char *msg, uint32_t ms);
extern bool appRtcWriteUnix(uint32_t ts);
extern bool appRtcReadUnix(uint32_t *ts);

