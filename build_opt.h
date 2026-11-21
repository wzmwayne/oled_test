/*
 * arduino-cli / IDE 会把这个文件的内容当作额外编译参数。
 * CORE_DEBUG_LEVEL 越低门槛越详细：0=关闭 1=错误 2=警告 3=信息 4=调试 5=最详细
 * 当前设为 5，让 Arduino 核心与 WiFi 库的 log_e/log_w 全部打出来，
 * 排查 wifi conn 连不上时能看到驱动内部的失败原因。
 * 定位完成后把这一行改回 0，否则日志量很大。
 */
-DCORE_DEBUG_LEVEL=5
