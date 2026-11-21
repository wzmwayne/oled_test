/*
 * wifidbg.h - USB 串口 WiFi 调试命令
 *
 * 用法（串口里）：
 *   wifi                     列子命令
 *   wifi on|off|status       开关 / 状态
 *   wifi scan                扫描
 *   wifi list                已保存的 WiFi
 *   wifi conn <序号|SSID> [密码]
 *   wifi ping <主机> [次数]
 *   wifi curl [-I] <URL>
 *   wifi dns <主机>
 *   wifi ntp [服务器]
 *
 * 命令在独立任务里跑（10KB 栈），TLS 握手不会把主循环栈打爆。
 */
#pragma once
#include <stdint.h>

void wifiDbgBegin();               /* setup 调一次：注册断线原因捕获 */
bool wifiDbgRun(const char *args); /* args = "wifi" 后面的整串 */
void wifiDbgHelp();
const char *wifiDbgLastReason();   /* 最近一次 STA 断开原因，没发生过返回 "-" */
