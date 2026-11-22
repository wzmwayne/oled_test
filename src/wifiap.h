/*
 * wifiap.h - WiFi 配置热点（设备自开 AP + 网页配置）
 *
 * 设备不连路由器、不做 NTP：进入本页时自己开热点，手机连上来打开
 * http://192.168.4.1 就能传书、导课表、对时间。相比 BLE 分块，HTTP
 * 直传省掉了 base64 与 MTU 碎片，几十 MB 的书也是一次拖进去。
 *
 * 生命周期：enterApp(APP_WEBAP) -> waBegin()；离开 -> waEnd()
 * 密码每次进来重新随机 8 位（数字 + 小写字母），屏幕上看得到。
 */
#pragma once
#include <stdint.h>

void waBegin();               /* 生成密码、开 AP、起 HTTP 服务 */
void waEnd();                 /* 停服务并关热点 */
void waPoll();                /* 主循环里调用 */
bool waRunning();
const char *waSsid();
const char *waPass();
const char *waIp();           /* 一般为 192.168.4.1 */
int  waClients();             /* 当前连上的手机数 */
uint32_t waUpBytes();         /* 本次会话累计上传字节 */
int  waUpFiles();             /* 本次会话成功上传的文件数 */
const char *waLast();         /* 最近一条操作结果，给屏幕第 4 行用 */
void waPrintInfo();           /* 串口 `ap`：打印热点实际运行参数 */
void waSelfTest();            /* 串口 `ap selftest`：自己扫自己，判断发射通不通 */

/* 现场排查用：换信道 / 临时开成开放网络（保留当前密码），都会重启热点 */
void waSetChannel(int ch);    /* 1..13 */
void waSetOpen(bool open);
void waSetProto(unsigned char proto);   /* 见下面三个常量 */
#define WA_PROTO_ALL 0   /* 11b/g/n，驱动默认 */
#define WA_PROTO_B   1   /* 只 11b：最低速 DSSS，最容易被扫到 */
#define WA_PROTO_BG  3   /* 11b + 11g */
int  waChannel();
bool waIsOpen();
