/*
 * reader.h - 纯文本阅读器
 *
 * 文件放 LittleFS /books/*.txt（UTF-8），一页 4 行、每行 128px。
 * 只做三件事：按字宽排版、翻页、记住读到哪（字节偏移）。
 * 字形宽度由主固件通过 appCpWidth() 提供，这里不直接碰字库。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RD_LINES      4
#define RD_LINE_BYTES 64
#define RD_WIDTH      128

void rdInit();                        /* 读上次进度，mount 之后调用 */
bool rdOpen(const char *name);        /* 打开某本书并恢复进度 */
bool rdOpenLast();                    /* 打开上次读的那本，没有就打开第一本 */
void rdClose();
bool rdLoaded();
const char *rdName();
uint32_t rdSize();
uint32_t rdPos();                     /* 当前页起始字节偏移 */
int  rdPercent();                     /* 0..100 */
const char *rdLine(int i);            /* 0..RD_LINES-1，UTF-8 */
void rdNext();
void rdPrev();
bool rdNextBook();                    /* 循环切下一本，返回是否切换成功 */
void rdSaveProgress();
