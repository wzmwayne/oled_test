/*
 * reader.h - 离线阅读器
 *
 * 文件放 LittleFS /books/*.ezzbk（由 neobook 或 tools/ezbk2ezzbk.py 生成）。
 * 进入阅读器先列书目，选中才打开；书名优先取容器元数据里的 title。
 * 进度按文件分别保存（NVS "rd" 命名空间的 idx 里存 name->偏移 的 JSON），
 * 文件被删掉时对应进度一并清除。
 * 字形宽度由主固件通过 appCpWidth() 提供，这里不直接碰字库。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RD_LINES      4
#define RD_LINE_BYTES 64
#define RD_WIDTH      128
#define RD_BOOK_MAX   16
#define RD_NAME_MAX   40
#define RD_TITLE_MAX  44     /* ezzbk 元数据里的书名，UTF-8 字节 */

void rdInit();                        /* mount 之后调用：建目录、扫书目、清孤儿进度 */
void rdScan();                        /* 重新扫描 /books，刷新列表与各书进度 */

int  rdBookCount();
const char *rdBookName(int i);
/* 书名：优先用 ezzbk 元数据里的 title，没有就退回文件名 */
const char *rdBookTitle(int i);
int  rdBookPercent(int i);            /* 0..100，没有进度就是 0 */
/* 直接读某本书容器里的书名（不依赖书目扫描），没有就留空串。蓝牙书目用 */
void rdBookMetaTitle(const char *name, char *title, size_t tn);

bool rdOpenIndex(int i);              /* 打开第 i 本，恢复它自己的进度 */
bool rdLoaded();
const char *rdName();
uint32_t rdSize();
int  rdPercent();
const char *rdLine(int i);
uint8_t  rdLineLevel(int i);          /* 0=正文 1=# 卷标题 2=## 章标题 */
uint32_t rdLineOffset(int i);         /* 该行对应的正文偏移 */
int      rdCurrentTocIndex();         /* 当前章节在目录里的下标 */
void rdNext();
void rdPrev();
void rdSaveProgress();
void rdForget(const char *name);      /* 文件被删时清掉它的进度 */

/* ---- 目录（.ezb 里的 # / ## 行）---- */
int  rdTocCount();
const char *rdTocName(int i);
bool rdGotoToc(int i);                /* 跳到第 i 章开头 */
