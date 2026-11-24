# BLE 配置协议

一个服务、两个特征：**JSON 命令通道**（一问一答）和**二进制数据通道**（传书用）。
手机连上即可读写，不做系统配对绑定；安全性由应用层 6 位 PIN 承担（错 3 次设备断开并重启服务）。

| 项 | 值 |
|---|---|
| 设备名 | `OLED-Panel`（扫 `OLED` 前缀） |
| 服务 UUID | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| 命令特征 | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` — WRITE / WRITE_NR / NOTIFY / READ |
| 数据特征 | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` — WRITE / WRITE_NR（纯二进制） |
| MTU | 设备接受 517；**客户端必须等 `onMtuChanged` 再收发**，否则 notify 会被截到 20 字节 |

## 报文

请求（写命令特征，UTF-8 JSON）：

```json
{"pin":"483920","action":"settime","params":{"ts":1763860000}}
```

响应（notify，UTF-8 JSON）：

```json
{"ok":true,"action":"settime","msg":"时间已写入"}
```

失败统一 `{"ok":false,"action":"...","msg":"中文原因"}`。
单条响应必须塞进一次 notify（MTU 517 → 514 字节），所以列表类接口都要分页。

## 命令总表

| action | 请求 params | 响应 params |
|---|---|---|
| `ping` | — | —（`msg`=`pong`） |
| `get` | — | `tz` `tt_count` `week_parity`(1=单周 2=双周) `fs_free` |
| `settz` | `tz`(-12..14) | — |
| `settime` | `ts`（UTC 秒） | — |
| `gettime` | — | `ts`（UTC 秒） |
| `unpair` | — | —（清配对并重启服务，连接会断） |
| `fsstat` | — | `total` `used` `free`（LittleFS 字节）`heap` |
| `blist` | — | `count` `books:[{name,size,t?}]`，最多 16 本 |
| `bopen` | `name` `size` `chunk`(4096..262144) | `chunk`（实际采用的段长） |
| `bsync` | — | `len` `hash` `expect`（当前进度，等 ack 超时时用） |
| `bbad` | — | `len` `hash`（回退到上一次确认的位置） |
| `bclose` | `size` `hash` | `size` `hash`（整本校验） |
| `bcancel` | — | —（放弃上传，删掉半截文件） |
| `bdel` | `name` | —（同时清掉该书阅读进度） |
| `bput` | `name` `off` `data`(base64，原文 ≤256B) | `next` `size` |
| `gettt` | — | `count` `today`(1..7) `week_parity` |
| `settt` | `data`(分片) / `end` / `reset` | `len` 或 `count` |
| `listtt` | `from`(≥0) `count`(1..4) | `total` `from` `courses:[{name,day,start,end,parity,global,event}] |
| `addcourse` | `name`(≤24 字节 UTF-8) `day`(1..7) `start` `end`(0..1439) `parity`(0/1/2) `global`(0/1) | `index` `count` |
| `delcourse` | `index` | `count` |
| `fliptt` | — | `changed` `count` |
| `cleartt` | — | `count`(=0) |

## 传书（二进制通道）

书只有一种格式：`.ezzbk`（容器规范见 neobook 的 `notes/ezzbk-spec-v0.1.md`）。
手机端只做校验后原样上传，**不在手机上打包**。

1. `bopen` 声明 `name` / `size`（字节）/ `chunk`（段长，默认 32768）。
   设备建文件、算空间，回实际段长；名字必须 ≤32 字节、非空、不含 `/ \ :`、后缀 `.ezzbk`。
2. 把文件切块（每块 `min(MTU-3, 512)` 字节）连写数据特征，**无应答写**，设备边收边写盘。
3. 设备每收满一段（或收满 `size`）就在命令特征上 notify：

   ```json
   {"ok":true,"action":"bputack","params":{"len":32768,"hash":2166136261}}
   ```

   手机核对自己的 `len` / `hash`（**FNV-1a 32 位**，初值 2166136261，按文件顺序累计）：
   - 对得上 → 继续发下一段；
   - 对不上 → `bbad` 让设备回退到上一次确认的位置，再重发；
   - 一直等不到 ack → `bsync` 问一次当前进度再决定。
4. 全部发完：`bclose` 带 `size` 与 `hash` 做整本校验，通过才保留文件，
   否则整本删掉（不留半截书）。`bclose` 之后旧进度作废、书目重新扫描。

中途放弃发 `bcancel`：关句柄并删掉半截文件。
`bput` 是旧的 base64 分块接口，慢一个数量级，只在调试时用。

## 书目

`blist` 返回 `/books` 下的每个文件：

```json
{"ok":true,"action":"blist","params":{"count":1,"books":[
  {"name":"三体.ezzbk","size":412345,"t":"三体"}]}}
```

- `t` 是容器元数据里的书名，没有这个字段时客户端显示 `name`。
- `bdel` 按 `name` 删除；删除成功会同时清掉该书的阅读进度。

## 发课表（整表覆盖）

一次 BLE 写放不下整张表，所以分三步，全部走同一个 `settt`：

1. `{"settt":{"reset":true}}` —— 清空暂存区（第一片之前发一次更保险）
2. `{"settt":{"data":"<JSON 片段>"}}` —— 反复追加，每片 ≤300 字符，回包 `len` 是累计长度
3. `{"settt":{"end":true}}` —— 解析并整表替换，回包 `count` 是写入条数

载荷格式与设备内 `/tt.json` 相同，时间是**距 00:00 的分钟数**：

```json
{"c":[
  {"n":"数学","d":1,"s":530,"e":570,"p":0},
  {"n":"午餐","d":1,"s":730,"e":730,"g":1}
]}
```

总大小上限 6KB（约 70 条）——超过会返回错误并清空暂存区；
解析失败时设备**保持原课表不变**，不会写坏。

## 课表语义

- `d`：1=周一 … 7=周日。
- `s` / `e`：距当天 00:00 的分钟数，如 8:00 = 480、9:40 = 580。必须 `0 ≤ s ≤ e ≤ 1439`。
- `s == e` 表示**事件**（单点时间，如「12:10 午餐」）：不会当「正在上」，列表里显示成 `· 12:10 午餐`。
- `g=1` 表示**全局事件**：每天都算，只追加在主页滚动条末尾，不进课表列表的时间线。
- `p`：0=每周，1=单周，2=双周。单双周按 **ISO 8601 周数**判定，奇数周为单周；
  RTC 未校时时按单周处理。
- 容量：最多 **72 条**，保存时按 (星期, 开始时间) 排序，`index` 以排序后为准。
- 存储：LittleFS `/tt.json`，改一次写一次，掉电不丢。
- **`fliptt` 的意义**：若学校口径与 ISO 整体反相（开学第一周不是奇数周），
  调一次即可把所有单周课与双周课对调，不需要逐条重录；每周课（`p=0`）不受影响。

## 示例

```
{"pin":"123456","action":"addcourse","params":{"name":"高等数学","day":3,"start":480,"end":580,"parity":0}}
{"ok":true,"action":"addcourse","msg":"已保存","params":{"index":2,"count":7}}

{"pin":"123456","action":"listtt","params":{"from":0,"count":4}}
{"ok":true,"action":"listtt","params":{"total":2,"from":0,"courses":[
  {"name":"高等数学","day":1,"start":480,"end":580,"parity":0,"global":0,"event":0},
  {"name":"午餐","day":1,"start":730,"end":730,"parity":0,"global":1,"event":1}]}}
```
