# FYSETC E4 Alt-Az 固件（LX200 + TMC2209 + FastAccelStepper）

FYSETC E4（ESP32-WROOM-32，板载 4×TMC2209）上的赤道仪式望远镜控制替代固件：
用 **LX200 兼容串口协议** 接收 Stellarium 23.4 的指令，把 RA/Dec 目标换算成
AZ/ALT，用 FastAccelStepper 产生两轴 STEP/DIR 脉冲驱动空载电机。

- X 电机 → **AZ 方位轴**（可连续旋转，0..360° wrap）
- Y 电机 → **ALT 高度轴**（软件限位 0..90°）
- Z / E0 未使用

> 注意：本固件 **不做任何上传/烧写**。烧录由 Lead 用 COM7 执行；串口独占。

---

## 1. 编译与烧录

```powershell
# 在 firmware/ 目录下
C:\Users\Administrator\.platformio\penv\Scripts\platformio.exe run          # 编译
C:\Users\Administrator\.platformio\penv\Scripts\platformio.exe run -t upload # 烧录（Lead 执行）
```

产物：`.pio/build/fysetc_e4/firmware.bin`。

构建平台：`espressif32@6.9.0`（Arduino core 2.0.17）。
2026-11-18 实测编译结果（v1.1.0，加入 NVS 持久化/会话日志后）：

```
RAM:   [=         ]   8.5% (used 28012 bytes from 327680 bytes)
Flash: [===       ]  26.4% (used 346297 bytes from 1310720 bytes)
Building .pio/build/fysetc_e4/firmware.bin
========================= [SUCCESS] Took 17.53 seconds =========================
```

v1.0.0（首版）也曾一次编译通过并在真机烧录验证：RAM 8.2% / Flash 26.1%，
`[SUCCESS] Took 39.80 seconds`，烧录 342896 bytes。

`platformio.ini` 关键项：
- `platform = espressif32@6.9.0`、`board = esp32dev`、`framework = arduino`
- `upload_port = COM7`、`upload_speed = 460800`（ROM bootloader 烧录波特率）
- `monitor_speed = 9600`（运行时 LX200 串口实际波特率）

**运行时串口是 9600 8N1**，不是 115200。原因：Stellarium 23.4 的直连 LX200
串口客户端在 `plugins/TelescopeControl/src/common/SerialPort.cpp` 里硬编码
`BuildCommDCBW("9600,n,8,1")`，不可配置。烧录走 ESP32 ROM bootloader，与固件
运行波特率无关。

---

## 2. 硬件引脚（来自 Marlin `pins_FYSETC_E4.h` / `pins_ESPA_common.h`）

| 功能 | 引脚 |
|---|---|
| AZ STEP (X) | GPIO 27 |
| AZ DIR (X)  | GPIO 26 |
| ALT STEP (Y)| GPIO 33 |
| ALT DIR (Y) | GPIO 32 |
| 四轴共用 EN（低有效） | GPIO 25 |
| TMC2209 UART RX/TX | GPIO 21 / 22（Serial1，115200，四轴共享单线总线） |

TMC2209 从机地址（FYSETC 官方 `FYSETC/FYSETC-E4` 仓库
`examples/Ender-3/Configuration_adv.h` 核实）：**X=1，Y=3，Z=0，E0=2**；
`R_SENSE = 0.11 Ω`（Marlin TMC2209 默认值）。本固件使用
`TMC2209Stepper(&Serial1, 0.11f, 1)`（AZ）与 `(…, 3)`（ALT），
`rms_current = 650 mA`、`microsteps = 16`、stealthChop（`en_spreadCycle(false)` +
`pwm_autoscale(true)`）。

**UART 降级**：如果某个驱动在 UART 上无应答（`test_connection()` 非 0），
固件仍按纯 STEP/DIR 继续工作（电机照样转），并在 `:XG#` 报告 `TMC=FAIL`。
注意此时微步/电流是驱动的上电默认状态（不是 650 mA / 1/16），
`AZ/ALT_STEPS_PER_DEG` 的度↔步换算可能不准，但动作方向与功能不受影响。
实测本板 UART 双向正常（`:XG#` → `TMC=OK`）。

---

## 3. 运行期 UART 行为（重要）

UART0（CH340 → COM7）专供 LX200，**运行期不允许任何调试输出**，否则
Stellarium 会把杂字节当成 `:GR#` 应答而解析失败（然后静默 10 秒）。

- 上电 banner 默认**关闭**（`config.h` 中 `LX200_BOOT_BANNER 0`）。
  需要台架调试时用 `-DLX200_BOOT_BANNER=1` 编译打开（只在 setup 打印一次）。
- 所有诊断走自定义命令 `:XG#`。

---

## 4. LX200 协议实现

### 4.1 Stellarium 23.4 直连 LX200 的实际行为（已对 v23.4 源码核实）

- 打开串口后**没有握手**，不查询 `:GVP#`/`:GVN#`，立即每 500 ms 轮询：
  `#:GR#` → 等待应答 → `#:GD#`。命令带一个**前导 `#`**，解析器会忽略 `:` 之前的字节。
- 应答必须是精确的**长格式**（Stellarium 按字节数解析，短了会一直等）：
  - `:GR#` → `"HH:MM:SS#"`，9 字节
  - `:GD#` → `"+DD\xDFMM:SS#"`，10 字节（`0xDF` 是度符号；`*` 也容忍）
- goto：`#:Q#` → `:SrHH:MM:SS#`（回 `1`）→ `:Sd+DD\xDFMM:SS#`（回 `1`）→ `:MS#`（回 `0`）
- sync：`#:Q#` → `:Sr…#` → `:Sd…#` → `:CM#`（回 `0`）
- Stellarium **完全不发** `:St`/`:Sg`/`:SL`/`:SG`/`:SC`，所以站点与时钟必须由固件自带
  （或由 Lead 的脚本用下面的 `:XW`/`:XC` 注入）。
- 位置查询是**赤道坐标**（Stellarium 没有地平坐标模式），固件用内部 UTC+站点
  由实际 alt/az 反算 RA/Dec 上报。推荐望远镜 JSON 用 `"equinox": "JNow"`
  （`docs/stellarium-config.md` 的样例即 JNow），此时固件不做事先岁差换算
  （`CLIENT_EQUINOX_J2000 0`）；若客户端设成 J2000，把该宏改成 1 即可由固件做
  IAU 1976 岁差转换（差异 <0.4°）。
- **应答字节数必须精确**：`:Sr`/`:Sd` 的应答是**单字节** `1`（成功）/ `0`（无效），
  `:MS#`/`:CM#` 成功也是**单字节** `0`（Stellarium 的解析器只消费 1 字节）。
  自定义 `:GVP#`/`:XG#`/`:XW`/`:XC` 则是 `#` 结尾（验收工具按 `#` 读）。
- 收到 `:` 之前的杂散字节（Stellarium 的前导 `#`、`\r`、`\n`、boot ROM 残字节）一律丢弃。

### 4.2 命令表

| 命令 | 说明 | 应答 |
|---|---|---|
| `:GVP#` | 产品名 | `FYSETC E4 AltAz#` |
| `:GVN#` | 固件版本 | `1.0.0#` |
| `:GR#` | 当前 RA（长格式） | `HH:MM:SS#` |
| `:GD#` | 当前 Dec（长格式，0xDF 度符号） | `+DD\xDFMM:SS#` |
| `:GA#` / `:GZ#` | 当前 ALT / AZ | `sDD\xDFMM:SS#` / `DDD\xDFMM:SS#` |
| `:GG#` `:GL#` `:GC#` `:Gt#` `:Gg#` `:GS#` | UTC 偏移 / 本地时间 / 本地日期 / 纬度 / 经度 / 恒星时 | 标准格式 |
| `:SrHH:MM:SS#` | 设目标 RA | `1`（单字节）/ `0` |
| `:Sd+DD\xDFMM:SS#` | 设目标 Dec | `1`（单字节）/ `0` |
| `:MS#` | goto 已选目标 | `0`（单字节，立即；随后异步转动） / `1#`（失败） |
| `:CM#` | sync（把当前位置定义为已选目标） | `0`（单字节） / `1#` |
| `:Q#` | 停止运动 | 无应答 |
| `:U#` | 切换短/长格式（恒为长格式） | 无应答 |
| `:D#` | 距离条 | `#` |
| `:St±DD*MM#` `:Sg±DDD*MM#` | 设站点纬度 / 经度（东经为正） | `1#` |
| `:SLHH:MM:SS#` `:SCMM/DD/YY#` `:SG±HH[:MM]#` | 设本地时间 / 日期 / UTC 偏移 | `1#` |
| `:XW<unixSeconds>#` | **自定义**：设 UTC 时钟（写入 NVS，掉电/复位保持） | `1#` / `0#` |
| `:XC<lat>,<lon>#` | **自定义**：设站点十进制度（北纬/东经为正，写 NVS） | `1#` / `0#` |
| `:XG#` | **自定义**：单行状态诊断，`#` 结尾，<200 字节 | 见下 |
| `:XJ#` | **自定义**：返回上一会话收到的最近 24 条非常规命令（`ms:cmd|ms:cmd|...`） | `...` + `#`；无记录回 `NONE#` |
| `:XR#` | **自定义**：清除 NVS 中持久化的指向状态并把两轴位置归零 | `1#` |

未知命令**不回任何字节**（避免破坏 Stellarium 的应答匹配）。在 `:` 之前的杂散
字节（Stellarium 的前导 `#`）被忽略。

### 4.3 `:XG#` 诊断行示例

```
S=IDLE AZ=123.45 ALT=45.67 AZS=12345/12400 ALTS=4000/4000 TRK=1 RA=12.3456 DEC=+45.67 TGT=12.3456/+45.67 TMC=OK UTC=1763456789 LAT=39.90 LON=+116.41 BOOT=7 LASTMS=128734#
```

字段：状态机（`S=IDLE/SLEW`）、当前 AZ/ALT（度）、AZ/ALT 当前步数/目标步数
（为控制行长，显示值截断到 ±999999）、是否在跟踪（TRK）、当前上报的 RA/Dec、
目标 RA/Dec、TMC UART 状态（`OK`/`AZ`/`ALT`/`FAIL`）、当前 UTC unix 秒、
纬度、经度、启动次数（BOOT，每次 DTR 复位/上电 +1）、上一次接受 `:MS#` 时的
`millis()`（LASTMS，0=本次上电后还没 goto；从 NVS 恢复时显示上一会话的值）。
整行（含 `#`）<200 字节。

### 4.4 命令缓冲解析（实测踩过的坑）

`:SrHH:MM:SS#` 与 `:Sd+DD\xDFMM:SS#` 的参数**本身含 `':'`**，所以解析器不能
"见到 `:` 就重开一条命令"。当前实现（`lx200Poll()`）：

- 只有 `s_len == 0`（当前没有在拼命令）时，`':'` 才作为新命令的起始；
- 命令拼接中（`s_len > 0`）遇到的 `':'` 一律当参数数据；
- `'#'` 结束并派发命令；`s_len == 0` 时的杂散 `'#'` / `\r` / `\n` / 其它字节忽略；
- 缓冲溢出（96 B）丢弃该条命令，下一条命令不受影响。

这个 bug 在真机上表现为：`:Sr`/`:Sd` 无应答、`:MS#` 回 `0` 但目标未设置；
修正后 `:Sr`→`1`、`:Sd`→`1`、`:MS#`→`0` 与 goto 均正常。

### 4.5 NVS 持久化 + 会话命令日志（`src/persist.cpp`）

**为什么需要**：任何程序打开 COM7 时，CH340 的 DTR/RTS 会复位 ESP32，RAM 里的
指向/目标/跟踪状态会归零，也没有证据说明 Stellarium 到底发过什么。固件用 NVS
（namespace `altazP`）保存：
- **指向状态**：AZ/ALT 步数、目标 RA/Dec、`:Sr`/`:Sd` 选择值、sync 偏移量、
  TRK 状态、UTC、BOOT 次数、LASTMS。每 2 s 写一次，并在收到 `:MS#` 时立即写；
  上电/复位后若校验通过则恢复（位置/指向/跟踪/时钟），因此重新打开 COM7 后
  `:GR#`/`:GD#`/`:XG#` 仍是复位前的位置。
  ⚠ 这是**最后指令位置（开环）**，不是编码器真值。
- **会话命令日志**：24 条环形缓冲，每条 `millis():命令原文`（截断 32 字符）。
  过滤掉高频轮询 `:GR`/`:GD`/`:GA`/`:GZ`、识别查询 `:GVP`/`:GVN` 以及诊断命令 `:XG`/`:XJ`（否则几秒就被轮询刷满），
  保留 `:Q#`/`:Sr`/`:Sd`/`:MS#`/`:CM#` 等关键命令；在 `:Sr`/`:Sd`/`:CM#`/`:Q#`/
  `:MS#` 时写入 NVS，boot 时也写入一次并保留上一会话的日志。
  用 `:XJ#` 读回（`ms:cmd|ms:cmd|...`，无 `#` 出现在内容里，整行 <800 字节）。
- `:XR#` 清除持久化指向并把两轴归零（`boot` 计数和会话日志保留）。

写入频率：状态每 2 s 一次 + 关键命令事件；NVS 有磨损均衡，demo/验证场景足够。
`firmware/.pio`、`*.log` 已在仓库 `.gitignore` 中。

---

## 5. 默认参数与可调宏（`src/config.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `LX200_BAUD` | 9600 | LX200 串口波特率（Stellarium 硬编码 9600） |
| `AXIS_MAX_SPEED_HZ` | 2000 | 最大步频 steps/s |
| `AXIS_ACCEL_HZ2` | 800 | 加速度 steps/s² |
| `MOTOR_FULL_STEPS` | 200 | 电机整步/圈 |
| `TMC_MICROSTEPS` | 16 | 微步 |
| `AZ_GEAR_RATIO` / `ALT_GEAR_RATIO` | 5.0 | 虚拟减速比（机械未知，仅用于度↔步换算） |
| `AZ_STEPS_PER_DEG` | 44.44 | = 200×16×5/360 |
| `ALT_STEPS_PER_DEG` | 44.44 | 同上 |
| `ALT_MIN_DEG` / `ALT_MAX_DEG` | 0 / 90 | 高度软件限位（可改 0..180） |
| `TRACK_INTERVAL_MS` | 200 | 跟踪重算周期 |
| `SITE_LAT_DEG_DEFAULT` | 39.9042 | 默认北京纬度 |
| `SITE_LON_DEG_DEFAULT` | +116.4074 | 默认北京经度（东正） |
| `DEFAULT_UTC_OFFSET_SEC` | +8h | 默认 UTC 偏移（`__DATE__`/`__TIME__` 按本地时间解析） |
| `CLIENT_EQUINOX_J2000` | 0 | 客户端坐标系：0 = JNow（推荐，见 telescopes.json 样例），1 = J2000 |
| `LX200_IDLE_STOP_MS` | 6000 | 串口 6 s 无任何收字节 → 自动停止电机/跟踪 |
| `LX200_BOOT_BANNER` | 0 | 台架调试时设 1 打开一次性 banner |

时间默认值是**编译时刻**（按 +08:00 解释后换算 UTC），因此 Lead 的脚本应在
启动 Stellarium 前用 `:XW<unix>` / `:XC<lat>,<lon>` 注入精确值；两者写入 NVS，
即使打开串口触发 ESP32 复位（DTR/RTS）也不会丢失（时间会停留在最后一次设置时刻）。

`AZ_STEPS_PER_DEG` / `ALT_STEPS_PER_DEG` 只影响"1 度 = 多少步"；机械未接负载时
不影响功能，接机械后按实际减速比修改 `*_GEAR_RATIO` 即可。

---

## 6. 运动与跟踪逻辑

1. `:Sr` + `:Sd` 记录目标（赤道坐标，equinox 由 `CLIENT_EQUINOX_J2000` 决定；默认 JNow）。
2. `:MS#`：先**立即回单字节 `0`**，然后把目标换算成 of-date 赤道 → `astroRaDecToAltAz()`
   → 目标 AZ（最短路径连续旋转）与目标 ALT（夹紧到 `ALT_MIN_DEG..ALT_MAX_DEG`，
   **夹紧而不是报错**，保证空载验收时电机一定动作）→ 两轴 `moveTo`（异步、非阻塞）。
3. 两轴停止（且 ≥300 ms 后）进入跟踪：每 200 ms 用内部时钟重新算目标的
   alt/az，向新位置小步移动（开环恒星时跟踪）。
4. `:Q#`：`forceStop()` 两轴并退出跟踪。
5. 串口 6 s 收不到任何字节（客户端关闭/崩溃）→ 自动 `scopeStop()`，防止电机一直转。
6. `:GR#`/`:GD#`：由**实际步数位置** → alt/az → 赤道（of-date 或 J2000）上报，
   保证"报什么位置就是电机实际在哪"。
7. 上电/复位时从 NVS 恢复最后指向（步数/目标/TRK/UTC），不会归零；`:XR#` 才归零。
   若恢复时 TRK=1，会继续跟踪（无客户端命令 6 s 后由空闲保护停车）。

> 协议细节以 `docs/stellarium-lx200-spec.md`（第 10 节 P0/P1 清单）为准。

---

## 7. 真机验收记录（2026-11-18，Lead 在 FYSETC E4 / COM7 实测）

### 7.1 v1.0.0（首版，已烧录实测通过；Flash 26.1% / RAM 8.2%）

| 项目 | 实测结果 |
|---|---|
| `:GVP#` | `FYSETC E4 AltAz#`（工具可识别） |
| `#:GR#` | 9 字节 `HH:MM:SS#`，Stellarium 轮询正常 |
| `#:GD#` | 10 字节 `+DD\xDFMM:SS#`（单字节 0xDF） |
| `:XW` / `:XC` | 生效；写入 NVS，复位/重开串口后仍保持 |
| `#:Q#` / `:Sr` / `:Sd` / `:MS#` | `:Q#` 停转（无应答）；`:Sr`→`1`；`:Sd`→`1`；`:MS#`→`0` |
| M31 goto | AZ 20.8°→78.46°、ALT 0°→77.29°，与理论值（78.5°/77.3°）一致，约 4.5 s 完成 |
| 跟踪 | goto 完成后 `TRK=1`，两轴持续微动；`:XG#` 回报 RA=0.7124h / Dec=+41.27° 与目标 0.7122h/+41.27° 吻合 |
| TMC2209 | `:XG#` 显示 `TMC=OK`（UART 通，stealthChop 650 mA） |

### 7.2 v1.1.0（NVS 持久化 + 会话日志，编译通过，待 Lead 烧录复验）

新增验证点：
1. Stellarium 会话结束后（关掉 Stellarium），用 `tools/set_scope.py --status` 或串口
   工具重开 COM7（会触发一次 DTR 复位）→ `:XG#` 应仍是复位前的指向（步数/目标/
   `BOOT` 递增/`LASTMS` 非 0），而不是 0/0。
2. `:XJ#` 应能看到该会话的命令证据，例如
   `...:Q|...:Sr00:42:44|...:Sd+41\xDF16:00|...:MS|...`（`ms:cmd` 形式，`|` 分隔）。
3. `:XR#` → `1#`；随后 `:XG#` 的步数/目标归零、`:XJ#` 历史仍保留。

验收步骤（复现）：

1. 烧录后拔插/复位，确认串口 9600 打开时无杂散字节。
2. 启动 Stellarium **之前**用 `tools/set_scope.py`（或串口工具）以 9600 8N1 发送：
   `:XW<当前unix秒>#` → 期望 `1#`；`:XC<lat>,<lon>#` → 期望 `1#`。
3. 发 `:XG#`，检查 `TMC=OK`、`UTC`/`LAT`/`LON` 正确。
4. 启动 Stellarium（LX200 直连 COM7 9600、equinox JNow），连接望远镜：轮询正常。
5. 选目标 → goto：`:XG#` 中 `S=SLEW`，结束后 `TRK=1`；`:Q#` 可立即停转。
6. 关闭 Stellarium 后重开 COM7 读 `:XG#` / `:XJ#` 验证 7.2 的状态恢复与命令日志。

**首次连接时序提醒**：如果打开 COM7 会触发 ESP32 自动复位（CH340 DTR/RTS），
Stellarium 的第一条 `:GR#` 可能落在启动期间而超时，它会静默 10 s 后重试，
因此连接建立最多可能需要 ~16 s，属正常现象。

---

## 8. 已知限制

- **时间/站点必须外部注入**：Stellarium 23.4 直连 LX200 完全不发 `:St/:Sg/:SL/:SG/:SC`。
  不注入时固件用"编译时刻 + 默认北京站点"兜底；时间每差 1 分钟，指向约差 0.25°，
  因此验收/使用前必须用 `tools/set_scope.py` 下发 `:XW`/`:XC`（写入 NVS，复位不丢）。
- **ALT 软件限位 0°..90°**（`ALT_MIN_DEG`/`ALT_MAX_DEG` 可改 0..180）；目标越界时
  **夹紧到限位**而不是回 LX200 错误码（空载验收保证电机必动，接真实机械后可改成报错）。
- **AZ 无硬限位**，可连续旋转（0..360° wrap，goto 走最短路径）。
- **开环、无编码器/无机械限位**：上电位置定义为 AZ=0°、ALT=0°；位置精度取决于
  步进分辨率（当前 44.44 steps/deg ≈ 0.0225°/步）和是否丢步，长时间运行会累积误差。
- **跟踪为开环恒星时近似**：每 200 ms 按目标 RA/Dec 重算 alt/az 小步移动；未含大气
  折射改正、未含章动（~17"），对演示/空载验证足够。
- **equinox**：默认按 JNow（`CLIENT_EQUINOX_J2000 0`，配套 `telescopes.json` 的
  `"equinox": "JNow"`）；若客户端用 J2000 则把宏改 1（固件做 IAU 1976 岁差）。
- **机械速比未知**：`AZ/ALT_GEAR_RATIO` 是占位值（200×16×5/360 = 44.44 steps/deg），
  接真实机械后必须按实际传动比修改。
- **持久化的是"最后指令位置"**（NVS 恢复，开环），不是编码器/限位真值：上电恢复后
  指向与复位前一致，但如果复位前有人手动转动过电机（不开步），恢复值不会反映。
  `:XR#` 清除持久化并把两轴归零（BOOT 计数与会话日志保留）。
- **NVS 写入频率**：状态每 2 s + 关键命令事件（`:Sr/:Sd/:Q/:MS/:CM` 时写日志）；
  NVS 有磨损均衡，验证/demo 场景没问题，长期运行可把 2 s 改成更长。
- **会话日志只记非常规命令**：为免几秒内被 `#:GR#`/`#:GD#` 轮询刷满，日志过滤掉
  `:GR/:GD/:GA/:GZ`、`:GVP/:GVN` 和 `:XG/:XJ`，保留 `:Q/:Sr/:Sd/:MS/:CM/:XW/:XC/:XR`
  等（含参数原文）。
- 串口 6 s 无任何收字节会自动停车；`Serial1`（TMC UART）与 LX200 串口相互独立。
