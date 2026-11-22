# BLE 配置协议

单服务、单特征、一问一答。手机连上即可读写，不做系统配对绑定；安全性由应用层 6 位 PIN 承担（错 3 次设备会断开并重启服务）。

| 项 | 值 |
|---|---|
| 设备名 | `OLED-Panel`（广播名以 `OLED` 开头，便于扫描过滤） |
| 服务 UUID | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| 特征 UUID | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| 特征属性 | READ / WRITE / WRITE_NR / NOTIFY |
| MTU | 设备接受 517；**客户端必须等 `onMtuChanged` 再收发**，否则 notify 会被截到 20 字节 |

## 报文

请求（写入特征，UTF-8 JSON）：

```json
{"pin":"483920","action":"settime","params":{"ts":1763860000}}
```

响应（notify，UTF-8 JSON）：

```json
{"ok":true,"action":"settime","msg":"时间已写入"}
```

失败统一 `{"ok":false,"action":"...","msg":"中文原因"}`。

单条响应必须塞进一次 notify（MTU 517 → 514 字节），因此列表类接口都要分页。

## 命令总表

| action | 请求 params | 响应 params |
|---|---|---|
| `ping` | — | — (`msg`=`pong`) |
| `get` | — | `tz` `tt_count` `week_parity` `fs_free` |
| `settz` | `tz`(-12..14) | — |
| `settime` | `ts`（UTC 秒） | — |
| `gettime` | — | `ts`（UTC 秒） |
| `unpair` | — | —（清配对并重启服务，连接会断） |
| `fsstat` | — | `total` `used` `free`（LittleFS 字节）`heap` |
| `blist` | — | `count` `books:[{name,size}]` |
| `bput` | `name` `off` `data`(base64，原文 ≤256B) | `next` `size` |
| `bdel` | `name` | — |
| `gettt` | — | `count` `today`(1..7) `week_parity` |
| `settt` | `data`(分片) / `end` / `reset` | `len` 或 `count` |
| `listtt` | `from`(≥0) `count`(1..6，固件实际按 ≤4 截断) | `total` `from` `courses:[{name,day,start,end,parity}]` |
| `addcourse` | `name`(≤24 字节 UTF-8) `day`(1..7) `start`(0..1439) `end`(0..1439) `parity`(0/1/2) | `index` `count` |
| `delcourse` | `index` | `count` |
| `fliptt` | — | `changed` `count` |
| `cleartt` | — | `count`(=0) |

## 书籍上传（阅读器）

纯文本放 LittleFS `/books/*.txt`（UTF-8）。因为一次 notify 装不下整本书，按块上传：

1. 客户端从 `off=0` 开始，每包原文字节数 **≤256**，base64 后约 342 字符；
2. 每包回包里的 `next` 就是下一包的 `off`，`size` 是当前文件长度；
3. 断点续传直接用已有的 `off`，服务端是随机写入，不依赖会话状态。

```
{"pin":"123456","action":"bput","params":{"name":"三体.txt","off":0,"data":"5LiJ5L2T"}}
{"ok":true,"action":"bput","params":{"next":256,"size":256}}
```

文件名只允许 ≤32 字节，不能含 `/`、`\`、`:`；单次 `blist` 最多回 8 本。

上传前建议先发一次 `fsstat`，确认 `free` 大于文件大小（留 4KB 余量）：

```
{"pin":"123456","action":"fsstat"}
{"ok":true,"action":"fsstat","params":{"total":1835008,"used":327680,"free":1507328,"heap":198432}}
```

## 发课表（整表覆盖）

一次 BLE 写放不下整张表，所以分三步，全部走同一个 `settt`：

1. `{"settt":{"reset":true}}` —— 清空暂存区（可选，第一片之前发一次更保险）
2. `{"settt":{"data":"<JSON 片段>"}}` —— 反复追加，每片 ≤300 字符，回包 `len` 是累计长度
3. `{"settt":{"end":true}}` —— 解析并整表替换，回包 `count` 是写入条数

载荷格式与设备内 `/tt.json` 相同，时间是**距 00:00 的分钟数**：

```json
{"c":[{"n":"数学","d":1,"s":530,"e":570,"p":0}]}
```

总大小上限 4KB（约 60 门课）；超过会返回错误并清空暂存区。
解析失败时设备**保持原课表不变**，不会写坏。

## 课表语义

- `day`：1=周一 … 7=周日。
- `start` / `end`：距当天 00:00 的分钟数，如 8:00 = 480，9:40 = 580。必须 `start < end`。
- `parity`：0=每周都上，1=单周，2=双周。
- 单双周按 **ISO 8601 周数**判定：奇数周为单周。设备首次上电 RTC 未校时时按单周处理。
- 容量：最多 60 条，保存时自动按 (星期, 开始时间) 排序，`index` 以排序后为准。
- 存储：LittleFS `/tt.json`，改一次写一次，掉电不丢。
- **`fliptt` 的意义**：单双周按 ISO 周数判定，若学校口径与之整体反相（开学第一周不是 ISO 奇数周），
  调一次 `fliptt` 即可把所有单周课与双周课对调，不需要逐条重录。每周课（parity=0）不受影响。

## 示例

```
{"pin":"123456","action":"addcourse","params":{"name":"高等数学","day":3,"start":480,"end":580,"parity":0}}
{"ok":true,"action":"addcourse","msg":"已保存","params":{"index":2,"count":7}}

{"pin":"123456","action":"listtt","params":{"from":0,"count":4}}
{"ok":true,"action":"listtt","params":{"total":7,"from":0,"courses":[
  {"name":"高等数学","day":1,"start":480,"end":580,"parity":0},
  {"name":"大学英语","day":1,"start":600,"end":690,"parity":1}]}}
```
