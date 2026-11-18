# FYSETC E4 Alt-Az Mount for Stellarium

用一块 **FYSETC E4**（ESP32 + 4×TMC2209，USB 串口 `COM7`）做一个可以被 **Stellarium 23.4** 直接控制的 **地平式（Alt-Az）赤道仪**：
X 电机 = 方位轴（AZ），Y 电机 = 高度轴（ALT）。电机空转也能验证：Stellarium 里选中天体按 `Ctrl+1`，两块电机就会转起来，星图上的望远镜标记会按固件回报的赤经/赤纬移动。

> 本项目将板子原来的 Marlin（3D 打印机固件）替换为我们自己的 LX200 固件；原固件已整片备份（见「恢复原厂固件」）。
> 不需要 ASCOM、不需要 INDI、不需要额外的中转程序：**Stellarium 直接打开 COM7 说话**。

---

## 1. 硬件与接线

| 项目 | 说明 |
|---|---|
| 控制板 | FYSETC E4（ESP32-WROOM-32，板载 4×TMC2209，CH340 USB 串口） |
| 串口 | `COM7`，仅用于 LX200 通讯（Stellarium 23.4 的 LX200 直连**固定 9600 8N1**） |
| 方位轴 AZ | X 驱动位：STEP=GPIO27，DIR=GPIO26 |
| 高度轴 ALT | Y 驱动位：STEP=GPIO33，DIR=GPIO32 |
| 驱动使能 | GPIO25（四轴共用，低有效） |
| TMC2209 UART | `Serial1` RX=GPIO21 / TX=GPIO22，115200，共享总线 |
| TMC 地址/电流 | X=1、Y=3（FYSETC E4 官方 Marlin 配置），650 mA RMS，16 微步 |
| 限位开关 | **不使用**（空转演示；固件内对 ALT 做 0°~90° 软件限位） |
| 步进换算 | 200 整步 × 16 微步 × 5（虚拟减速比）/ 360° ≈ 44.44 步/度（可改，见 §7） |

> 若你的机械结构有真实减速比，请修改 `firmware/src/config.h` 中的 `AZ_GEAR_RATIO` / `ALT_GEAR_RATIO` 后重新编译烧录。

## 2. 工作原理（为什么这么设计）

Stellarium 23.4 的望远镜控制插件里，「Meade LX200 (compatible)」直连的实现是一个**极小的命令子集**（源码见 `docs/stellarium-lx200-spec.md`，附行号）：

- 串口参数硬编码 **9600 8N1、无流控**；打开串口后**不做任何握手**，立刻每 500 ms 轮询一次位置：
  - `#:GR#` → 固件应答 `HH:MM:SS#`（9 字节）
  - `#:GD#` → 固件应答 `+DD\xDFMM:SS#`（10 字节，`0xDF` 是度符号）
- goto：`#:Q#`（停止）→ `:SrHH:MM:SS#`（应答单字节 `1`）→ `:Sd+DD\xDFMM:SS#`（应答 `1`）→ `:MS#`（**立即**应答 `0`，随后异步转动）
- **Stellarium 不发任何时间/地点命令**，它认为望远镜自己知道时间地点；并且它只认**赤道坐标 RA/Dec**（没有地平坐标选项）。

因此固件必须自己完成：

1. 由 Stellarium 下发的目标 RA/Dec + 自己的 UTC + 站点经纬度，换算成目标 ALT/AZ（内置恒星时算法，默认 equinox = `JNow`，无需岁差矩阵）；
2. 驱动两轴步进到目标，并在 goto 后按恒星时率持续开环跟踪；
3. 把当前实际 ALT/AZ 反算回 RA/Dec 上报给 Stellarium（星图标记因此能正确显示）。

由于 Stellarium 不同步时间地点，**使用前需要用 `tools/set_scope.py` 把 UTC/站点写进板子**（写入 NVS，掉电/复位不丢）。Stellarium 启动脚本里固定的时间/地点必须与之一致（见 `tools/startup.ssc`）。

## 3. 目录结构

```
firmware/                  ESP32 固件（PlatformIO / Arduino-ESP32 2.0.17）
  platformio.ini           平台与库依赖（espressif32@6.9.0）
  src/                     config / astro / motion / tmc / scope / lx200 / main
  README.md                固件细节、真机验收记录、已知限制
docs/
  stellarium-lx200-spec.md 协议逐字节规格（Stellarium v23.4 源码依据 + 固件 P0/P1 清单）
  stellarium-config.md     Stellarium 配置注入、telescopes.json、启动脚本、GUI 操作
  images/                  验收截图
tools/
  set_scope.py             通过 COM7 设置固件 UTC / 站点，打印 :XG# 状态
  deploy_stellarium.py     一键写入 Stellarium 配置（config.ini + telescopes.json）
  stellarium_sim.py        验收工具：按 Stellarium 真实字节序列测协议（connect/status/goto/abort/setclock）
  mock_lx200_mount.py      无硬件自测用的 TCP 模拟固件
  startup.ssc              Stellarium 启动脚本（自动选目标 + goto，用于无人值守验收）
  telescopes.json.sample   预置望远镜 JSON 样例
  probe_serial.py / query_marlin.py / backup_flash.py / win_util.py 等探测与工具脚本
backup/                    原厂 Marlin 固件整片备份（large, 已 .gitignore）
```

## 4. 快速开始

### 4.1 安装 Python 依赖

```powershell
pip install pyserial
```

### 4.2 编译并烧录固件

```powershell
# 需要 PlatformIO（或直接用 .pio/build/fysetc_e4/firmware.bin 烧录）
cd firmware
pio run                     # 编译
pio run -t upload           # 烧录到 COM7（upload_speed=460800）
```

### 4.3 配置 Stellarium（一次性）

```powershell
python tools\deploy_stellarium.py
```

它会：
1. 备份 `%APPDATA%\Stellarium\config.ini`；
2. 打开 `[plugins_load_at_startup] TelescopeControl = true`；
3. 写入 `%APPDATA%\Stellarium\modules\TelescopeControl\telescopes.json`（望远镜名 `Fysetc AltAz`、`COM7`、启动时自动连接）。

### 4.4 给板子对时 / 设站（每次使用前）

```powershell
# UTC 时间 + 站点（东经/北纬为正；不写 --date 则用当前 UTC）
python tools\set_scope.py --lat 39.9042 --lon 116.4074 --date 2026-11-18T12:00:00
```

> 板子/ESp32 没有电池 RTC，且 **Stellarium 每次打开 COM7 会因 DTR 复位板子**；对时值写在 NVS 里，复位后保留。Stellarium 打开串口后约 **15~20 秒**才会稳定（第一次轮询超时 5 s + 插件静默 10 s，属正常）。

### 4.5 启动 Stellarium，开始控制

```powershell
& 'E:\Stellarium\stellarium.exe'
```

- 启动后望远镜会自动连接（望远镜窗口里状态为 Connected）；
- 用搜索（`F3`）选中目标 → 按 **`Ctrl+1`**：望远镜 goto 该目标，电机转动；
- `Ctrl+0` 打开 "Slew telescope to" 窗口可手动输入 RA/Dec 并 Slew；
- `Ctrl+Shift+1` = sync 到选中目标；**Abort 对 LX200 直连无效**（Stellarium 该客户端未实现 abort），要停就再触发一次 goto（新序列开头会发 `:Q#`）。

### 4.6 无 Stellarium 的协议验收（可选）

```powershell
python tools\stellarium_sim.py --port COM7 status
python tools\stellarium_sim.py --port COM7 goto --ra 00:42:44 --dec +41:16:00
python tools\stellarium_sim.py --port COM7 abort
```

## 5. 验收记录（实测）

| 项目 | 结果 |
|---|---|
| 编译 | `pio run` SUCCESS，Flash 26.1%，RAM 8.2% |
| 烧录 | `pio run -t upload` SUCCESS（COM7，460800） |
| 固件识别 | `:GVP#` → `FYSETC E4 AltAz#` |
| 位置查询 | `#:GR#` 9 字节、`#:GD#` 10 字节（含单字节 `0xDF`） |
| 时钟/站点 | `:XW<unix>#` / `:XC<lat>,<lon>#` 生效并写入 NVS，复位后保留 |
| goto | `#:Q#`→`:Sr`→`:Sd`→`:MS#` 全流程；M31 目标（RA 0.712 h / Dec +41.27°）4.5 s 内从 AZ 20.8°/ALT 0° 转到 AZ 78.46°/ALT 77.29°（理论 78.5°/77.3°） |
| 跟踪 | goto 后 `TRK=1`，ALT/AZ 步数按恒星时率缓慢变化 |
| TMC2209 | 两轴 UART 通讯正常（`TMC=OK`），650 mA、16 微步、stealthChop |
| Stellarium 连接 | 自动连接成功（`log.txt`: `ConnectionInternal "Fysetc AltAz:TelescopeServerLx200:JNow:COM7:500000"`，`Loaded successfully 1 telescopes`） |
| Stellarium goto | 启动脚本 `tools/startup.ssc` 依次 goto M31 → Vega；固件 `:XJ#` 会话日志记录到 Stellarium 发来的 `:Q`/`:Sr00:44:14`/`:Sd+41°25:14`/`:MS` 与 `:Q`/`:Sr18:37:50`/`:Sd+38°48:43`/`:MS`；星图上望远镜标记落在 Vega 上（见 `docs/e2e-verification.md`、`docs/images/`） |
| 状态持久化 | 每次打开 COM7 板子会复位，固件从 NVS 恢复上次指向/目标：实测复位后 `AZ=294.97° ALT=33.91°`、`TGT=18.6306/+38.81`（=Vega），`BOOT` 递增 |

> 完整的验收过程、截图与日志证据见 **[docs/e2e-verification.md](docs/e2e-verification.md)**。

## 6. 常见问题

| 现象 | 处理 |
|---|---|
| Stellarium 里看不到望远镜 / 自动连接失败 | 检查 `%APPDATA%\Stellarium\modules\TelescopeControl\telescopes.json` 的 `version` 必须是 `0.4.1`，且 `tcp_port`、`delay` 必填；看 `%APPDATA%\Stellarium\log.txt` |
| 连上后 15 秒内位置不动 / 日志有 reset | 正常：打开 COM7 复位板子，Stellarium 5 s 超时 + 10 s 静默后恢复 |
| 状态 Connected 但星图标记不动 | 板子固件没跑起来/波特率不是 9600/被其它程序占用 COM7；用 `tools\stellarium_sim.py status` 检查 |
| goto 后标记位置偏移很大 | 板子的 UTC/站点与 Stellarium 脚本设置不一致：每 1 分钟时间误差 ≈ 0.25° 指向误差，先用 `tools\set_scope.py` 对时 |
| 电机不转 | 检查 X/Y 电机是否插在 E4 的 X/Y 驱动位；听 TMC 是否有保持电流声；`tools\set_scope.py --status` 看 `TMC=OK` |
| Abort 无效 | Stellarium 23.4 的 LX200 直连未实现 abort；再按一次 `Ctrl+1`（会先发 `:Q#`）或断电 |
| 想恢复 3D 打印机用途 | 见 §8 恢复原厂 Marlin |

## 7. 可调参数

`firmware/src/config.h`：

- `AZ_GEAR_RATIO` / `ALT_GEAR_RATIO`：每度对应步数（默认 5:1 虚拟减速，44.44 步/度）
- `AXIS_MAX_SPEED_HZ`（默认 2000 步/s）、`AXIS_ACCEL_HZ2`（默认 800 步/s²）
- `ALT_MIN_DEG` / `ALT_MAX_DEG`（默认 0~90°）
- `TMC_RMS_CURRENT_MA`（默认 650 mA）、`TMC_MICROSTEPS`（默认 16）
- `SITE_LAT_DEG_DEFAULT` / `SITE_LON_DEG_DEFAULT`（默认北京）

## 8. 恢复原厂 Marlin 固件

板子原固件已完整备份为 `backup/marlin_e4_original_16MB.bin`（16 MB 整片，SHA256 见下），恢复：

```powershell
python -m esptool -p COM7 -b 460800 write-flash 0x0 backup\marlin_e4_original_16MB.bin
```

- 备份 SHA256：`BA1F56EA3732EC1921B26393BEFD8468966B28E15081FEDD89C74ADB8D763200`
- 也可从 FYSETC 官方仓库（FYSETC/FYSETC-E4）下载 Marlin 固件重新烧录。

## 9. 文档与参考

- `docs/stellarium-lx200-spec.md`：Stellarium v23.4 LX200 直连逐字节协议、超时/静默惩罚、固件 P0/P1 清单
- `docs/stellarium-config.md`：`telescopes.json` / `config.ini` 配置、GUI 步骤、启动脚本机制
- `firmware/README.md`：固件结构、真机验收记录、已知限制
- Stellarium 源码：<https://github.com/Stellarium/stellarium>（tag `v23.4`）
- FYSETC E4 资料：<https://github.com/FYSETC/FYSETC-E4>

## 10. 许可

本仓库代码用于个人天文 DIY；参考的 Stellarium 源码为 GPL，本仓库文档中的协议说明仅作事实性引用。固件依赖 FastAccelStepper（GPL-3.0）与 TMCStepper（GPL-3.0），如需再分发固件请遵循对应许可证。
