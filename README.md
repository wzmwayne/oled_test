# OLED 128x64 连接测试（ESP32-C3）

零依赖固件，只用 ESP32 自带的 `Wire`。点阵字库与 Orange Pi 面板工程完全同一套
（文泉驿微米黑 14px）。

## 接线

| OLED | ESP32-C3 | 说明 |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SCL | **GPIO7** | 时钟 |
| SDA | **GPIO6** | 数据 |

GPIO6 / GPIO7 在 C3 上都是空闲脚（C3 的 Flash 走 GPIO12–17）。

## 编译烧录

> **默认走 GitHub CI**：本地编译（`refresh.sh` 的编译段）只用于排障，日常一律
> `git push` → 等 `build` workflow（约 1m45s）→ `./getfw.sh` 把产物拉到
> `/sdcard/Download`。安卓 App 同理：`android/` 有改动时 `android` workflow 出
> `oled-panel.apk`，用 `./getapk.sh` 拉到 `/sdcard/Download`。

**fqbn 后面必须带 `:CDCOnBoot=cdc`**，否则板载 Type-C 看不到任何串口输出
（C3 默认 `Serial` = UART0 GPIO20/21，与原生 USB 不通）。

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc ~/Arduino/oled_test
arduino-cli upload  -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc ~/Arduino/oled_test
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

实测占用：**465 KB flash（35%）/ 16 KB RAM（4%）**，其中字库 128 KB 在 flash，不占 RAM。

## 串口命令

固件运行中，在串口里输入命令并回车：

| 命令 | 作用 |
|---|---|
| `info` | 打印全部运行信息（芯片 / 屏幕 / I2C / 字库 / 内存 / 刷新率） |
| `help` 或 `?` | 命令列表 |

`info` 输出示例：

```
========== 运行信息 ==========
芯片        : ESP32-C3 rev3
CPU         : 160 MHz
Flash       : 4 MB
SDK         : v5.x.x
USB CDC     : 已启用 (Serial=USB)
构建时间    : Nov 17 2026 22:58:00
---- 屏幕 ----
控制器      : SSD1306
分辨率      : 128x64
I2C 地址    : 0x3C
SDA / SCL   : GPIO6 / GPIO7
I2C 时钟    : 400000 Hz
分块大小    : 64 字节
I2C 错误    : 0
刷新率      : 41 FPS
总线上设备  : 1 个 0x3C
---- 字库 ----
字形数      : 4015
单字形      : 32 字节
字库总量    : 125 KB (flash)
---- 运行 ----
运行时间    : 37 秒
自由堆      : 311000 B / 最小 310000 B
==============================
```

## 两个已修的坑

### 花屏（大部分乱、顶部一小条正常）

ESP32 的 Wire 发送缓冲默认只有 **128 字节**：

```c
// libraries/Wire/src/Wire.h:48
#define I2C_BUFFER_LENGTH 128
```

一帧 1024 字节一次性 `Wire.write()` 会被截断，只有最前面一两页数据真正发出去，
剩余页保留 OLED 上电时的随机内容。

修法：按 64 字节分块发送（`I2C_CHUNK`），并统计错误；若 400kHz 下有错自动降到 100kHz 重来。

### 串口没回显

`esp32c3.menu.CDCOnBoot` 默认 Disabled → `Serial` 是 UART0。
板载 Type-C 接的是原生 USB-Serial-JTAG，所以什么都收不到。

修法：编译带 `:CDCOnBoot=cdc`；固件同时把日志镜像一份到 UART0，两种接法都能看。

## 屏幕依次显示

| 顺序 | 画面 | 检查什么 |
|---|---|---|
| 1 | 「OLED 128x64 / 全屏点亮」+ 全白 | 供电、电荷泵、整行整列坏点 |
| 2 | 「全屏熄灭」+ 全黑 | 关断干净度、残影 |
| 3 | 「棋盘格」+ 2x2 棋盘 | 行列寻址、有无斜切错位 |
| 4 | 「进度条」+ 左下到右上 | 每列每行独立可控 |
| 5 | 「竖条纹」+ 4px 竖条 | 列驱动均匀性 |
| 6 | 「边框对角线」 | 整体几何 |
| 7 | 循环信息页 | 中文字库、刷新率 |

## 课表

数据存 LittleFS 的 `/tt.json`，最多 60 条，字段只有课程名、星期、起止时刻、单双周。

| 操作 | 按键 |
|---|---|
| 进课表页 | 主页 `#` 进菜单（第 5 项）或长按 `#` 输编号 `111` |
| 上下翻课 | `0` / `1`（可长按连发） |
| 换看星期几 | `#` 短按循环「今天 → 周一 … 周日」，`#` 长按回到今天 |
| 回主页 | `*` 短按 |

主页第 4 行会走马灯显示当前状态（放得下就居中不动）：

```
正在上 高等数学 剩23分 · 16:40放学
下节 高等数学 14:05 · 还有45分 · 16:40放学
今日课已结束 · 16:40放学
今日无课
```

单双周按 **ISO 周数**奇偶判定（奇数周=单周），不需要配置学期起始日。
课表全部通过 BLE 录入，接口见 [docs/ble-protocol.md](docs/ble-protocol.md)。

## 字库

`tools/font_wqy14.h`，4015 字形，125 KB：ASCII + CJK 标点 + 全角 + GB2312 一级汉字。

光栅化参数与 Pi 工程的 `tools/genfont.py` 逐行一致
（`SIZE=14 ROWS=14 BASELINE=12 PAD=3 阈值>96`），已交叉校验 60 个字形
与 `glyphs_p00.go` **逐像素完全相同**。

重新烘：

```bash
cd ~/Arduino/oled_test/tools
python3 genfont_c.py                    # 默认 4015 字
python3 genfont_c.py --full             # 全 CJK，约 670 KB
python3 genfont_c.py --chars "测试屏幕"   # 只要这些字
```

渲染与 Pi 版 `OLED.Text` 一致：前进量 = 字宽，无额外字距；未收录字符画空心方框并前进 8 像素。

## 故障判读

| 现象 | 结论 |
|---|---|
| 串口完全没有输出 | fqbn 漏了 `:CDCOnBoot=cdc` |
| 仍花屏但 `info` 里 I2C 错误 = 0 | 多半是 SH1106，改 `OLED_CONTROLLER` |
| `info` 里 I2C 错误 > 0 | 电气问题：缩线、查上拉、外接 4.7k |
| 整屏左移 2 像素 | SH1106 |
| 只亮上半屏 / 上下重复 | 是 128x32 不是 128x64 |
| 扫不到设备 | 接线 / 模块是 SPI 版 |

## 目录

```
oled_test/
├── oled_test.ino          固件（界面/按键/OLED/DS3231/NTP）
├── src/
│   ├── btconf.cpp/.h      BLE 配置服务
│   ├── timetable.cpp/.h   课表存储与查询
│   └── font_wqy14.h       字库声明（数据在 font 分区）
├── android/               手机端配置 App（Kotlin）
├── docs/
│   ├── ble-protocol.md    BLE 协议（App/固件共用契约）
│   └── ble.html           网页版调试器（已由 App 取代，保留备用）
├── tools/genfont_c.py     字库生成器
├── refresh.sh             本地编译+合并整包（排障用）
├── getfw.sh               拉最新固件到 /sdcard/Download
└── getapk.sh              拉最新 App 到 /sdcard/Download
```
