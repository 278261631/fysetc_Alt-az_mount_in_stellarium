# Stellarium 23.4 直连 LX200 协议规格（Fysetc Alt-az 固件对照版）

> 研究依据：Stellarium 官方仓库 tag **v23.4** 源码（与 `E:\Stellarium\stellarium.exe` 文件版本 23.4.0 对应）。
> 所有结论均标注 `文件:行号`，可自行核对。源码目录约定：
> - `Lx200/` = `plugins/TelescopeControl/src/Lx200/`
> - `common/` = `plugins/TelescopeControl/src/common/`
> - `TC.cpp` = `plugins/TelescopeControl/src/TelescopeControl.cpp`
> - `TCDL.cpp` = `Lx200/TelescopeClientDirectLx200.cpp`
>
> 结论先行（会直接影响固件设计的 5 个点）：
> 1. **串口参数硬编码 9600 8N1、无流控、不可配置** → 固件运行期必须用 9600。（不是 115200！）
> 2. **没有任何握手/产品名查询**（无 `:GVP#`/`:GVN#`），打开串口后立刻轮询 `#:GR#`/`#:GD#`。
> 3. **没有任何时间/地点同步命令**（无 `:St`/`:Sg`/`:SL`/`:SG`/`:SC`/`:Sz`）→ 固件必须自己有时间+站点坐标。
> 4. Stellarium 认为望远镜报的是**赤道坐标 RA/Dec**（可选 J2000/JNow），**没有地平坐标选项** → 固件必须做 alt/az ↔ RA/Dec 换算。
> 5. 固件在 LX200 串口上**绝对不能输出任何非应答字节**（banner/日志/异步消息），否则会触发 Stellarium 清队列并静默 10 秒。

---

## 1. 版本与源码依据

| 事实 | 依据 |
|---|---|
| `E:\Stellarium\stellarium.exe` = 23.4.0 | 文件属性 FileVersion |
| 插件版本 0.4.2，配置版本 **0.4.1** | `plugins/TelescopeControl/CMakeLists.txt`（`TELESCOPE_CONTROL_CONFIG_VERSION="0.4.1"`） |
| 直连 LX200 类 = `TelescopeClientDirectLx200` | `TelescopeClient.cpp:92-95`（`type=="TelescopeServerLx200"`） |
| 设备模型 "Meade LX200 (compatible)" → `TelescopeServerLx200` | `plugins/TelescopeControl/resources/device_models.json` |
| 望远镜列表存 `telescopes.json`（不是 config.ini） | `TC.cpp:696-1028` |

---

## 2. 串口参数与打开行为（关键）

`common/SerialPort.cpp`：

- Windows：`CreateFileW(..., GENERIC_READ|GENERIC_WRITE, 0, ...)`（第 44 行）——**共享模式 0，独占打开**：只要 Stellarium 连着，任何其他程序（含 Lead 的串口工具、Marlin 监控）都打不开 COM7。
- 波特率/帧格式硬编码：`BuildCommDCBW("9600,n,8,1")`（第 75 行）；Linux 侧 `B9600`、`CS8`、无校验（第 123-131 行）。
- 没有 RTS/CTS 流控设置；读超时被设为「立即返回」：`ReadIntervalTimeout = MAXDWORD`、总超时 0（第 52-57 行）。
- 关闭串口时恢复原始 DCB（第 155 行）。
- 插件里**没有任何 baud/流控的 GUI 或 JSON 配置项**（全插件源码只有上述两处 9600）。

> **对固件的硬要求：LX200 串口 `9600 8N1`，无流控。**
> 烧录用 115200 与运行期无关（esptool 自会切换）；不要在固件里把 LX200 UART 设成 115200。
> 若固件用 `Serial`（UART0，经 CH340 到 COM7），运行时 `Serial.begin(9600, SERIAL_8N1)`。
>
> 附带风险：Windows 打开串口时 `SetCommState` 可能改变 DTR/RTS 电平，FYSETC E4（Marlin 板）常见自动复位电路可能因此复位 ESP32；boot ROM 的启动输出以 115200 发出、在 9600 下会变成乱码被 Stellarium 读到 → 可能触发一次 10 秒静默后自动恢复（见 §4）。验收时点 Start 后至少等 20 秒。

---

## 3. 连接时间线（Stellarium 视角，逐字节）

### 3.1 打开连接时**没有**握手

`TCDL.cpp:41-103`（构造函数）只做三件事：
1. 用正则 `^([^:]*):(\d+)$` 拆参数串 `"<串口名>:<delay微秒>"`（第 56-69 行）；
2. 校验 delay ∈ (0, 10,000,000] 微秒（第 74-78 行）；
3. `new Lx200Connection(*this, serialDeviceName)` 打开串口（第 88 行），失败则打印 qWarning 并放弃。

**没有** `:GVP#`、`:GVN#`、`:X#` 之类的探测；**没有**重试；**没有**时间/地点下发。
（`E:\Stellarium\log.txt` 中只会看到 `TelescopeClientDirectLx200 parameters: port, time_delay: COM7 500000` 之类 debug 行。）

### 3.2 打开成功后立刻开始轮询（t≈0）

`Lx200Connection` 构造：`time_between_commands=0`、`next_send_time=GetNow()`（`Lx200Connection.cpp:34-40`）→ 无初始延迟。
`TCDL.cpp:235-246 step()`：`queue_get_position=true; next_pos_time=-inf` → 第一次调用立刻发：

```
发送: "#:GR#"          （5 字节，见 §5.1）
等待: "HH:MM:SS#"      （9 字节）
发送: "#:GD#"
等待: "+DD<0xDF>MM:SS#"（10 字节）
```
收到 `:GD#` 应答后 `queue_get_position=true`，并 `next_pos_time = now + 500000µs`（`TCDL.cpp:232,243`）。
轮询节拍由 `TelescopeClientDirectLx200::step()` 的 500 ms 固定值决定，**不可配置**；配置里的 `delay` 只用于位置插值补偿，不改变轮询周期。

### 3.3 goto / sync 触发的命令序列

`telescopeGoto()` → `gotoReceived()` → `Lx200Connection::sendGoto()`（`Lx200Connection.cpp:69-99`）：
```
"#:Q#"              ← 先停止当前运动（needsNoAnswer）
":SrHH:MM:SS#"      ← 设置选中目标 RA (J2000 或 JNow，取决于 JSON 的 equinox)
":Sd+DD<0xDF>MM:SS#"← 设置选中目标 Dec
":MS#"              ← 执行 goto
```
`telescopeSync()` 同样先发 `#:Q#`、`:Sr`、`:Sd`，但最后一条是 `:CM#`（`Lx200Connection.cpp:101-131`）。

> 注意 `goto_commands_queued <= 1` 的限流（第 71 行）：上一条 goto 的 `:MS#` 应答没收到之前，新的 goto 会被**静默丢弃**。固件必须及时回 `:MS#`。

### 3.4 超时、重试与 10 秒静默惩罚（验收最容易踩的坑）

`Lx200Connection.cpp`：

| 情况 | 行为 | 行号 |
|---|---|---|
| 普通命令（GR/GD/Sr/Sd/CM）应答超时 | **5 秒** | 第 165-174 行 |
| `:MS#` 应答超时 | **3 秒** | 第 167-169 行 |
| 超时且已收到部分短应答（仅 `:MS#`/`:CM#` 的首字节） | 丢弃该命令，继续 | 第 271-289 行 |
| 其它超时 / 收到无法解析的应答（`readAnswerFromBuffer` 返回 -1） | `resetCommunication()`：清空队列 + **接下来 10 秒不发送任何命令**，然后重新开始轮询 | 第 45-64、290-294 行 |
| 收到数据但队列为空且已过发送时刻 | 同样 `resetCommunication()`（惩罚 10 秒） | 第 194-210 行 |

**因此固件必须：**
- 对每个查询/命令在 1 秒内完整应答（建议 <100 ms）；
- 绝不输出主动消息（banner、调试、状态推送、换行、回显）；
- 解析器要能吃掉杂字节（前导 `#`、`\r`、`\n`、开机残字节），只在看到 `':'` 开头的完整命令时动作；
- 测量/调试信息请走自定义扩展命令（如 `:XG#`），**不要**主动打印。

### 3.5 连接状态判定

`TCDL.cpp:248-256`：`isConnected()/isInitialized()` 都只是 `!lx200->isClosed()`。
即「串口打开成功」= Connected，**不校验任何握手**——所以「Stellarium 显示 Connected 但固件没反应」是可能的（日志里会因超时重置反复出现）。

---

## 4. 位置查询应答格式（长格式，P0）

`Lx200Command.cpp` 中 `Lx200CommandGetRa` / `Lx200CommandGetDec` 的解析器：

- 应答首字节若是 `'#'` 会被丢弃（第 460-461、563-564 行）→ 固件**可选**先回一个 `#`，但没必要；正常从数据开始。
- **RA 长格式（必须恰好 9 字节）**（第 463-535 行）：
  ```
  "HH:MM:SS#"
  ```
  - 2 位小时 + ':' + 2 位分 + ':' + 2 位秒 + '#'；全零填充，无符号，无小数；小时 00-23。
  - 解析器在 `':'` 分支要求剩余 ≥9 字节（第 490-491 行），否则继续等 → 必须一次发完整。
- **Dec 长格式（必须恰好 10 字节）**（第 566-653 行）：
  ```
  "+DD\xDFMM:SS#"      （0xDF = 223 = ISO-8859-1 的度数符号 "°"）
  ```
  - 符号 `+`/`-` 必须显式；2 位度 + 0xDF + 2 位分 + ':' + 2 位秒 + '#'；全零填充。
  - 若把 0xDF 写成 `*`（ASCII 42）或普通 `°`（U+00B0 的 UTF-8 两字节），解析器只在 DEBUG4 里记日志但**不中断**（第 594-600 行），随后仍然按固定偏移解析 → 可能把时间字段读错。**务必发单字节 0xDF。**
- **短格式**：RA `"HH:MM.S#"`（第 494-498 行），Dec `"+DD\xDFMM#"`（第 606-609 行）。首次收到短格式时 Stellarium 会发一次 `#:U#` 切换格式（`TCDL.cpp:202-210`）。
  → **固件应始终使用长格式**，这样永远不会触发 `:U#`。
- 换算：RA 应答 → 秒数 → `ra_int = floor(sec * 2^32/86400)`（第 533 行）；Dec 应答 → 角秒 → `dec_int = floor(arcsec * 2^32/(360*3600))`（第 651 行）。即 Stellarium 端精度为「赤经 1 秒 / 赤纬 1 角秒」，再多的小数位没有意义。

---

## 5. 命令全表（按 P0/P1/不使用 分类）

> 表中「实际字节」是 Stellarium 真正写到线上的内容（注意**部分命令带前导 `'#'`**，这是历史遗留，固件解析器必须容忍）。

### 5.1 P0（必须实现，否则连不上/不能验收）

| # | Stellarium 实际发送 | 时机 | 期望应答 | 源码 |
|---|---|---|---|---|
| P0-1 | `#:GR#` | 连接后立即，之后每 500 ms 一轮 | `HH:MM:SS#`（9B） | `Lx200Command.cpp:442-535` |
| P0-2 | `#:GD#` | 紧跟 GR 应答之后（同一轮） | `+DD\xDFMM:SS#`（10B） | `Lx200Command.cpp:545-653` |
| P0-3 | `:SrHH:MM:SS#` | goto/sync 时，紧接 `#:Q#` | 单字符 `1`（`0`=无效；只需 1 字节） | `Lx200Command.cpp:76-141` |
| P0-4 | `:Sd+DD\xDFMM:SS#` | 紧跟 `:Sr` 应答之后 | 单字符 `1` | `Lx200Command.cpp:150-226` |
| P0-5 | `:MS#` | `:Sd` 应答后 | 立即回 `0`（开始 slew）；失败回 `1` 或 `2` + 消息 + `#` | `Lx200Command.cpp:237-332` |
| P0-6 | `#:Q#` | 每次 goto/sync 序列的第一条 | **不应答**（当前实现 needsNoAnswer） | `Lx200Command.cpp:59-69` |

补充说明：
- P0-3/P0-4 的 `:Sr`/`:Sd` 应答只要首字节是 `'0'`/`'1'` 就会被消费；回 `"1"` 或 `"1#"` 都可以（多余的 `#` 会被下一条命令应答解析器忽略，见第 102-103/187-188/254-255 行）。**推荐回单字节 `1`**。
- P0-5 的 `:MS#` 回 `0` 时解析器**只消费 1 字节**（第 269-277 行），所以回 `"0"` 或 `"0#"` 都行（推荐 `0`）。
- P0-6 `:Q#` 是 `needsNoAnswer=true`，固件可以不回任何字节；但实现成「收到就停电机」即可（我们的验收工具会单独测 `:Q#`）。
- 固件不需要「真正的」ASCOM 语义；把 `:MS#` 做成「立即应答 `0` + 异步开始转动」最稳（3 秒超时很紧，等转完再回会超时）。

### 5.2 P1（兼容/诊断，建议实现）

| # | 命令 | 说明 | 源码 |
|---|---|---|---|
| P1-1 | `#:U#` | 切换长/短坐标格式。固件若只支持长格式，可以收到后**不回任何字节**（Stellarium 只在首次发现短格式时发一次；正常不会出现） | `Lx200Command.cpp:41-51` |
| P1-2 | `:CM#` | sync（把当前指向同步到 `:Sr`/`:Sd` 设的目标），回 `0`（失败 `1`/`2`+`#`） | `Lx200Command.cpp:339-434` |
| P1-3 | `:GVP#` | 产品名（**Stellarium 23.4 直连不会发**；我们的验收工具会发，便于识别固件）。建议回 `Fysetc AltAz#` 之类 | 无（LX200 通用） |
| P1-4 | 自定义诊断 | 建议 `:XG#` 回一行状态（如 `IDLE,AZ=12.34,ALT=45.67,TRK=1#`），不参与 Stellarium 流程 | 自定 |
| P1-5 | 自定义设置时钟/站点 | `:XW<unix秒>#` 设 UTC；`:XC<纬度>,<经度>#` 设站点（度，东经/北纬为正）。**Stellarium 不会发这些**，仅供我们的工具/firmware-dev 自检使用 | 自定 |

### 5.3 Stellarium 23.4 **不会**发送的命令（别为它们花时间）

`:GA#`（高度）、`:GZ#`（方位）、`:D#`（距离条/回焦）、`:GVP#`/`:GVN#`、`:St# :Sg# :SL# :SG# :SC# :Sz#`（时间/地点/时区/日期）、`:Sr` 之外任何 `:S...` 设置、`:RG#`/`:RC#`/`:RM#`/`:RS#`（微动速率）、`:MF#`/`:MP#`（焦点）、`:MS` 之外的 `:M...`。

依据：`Lx200/Lx200Command.hpp` 只声明了 8 个命令类（ToggleFormat/StopSlew/SetSelectedRa/SetSelectedDec/GotoSelected/SyncSelected/GetRa/GetDec）；整个 `plugins/TelescopeControl/` 源码中不存在上述其它命令的字节构造。

> 注意：`TelescopeClientDirectLx200` **没有** `:GA#/:GZ#` 也没有时间同步。网上的「LX200 协议大全」与本插件的实现子集差别很大，以本文为准。

---

## 6. 时间/地点同步（(b) 答案：Stellarium 不会发）

- `Lx200Command.hpp` 命令类清单 + 全插件 grep 均无 `:St/:Sg/:SL/:SG/:SC/:Sz`。
- Stellarium 的位置/时间只用于自己把望远镜报的 RA/Dec 画到星图上；它**默认望远镜自己知道时间地点**（真实 LX200 手控器里有）。
- 后果：**固件的 alt/az ↔ RA/Dec 换算所需的时间和站点必须由固件自己持有**。可选来源：
  1. 自定义命令（推荐，实现成本最低）：`:XW<unix>#` + `:XC<lat>,<lon>#`，由 `tools/stellarium_sim.py` 的 `setclock`/`connect` 或 Lead 手工下发；
  2. 编译期默认值（`__DATE__`/`__TIME__` + 固定站点）作为兜底；
  3. WiFi + SNTP（最准，但增加复杂度/启动时间）。
- 只要固件的 UTC、经纬度与 Stellarium 当前设置（`.ssc` 中 `core.setDate`/`core.setObserverLocation`）一致，星图上的望远镜标记就会指向正确天区。

---

## 7. 坐标映射（(d) 答案：必须报赤道坐标）

- `TCDL.cpp:167-171`：Stellarium 取望远镜位置 → `InterpolatedPosition` → 直接当作**赤道坐标**使用。
- `TCDL.cpp:259-273`：收到的 RA/Dec 按 `equinox` 转换成 **J2000** 存入（`equinoxEquToJ2000`，关闭折射）；发送 goto 时若 `equinox==JNow` 则用 `j2000ToEquinoxEqu` 反算（`TCDL.cpp:113-118,137-142`）。
- 唯一可选配置是 `equinox`：`"J2000"`（默认）或 `"JNow"`（`TC.cpp:824-830`；GUI 单选「Coordinate system: J2000 (default) / Equinox of the date (JNow)」）。**没有地平坐标/赤道仪类型选项**。

**取舍建议：望远镜 JSON 用 `"equinox": "JNow"`。**
理由：Stellarium 会把目标 RA/Dec 换算到「当日平赤道（equinox of date）」再下发，把回报值也按当日赤道解释；固件侧只需：
```
LST(视恒星时) = f(UTC, 经度)            // 固件自算
HA = LST − RA
sin(alt) = sin(lat)sin(dec) + cos(lat)cos(dec)cos(HA)
az = atan2(−sin(HA), tan(dec)cos(lat) − sin(lat)cos(HA))   // 北起顺时针
```
不需要岁差/章动矩阵（JNow 已含到当日赤道），误差对演示级别可忽略（几角秒~几十角秒，主要取决于恒星时算法与时间精度）。若用 J2000，则固件还要做 J2000→当日赤道的岁差换算，工作量更大、更容易出错。

> 物理上 alt-az 机架：AZ 轴连续可转（无限位）、ALT 轴建议软件限位 0°~90°。固件收到 RA/Dec 后换算成 alt/az 目标再驱动电机；Stellarium 只关心 `:GR#/:GD#` 回报的赤道坐标。

---

## 8. goto / sync / abort 语义对照

| 用户动作 | Stellarium 行为 | 固件需要 |
|---|---|---|
| 选中天体 + `Ctrl+1..9` | `slewTelescopeToSelectedObject(N)` → goto 序列（Q/Sr/Sd/MS） | 立即回 `:Sr`→`1`、`:Sd`→`1`、`:MS`→`0`，然后开始转动 |
| `Ctrl+0` 坐标窗 → Slew | 同上（RA/Dec 由输入框给出） | 同上 |
| `Alt+1..9` | 转到屏幕中心方向 | 同上 |
| `Ctrl+Shift+1..9` | sync：Q/Sr/Sd/`#:CM#` | `:CM#` 回 `0`；把当前位置设为选中 RA/Dec（轴校正） |
| 列表里 Start/Stop（直接连接） | 打开/关闭串口，停止时清队列（不发 `:Q#`） | 串口被关闭即停止；固件最好在无命令超时后自行刹车（可选保护） |
| `Ctrl+Shift+Alt+1..9` / Slew 窗口 Abort 按钮 | **对 LX200 直连无效**：`TelescopeClientDirectLx200` 未 override `telescopeAbortSlew()`，基类只打 warning（`TelescopeClient.hpp:88`） | 固件仍需实现 `:Q#` 停止（`tools/stellarium_sim.py abort` 会用它验收；另外每次新 goto 前也会先收到 `:Q#`） |

> 想让望远镜中途停下：在 Stellarium 里再触发一次 goto/sync（会先发 `:Q#`），或用我们的工具发 `:Q#`。

---

## 9. LX200 over TCP？(e) 答案：不支持（但另有二进制 TCP 备选）

- `Lx200Connection : public SerialPort`（`Lx200/Lx200Connection.hpp`），构造参数就是串口设备名（`TCDL.cpp:88`）→ **LX200 直连只能是串口**。
- GUI 里另一个选项「External software or a remote computer」（`connection: "local"/"remote"`）走的是 **Stellarium 私有二进制 telescope 协议**（`TelescopeClient.cpp:170-543` 的 `TelescopeTCP`），不是 LX200：
  - 包结构：`uint16 size`(LE) + `uint16 type`(LE) + payload；
  - 服务端→客户端位置包：`type=0`、`size=24` = server_micros(8) + ra_int(4) + dec_int(4) + status(4)；
  - 客户端→服务端 goto 包：`type=0`、`size=20` = client_micros(8) + ra_int(4) + dec_int(4)；
  - `ra_int/dec_int` 与 LX200 相同：`floor(angle * 2^32/(2π))`；坐标同样是赤道坐标+equinox 转换。
  - 默认端口 `10000 + slot`（slot 1 → **10001**），见 `TelescopeControl.hpp:233-235`。
- 23.4 已**不再随附**可执行 telescope server（`device_models.json` 无 `use_executable` 字段；`E:\Stellarium` 下无 `servers/`、无 `TelescopeServer*.exe`；仓库中也无 `telescope_server` 源码）。因此「本地/远程」只能连外部自写程序。
- 结论：**方案仍按 LX200 over 串口**；如果将来串口路线受阻，可让固件再开一个 TCP 端口实现上面 20/24 字节二进制协议（工作量小于 LX200，但 Stellarium 侧必须选「External software」而不是 LX200）。

---

## 10. 固件 P0/P1 实现清单（给 firmware-dev 直接对照）

### P0（缺一不可）
1. **串口 9600 8N1、无流控**；不主动输出任何字节（无 banner/日志/换行）；忽略 `\r`/`\n`/前导 `#`/杂字节。
2. 解析 `#:GR#` → `HH:MM:SS#`（9 字节，零填充，无小数）。
3. 解析 `#:GD#` → `+DD\xDFMM:SS#`（10 字节，单字节 0xDF，符号必有）。
4. 解析 `:SrHH:MM:SS#` → 存目标 RA，回 `1`。
5. 解析 `:Sd+DD\xDFMM:SS#` → 存目标 Dec，回 `1`。
6. 解析 `:MS#` → **立即**回 `0`，然后异步执行 goto（alt/az 换算 + 电机）；失败回 `1`+消息+`#`。
7. 解析 `#:Q#` → 停止运动，不回包。
8. 全部应答 <100 ms（硬上限：普通 5 s、`:MS#` 3 s），否则 Stellarium 会 reset + 静默 10 s。
9. 位置回报必须由固件自己维护的 alt/az + UTC + 站点换算成 RA/Dec（见 §6、§7）。
10. 启动/复位后快速就绪（建议 <1 s 内能应答；boot ROM 残字节容忍）。

### P1（加分/便于验收）
11. `:CM#` → 回 `0`（sync：用当前 alt/az 反推 RA/Dec 并记为当前指向）。
12. `#:U#` → 忽略即可（保持长格式）。
13. `:GVP#` → 回产品名 `Fysetc AltAz#`（我们的工具用它确认固件在线；Stellarium 不会发）。
14. `:XG#` → 一行状态（供无星图时验收）；`:XW<unix>#`、`:XC<lat>,<lon>#` → 设置时间/站点（自定义，Stellarium 不发）。
15. 无命令超时（例如 6 s 没收到任何字节）→ 自动减速停车（防止串口断开后电机狂转）。

### 明确不需要
`:GA#`、`:GZ#`、`:D#`、`:GVP#`（Stellarium 不调用）、任何时间地点 `:S...`、微动速率、焦点、回焦、温补。

---

## 11. 验收注意（给 Lead / 工具）

- 连接后 500 ms 内应看到 `GR/GD` 轮询；`log.txt` 里若反复出现 `Lx200Connection::dataReceived: error: command_list is empty` 或 `communicationResetReceived`，说明固件主动发了字节或应答超时。
- Stellarium 侧「Connected」不代表固件在应答；以工具 `status`/`goto` 为准。
- Stellarium 打开 COM7 期间独占；验收工具与星图不能同时占用。
- 首次连接可能因 ESP32 被 DTR 复位而多花 ~10-15 s（一次 reset 惩罚），属正常。
- 参考工具：`tools/stellarium_sim.py`（按本规格逐字节复现），另有 `tools/mock_lx200_mount.py` 可在无硬件时自测工具本身。
