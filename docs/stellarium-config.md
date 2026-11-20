# Stellarium 23.4 望远镜预置配置（Fysetc AltAz / COM7 / LX200）

> 目标：把「Fysetc AltAz」预置成 Stellarium 23.4 的直连 LX200 串口望远镜（COM7），并让它在 Stellarium 启动时**自动连接**，无需点 GUI。
> 所有格式均来自 v23.4 源码（行号见 §8）。**重要纠正**：望远镜列表**不在 config.ini**，而在 `%APPDATA%\Stellarium\modules\TelescopeControl\telescopes.json`；config.ini 只负责「启动时加载插件」和显示选项。
>
> **使用提醒**：板子没有电池 RTC，而 Stellarium 直连 LX200 不同步时间/地点。日常请用启动器
> `tools\start_stellarium.py`（或双击 `tools\start_stellarium.bat`）启动 Stellarium，它会先把 PC 当前
> UTC 与站点通过 `:XW`/`:XC` 写进板子；否则断电后时钟过期会导致 goto 停错位置。详见
> `docs/e2e-verification.md` §8。

---

## 1. 结论摘要

| 项目 | 值 | 依据 |
|---|---|---|
| 望远镜列表文件 | `%APPDATA%\Stellarium\modules\TelescopeControl\telescopes.json`（用户目录，不是安装目录） | `TC.cpp:696-733` |
| JSON 顶层版本 | `"version": "0.4.1"`（写错会被重命名备份并弹警告，脚本启动会被卡住！） | `plugins/TelescopeControl/CMakeLists.txt`；`TC.cpp:761-776` |
| 连接类型 | `"connection": "internal"`（本机串口直连，嵌入式 LX200 driver） | `TC.cpp:105-111,1398-1401` |
| 设备模型 | `"device_model": "Meade LX200 (compatible)"`（→ `TelescopeServerLx200` → `TelescopeClientDirectLx200`） | `device_models.json`；`TelescopeClient.cpp:92-95` |
| 串口名 | `"serial_port": "COM7"` | `TC.cpp:848,1086` |
| 波特率 | **无此字段**；Stellarium 固定 9600 8N1 | `common/SerialPort.cpp:75,127` |
| 必填但内部连接不用的键 | `"tcp_port": 10001`（必须 >1023，否则整条记录被丢弃） | `TC.cpp:918-925`；`TelescopeControl.hpp:233-235` |
| 必填延迟 | `"delay": 500000`（微秒，>0；默认 0.5 s） | `TC.cpp:928-935`；`TelescopeControl.hpp:239` |
| 自动连接 | `"connect_at_startup": true` | `TC.cpp:938-1014` |
| 坐标系统 | `"equinox": "JNow"`（推荐）或 `"J2000"` | `TC.cpp:824-830` |
| 插件开机加载 | config.ini `[plugins_load_at_startup]` 里 `TelescopeControl = true` | 本机 config.ini 第 527-552 行 |

> **前端要求**：固件 LX200 串口必须是 **9600 8N1 无流控**（`SerialPort.cpp:75/127` 硬编码，插件无 baud 选项）。115200 只用于 esptool 烧录。

---

## 2. config.ini 最小改动

文件：`%APPDATA%\Stellarium\config.ini`（本机 `C:\Users\Administrator\AppData\Roaming\Stellarium\config.ini`）。
当前状态（已核实）：第 527 行 `[plugins_load_at_startup]` 下第 **551 行** `TelescopeControl = false`；文件中**没有** `[TelescopeControl]` 节；用户目录下也还没有 `modules\TelescopeControl\` 目录。

### 2.1 必改（1 行）

```ini
[plugins_load_at_startup]
...
TelescopeControl                          = true
```

### 2.2 可选（显示项，插件首次加载后会自动创建该节；不改也行）

```ini
[TelescopeControl]
flag_telescope_reticles                   = true
flag_telescope_labels                     = true
flag_telescope_circles                    = true
telescope_labels_font_size                = 13
color_telescope_reticles                  = 0.6,0.4,0
color_telescope_labels                    = 0.6,0.4,0
color_telescope_circles                   = 0.6,0.4,0
flag_use_server_executables               = false
flag_enable_telescope_logs                = false
```

（键名与默认值见 `TC.cpp:601-694`；`[TelescopeControl]` 节由 `saveConfiguration()` 在插件首次 init 时自动写入，手写等价。）

### 2.3 可选：开机自动运行的 .ssc 启动脚本

```ini
[scripts]
startup_script                            = E:/github/fysetc_Alt-az_mount_in_stellarium/tools/startup.ssc
```

（`StelApp.cpp:424-433`：优先用 `--startup-script` 指定的文件，否则读 `scripts/startup_script`，默认 `startup.ssc`。）

> 注意：不要改 `[projection]` 等其它节；改之前备份 config.ini。

---

## 3. telescopes.json 完整可写入样例

路径（必须自己建目录）：`C:\Users\Administrator\AppData\Roaming\Stellarium\modules\TelescopeControl\telescopes.json`

```json
{
    "version": "0.4.1",
    "1": {
        "name": "Fysetc AltAz",
        "connection": "internal",
        "equinox": "JNow",
        "device_model": "Meade LX200 (compatible)",
        "serial_port": "COM7",
        "tcp_port": 10001,
        "delay": 500000,
        "connect_at_startup": true
    }
}
```

要点（全部来自 `TC.cpp:721-1028` 的校验逻辑）：
- 顶层 `version` 必须 **"0.4.1"**（与插件编译时常量一致），否则文件会被重命名成 `telescopes.json.backup.<时间>` 并弹模态警告框，望远镜丢失。
- slot 号键 `"1"` 必须是 1..9（`MIN_SLOT_NUMBER=1`、`SLOT_COUNT=9`，`TelescopeControl.hpp:228-231`）。
- `name` 非空；`connection` 必须是 `virtual/internal/local/remote/RTS2/INDI/ASCOM` 之一（`TC.cpp:105-111`）。
- `equinox` 必须是 `J2000` 或 `JNow`。
- `device_model` 必须是 `device_models.json` 里的 **name**（`TC.cpp:857-864`），LX200 三件套任选其一均可：
  `"Meade LX200 (compatible)"`、`"Meade AutoStar compatible"`、`"Losmandy G-11"`、`"Wildcard Innovations Argo Navis (Meade mode)"`、`"Meade ETX70 (#494 Autostar, #506 CCS)"`。
- `serial_port` 非空（`COM7`）。
- **`tcp_port` 必填且 1024..65535**（内部连接并不使用，但不填/填 0 会导致整条记录被删除，`TC.cpp:918-925`）。
- **`delay` 必填且 1..10000000 微秒**（内部连接会用它），缺省 0 会被判定无效并删除记录（`TC.cpp:928-935`）。
- `connect_at_startup: true` → Stellarium 启动、插件 init 阶段直接 `startClientAtSlot()` 打开 COM7（`TC.cpp:968-1002`），**无需任何点击**。
- 可选 `"circles": [0.5, 1.0]`（视场圈，度）。
- 文件编码 UTF-8（无 BOM 最稳），标准 JSON 即可。
- 不要写 `"baudRate"`、`"serverName"`、`"serverPort"`——v23.4 没有这些字段（它们来自 RemoteControl/旧版本配置），写了会被忽略，不报错。

### 3.1 部署步骤（PowerShell）

```powershell
$dir = Join-Path $env:APPDATA 'Stellarium\modules\TelescopeControl'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item 'E:\github\fysetc_Alt-az_mount_in_stellarium\tools\telescopes.json.sample' (Join-Path $dir 'telescopes.json') -Force
# 改 config.ini: [plugins_load_at_startup] TelescopeControl = true （见 §2.1）
# 启动 Stellarium 后用日志确认：
Select-String -Path (Join-Path $env:APPDATA 'Stellarium\log.txt') -Pattern 'TelescopeControl|TelescopeClientDirectLx200' | Select-Object -Last 20
```

日志成功标志：
- `[TelescopeControl] Loaded successfully 1 telescopes.`
- `TelescopeClientDirectLx200 parameters: port, time_delay: COM7 500000`
- 若出现 `cannot open serial device COM7` → 串口被占用/不存在（Stellarium 独占打开，见下文）。
- 若出现 `Unable to load telescope ...` → JSON 字段/版本问题（按 §3 校验项逐条对）。

> 自动连接失败不会弹窗，望远镜会从列表消失。要重试：打开望远镜窗口（见 §4）点 **Start**。

---

## 4. GUI 手动添加/操作步骤（不写 JSON 时的等价路径）

### 4.1 打开望远镜窗口
- `F2`（Configuration 窗口）→ **Plugins** 标签 → 选 **Telescope Control** → 点该插件的 **Configure** 按钮；或
- `Ctrl+0` 打开「Slew telescope to」窗口 → 点 **Configure telescopes...** 按钮。
- 窗口标题为 **Telescopes**（列表列：# / Status / Type / Name）。

### 4.2 添加望远镜（Add）
1. 点 **Add**（"Add a new telescope"）；
2. 「Telescope Configuration」窗口：
   - **Telescope controlled by:** 选 **“Stellarium, directly through a serial port”**（第 1 个单选，即 direct/`internal`）；
   - **Name:** `Fysetc AltAz`
   - **Connection delay:** `0.5`（秒 → JSON 里是 500000 微秒）
   - **Coordinate system:** 选 **“Equinox of the date (JNow)”**（推荐；或 J2000）
   - ☑ **“Start/connect at startup”**
   - **Device settings → Serial port:** 手工输入 `COM7`（下拉框可编辑）
   - **Device model:** `Meade LX200 (compatible)`
3. 点 **OK**。

### 4.3 连接/断开（直接连接专用按钮文字）
在 **Telescopes** 列表里选中该行：
- 状态 `Stopped`/`NA` 时按钮显示 **Start**（"Start the selected local telescope"）；点它 → COM7 被打开、状态 Connected；
- 已连接时按钮显示 **Stop**；点它 → 关闭串口、状态 Stopped。
（外部/远程连接才是 Connect/Disconnect，见 `TelescopeDialog.cpp:944-989`。）

### 4.4 让望远镜 goto
- 鼠标/搜索（`F3`）选中目标（如 Sirius）→ `Ctrl+1` = “Move telescope #1 to selected object”；
- 或 `Ctrl+0` 坐标窗：输入 RA/Dec（HMS/DMS/十进制三选一），下拉选 `Fysetc AltAz`，点 **Slew**；
- 该窗口还有 **Sync** 按钮（发 `:CM#`）和 **Abort** 按钮（对 LX200 无效，见协议文档 §8）。
- `Alt+1` = 转向屏幕中心；`Ctrl+Shift+1` = sync 到选中目标；`Ctrl+Shift+Alt+1` = abort（LX200 无效）。

### 4.5 快捷键汇总（v23.4 实测源码）
| 快捷键 | 功能 | 依据 |
|---|---|---|
| `Ctrl+1`..`Ctrl+9` | Move telescope #N to selected object（goto） | `TC.cpp:167-174` |
| `Alt+1`..`Alt+9` | Move telescope #N to screen center | `TC.cpp:176-180` |
| `Ctrl+Shift+1`..`9` | Sync telescope #N to selected object | `TC.cpp:182-186` |
| `Ctrl+Shift+Alt+1`..`9` | Abort slew（LX200 直连无效） | `TC.cpp:188-192` |
| `Ctrl+0` | 打开 Slew telescope to 窗口 | `TC.cpp:205` |
| `F2` | Configuration（Plugins 标签里配置插件） | 主程序 |

---

## 5. 启动脚本 .ssc（端到端自动验收）

### 5.1 机制（已核对源码）
- `stellarium.exe --startup-script <文件>`：`src/CLIProcessor.cpp:120`（帮助文本）、`:202`（解析）、`:274-276`（`qApp->setProperty("onetime_startup_script", …)`）；也支持配置 `[scripts] startup_script`（默认 `startup.ssc`）——`src/core/StelApp.cpp:424-433`。
- 执行时机：`StelApp::initScriptMgr()` 在 `StelMainView.cpp:978-981` **于所有插件 init 之后**被调用，且用 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 投递，所以脚本在事件循环启动后才跑。⇒ **插件（含 `connect_at_startup` 的自动连接）先执行，脚本后执行**；脚本里仍建议 `core.wait(2)` 等串口握手完成。
- 路径：绝对路径直接可用；相对路径在 `scripts` 目录下找（`StelScriptMgr.cpp:825-832`）。
- 脚本引擎把**所有模块按 objectName 暴露为全局对象**（`StelScriptMgr.cpp:536-550`），`TelescopeControl` 的 objectName 在 `TC.cpp:103` 设置，其 `slewTelescopeToSelectedObject/…` 是 **public slots**（`TelescopeControl.hpp:244,345-375`）→ 脚本可直接调用。
- 脚本结束后 Stellarium 继续运行（不会自动退出）。

### 5.2 可直接用的 `tools/startup.ssc`（与 telescop.json 配套）

```javascript
// Stellarium 23.4 startup script (--startup-script)
// 1) 固定时间/地点（必须与固件自带的 UTC/经纬度一致，否则星图标记与指向对不上）
core.setDate("2026-11-18T12:00:00", "utc");
core.setObserverLocation(116.4, 39.9, 30, 1, "Beijing");

// 2) 等 TelescopeControl 自动连接 + 首轮 GR/GD 轮询完成。
//    注意：打开 COM7 会让 ESP32 因 DTR 复位，Stellarium 首轮查询超时(5s)后会清队列
//    并静默 10s；因此第一次 goto 前至少等 20s，否则第一条 goto 会被静默期丢弃。
core.wait(20);

// 3) 选目标并 goto 望远镜 #1（等价 Ctrl+1）
core.selectObjectByName("M31", false);
TelescopeControl.slewTelescopeToSelectedObject(1);

// 4) 等 goto（按固件速度调整）
core.wait(12);

// 5) 可选：再转到另一个目标，证明连续 goto 正常
core.selectObjectByName("Vega", false);
TelescopeControl.slewTelescopeToSelectedObject(1);
core.wait(10);

// 6) 可选：sync 测试（发 :CM#；对 LX200 有效）
// TelescopeControl.syncTelescopeWithSelectedObject(1);

// 7) 可选：abort 测试（注意：LX200 直连未实现，调用只会写日志、不会发 :Q#）
// TelescopeControl.abortTelescopeSlew(1);
```

启动：
```powershell
& 'E:\Stellarium\stellarium.exe' --startup-script 'E:\github\fysetc_Alt-az_mount_in_stellarium\tools\startup.ssc'
```

注意：
- `core.setObserverLocation(经度, 纬度, 海拔米, …)` 的**参数顺序是 经度在前**（`StelMainScriptAPI.hpp:405`）。
- `core.setDate` 默认按 UTC 解释；`core.wait(秒)` 阻塞脚本线程但不阻塞主事件循环（`StelMainScriptAPI.hpp:913-923`）。
- 若脚本执行时望远镜还没连上，`telescopeGoto` 内部会因 `!isConnected()` 直接返回（`TCDL.cpp:110-111`）→ 用 `core.wait` 保证已连接。
- 脚本无法直接发 `:Q#`（Abort 对 LX200 无效）；需要中途停止时，在脚本里再触发一次 goto 即可（新 goto 会先发 `:Q#`）。

---

## 6. RemoteControl（HTTP API）现状与替代

- 23.4 的 HTTP API 由 RemoteControl 插件提供，配置在 config.ini **`[RemoteControl]`** 节（本机已存在）：`autostart = true`、`port = 8090`、`min_threads/max_threads/...`（`plugins/RemoteControl/src/RemoteControl.cpp:297-322`）。
- `startServer()` 用 QtWebApp 的 `HttpListenerSettings.port` 创建 `HttpListener`（`RemoteControl.cpp:261-275`）；该结构体**只有 port，没有 host 字段**，即固定绑定全部接口、无法只监听 127.0.0.1。端口在 1024..65535 内任意可改。
- 报错 `HttpListener: Cannot bind on port 8090: Operation on socket is not supported`（WSAEOPNOTSUPP）属于 QTcpServer/QtWebApp 在该 Windows 环境下的 bind 失败，与 LX200/望远镜无关。可尝试：换端口（如 8091/18090）、关掉可能占用/拦截端口的软件、重启。**不建议作为验收主路径**（源码层面无法配置绑定地址）。
- **替代自动化路径（推荐）**：`--startup-script` + `core.selectObjectByName` + `TelescopeControl.slewTelescopeToSelectedObject(1)`（见 §5），完全不需要 HTTP；工具侧用 `tools/stellarium_sim.py` 做协议级断言。

---

## 7. 故障排查表

| 现象 | 原因/处理 |
|---|---|
| Stellarium 启动后列表里没有望远镜 | JSON 路径/版本/字段错误。看 `log.txt` 的 `loadTelescopes()` / `Unable to load telescope` 行；版本不匹配会生成 `.backup.*` 并弹窗 |
| 状态 Connected 但星图标记不动 | 固件没应答或格式错 → 看 `log.txt` 是否有 `communicationResetReceived`/`dataReceived: error`；用 `tools/stellarium_sim.py status` 校验 |
| 连接反复失败/10 秒后才恢复 | 固件在 LX200 串口上打了 banner/日志（触发 reset 静默 10s），或应答 >5s（`:MS#` >3s） |
| `cannot open serial device COM7` | 串口被其它程序占用（Stellarium 独占打开，`SerialPort.cpp:44` 共享模式 0）；或 COM 号不对（本机 CH340=COM7） |
| 能连上但 goto 不动 | 固件必须立即回 `:MS#` 的 `0`；另外检查 `:Sr`/`:Sd` 是否回了 `1`、15 字节格式（`:Sd` 的度符号是单字节 0xDF） |
| 星图标记位置与指向明显不符 | 固件的 UTC/站点坐标与 Stellarium 不一致；用 `equinox: "JNow"` 简化换算；用 `:XW`/`:XC` 同步时间地点 |
| JSON 改了没生效 | Stellarium 退出时会重写 telescopes.json；改文件时先关 Stellarium |
| 插件开机不加载 | config.ini `[plugins_load_at_startup] TelescopeControl = true` 没改 |

---

## 8. 源码依据（v23.4）

| 内容 | 位置 |
|---|---|
| `[TelescopeControl]` 配置键 | `plugins/TelescopeControl/src/TelescopeControl.cpp:601-694` |
| telescopes.json 保存/加载/校验/自动连接 | 同上 `:696-1028`（自动连接 `:968-1002`） |
| 连接类型字符串 | 同上 `:105-111`；枚举 `TelescopeControl.hpp:92-101` |
| 内部连接 initString（name:server:equinox:serial:delay） | 同上 `:1398-1401` |
| slot/端口/延迟常量 | `TelescopeControl.hpp:228-239`（MAX_SLOT 9、BASE_TCP_PORT 10000、DEFAULT_DELAY 500000） |
| 设备模型 "Meade LX200 (compatible)"→`TelescopeServerLx200` | `plugins/TelescopeControl/resources/device_models.json` |
| 类型映射到 `TelescopeClientDirectLx200` | `plugins/TelescopeControl/src/TelescopeClient.cpp:92-95` |
| 串口 9600 8N1（硬编码） | `plugins/TelescopeControl/src/common/SerialPort.cpp:44,52-57,75,123-131` |
| 快捷键 | `TelescopeControl.cpp:163-206` |
| GUI 对话框/按钮文字 | `plugins/TelescopeControl/src/gui/telescopeDialog.ui:41,189-257`；`TelescopeConfigurationDialog.cpp:190-251,386-411` |
| 启动脚本 CLI/执行时机 | `src/CLIProcessor.cpp:120,202,274-276`；`src/core/StelApp.cpp:415-433`；`src/StelMainView.cpp:978-981` |
| 脚本 API | `src/scripting/StelMainScriptAPI.hpp:127,215,405,913-923`；`StelScriptMgr.cpp:536-550,825-832` |
| RemoteControl 配置/绑定 | `plugins/RemoteControl/src/RemoteControl.cpp:73,81-84,261-275,297-322` |
| 配置版本 0.4.1 | `plugins/TelescopeControl/CMakeLists.txt` |
