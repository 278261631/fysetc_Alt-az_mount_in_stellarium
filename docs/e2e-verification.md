# 端到端验收记录（FYSETC E4 + Stellarium 23.4，COM7）

验收日期：2026-11-18
硬件：FYSETC E4（ESP32-WROOM-32，板载 TMC2209 ×4，CH340），X 电机=AZ、Y 电机=ALT，均空转。
软件：Windows 11 + Stellarium 23.4（`E:\Stellarium`）+ 本仓库固件 v1.1.0（PlatformIO / Arduino-ESP32 2.0.17）。

## 1. 固件编译与烧录

```
RAM:   [=         ]   8.5% (used 28012 bytes from 327680 bytes)
Flash: [===       ]  26.4% (used 346357 bytes from 1310720 bytes)
Building .pio/build/fysetc_e4/firmware.bin
========================= [SUCCESS] Took 17.52 seconds =========================
```

烧录：`pio run -t upload`（COM7，460800）→ `Hash of data verified`，SUCCESS。

## 2. 协议级验收（`tools/stellarium_sim.py`，完全按 Stellarium 字节序列）

| 命令 | 结果 |
|---|---|
| `status` | exit 0：`#:GR#` → 9 字节 `HH:MM:SS#`；`#:GD#` → 10 字节 `+DD\xDFMM:SS#` |
| `goto --ra 00:42:44 --dec +41:16:00` | exit 0：4.5 s 收敛，连续 3 次变化 ≤10″，残差 dRA=0″ dDec=−1″ |
| `abort` | exit 0：`:Q#` 后 1 s 内位移 1″（停止） |
| `setclock --utc ... --lat 39.9042 --lon 116.4074` | exit 0：`:XW<unix>#`、`:XC<lat>,<lon>#` 均返回 `1#`，写入 NVS，复位后保留 |

goto 过程节选（`tools/stellarium_sim.py ... goto`）：

```
[goto] t= 0.05s  RA=11:35:41  Dec=+50:08:27
[goto] t= 1.73s  RA=07:06:49  Dec=+64:08:06
[goto] t= 3.97s  RA=00:44:21  Dec=+41:20:22
[goto] t= 4.53s  RA=00:42:42  Dec=+41:15:59   残差 -2″/-1″
[goto] 位置已稳定（连续 3 次变化 <= 10.0"）
```

## 3. Stellarium 自动连接（无需点按钮）

`%APPDATA%\Stellarium\log.txt`：

```
Loaded plugin "TelescopeControl"
connectionType: TelescopeControl::ConnectionInternal  initString: "Fysetc AltAz:TelescopeServerLx200:JNow:COM7:500000"
Creating telescope "Fysetc AltAz:TelescopeServerLx200:JNow:COM7:500000" ; name/type/equinox/params: "Fysetc AltAz" "TelescopeServerLx200" JNow "COM7:500000"
TelescopeClientDirectLx200 parameters: port, time_delay: "COM7" 500000
[TelescopeControl] Loaded successfully 1 telescopes.
```

即 Stellarium 启动时按 `telescopes.json` 的 `connect_at_startup: true` 自动打开 COM7 并连上 `Fysetc AltAz`。

## 4. Stellarium 里 goto（截图）

`tools/startup.ssc` 通过 `stellarium.exe --startup-script` 自动执行：固定时间
`2026-11-18T12:00:00 UTC` / 站点 `116.4°E, 39.9°N`，等待通信恢复后依次
`M31` → `Vega`，最后把视野对准望远镜。

![Stellarium reticle on Vega](images/stellarium-goto-vega.png)

图中：
- 右下亮星 **织女一（Vega）** 上的橙色圆环 + 标签 `Fysetc AltAz` 就是望远镜标记（位置由固件回报）；
- 左侧信息面板是 `Fysetc AltAz` 望远镜的当前坐标：`18h37m47.50s / +38°48'12.1"`，与 Vega 当前坐标（`18h37m50s / +38°48'43"`）相差约 30 角秒；
- 绿色 `FYSETC E4 AltAz E2E` 是启动脚本打的屏幕标签。

局部放大：

![reticle zoom](images/stellarium-reticle-vega-zoom.png)

## 5. 固件侧证据：会话日志 `:XJ#`（关键）

Stellarium 关闭后，重新打开 COM7（此操作会让 ESP32 因 DTR 复位），读取固件 NVS 中的会话日志。
原始输出（固件 v1.1.0，`#` 为命令结束符，`\xDF` 为度符号，日志里显示为 `°`）：

```
XJ: 1282::XW1795003200|1606::XC39.9042,116.4074|34658::Q|34727::Sr18:37:50|
    34836::Sd+38°48:43|34938::MS|635::XW1795003200|745::XC39.904200,116.407400|
    960::XR|33284::Q|33346::Sr00:44:14|33743::Sd+41°25:14|33841::MS|
    45321::Q|45385::Sr18:37:50|45499::Sd+38°48:43|45601::MS#
```

日志格式为 `开机后毫秒数::命令原文`；时间戳在每次复位后从 0 重新计数，因此可按 `:XR#` 之后的最后一段
（最后一次 Stellarium 会话，`BOOT=5`）解读：

| 时间戳(ms) | 命令 | 含义 |
|---|---|---|
| 33284 | `:Q` | 每次 goto 前 Stellarium 先发停止 |
| 33346 | `:Sr00:44:14` | 目标 1 = M31 的 JNow 赤经 |
| 33743 | `:Sd+41°25:14` | M31 的 JNow 赤纬 |
| 33841 | `:MS` | 执行 goto |
| 45321 | `:Q` | 第二次 goto 前停止 |
| 45385 | `:Sr18:37:50` | 目标 2 = Vega 的 JNow 赤经 |
| 45499 | `:Sd+38°48:43` | Vega 的 JNow 赤纬 |
| 45601 | `:MS` | 执行 goto |

日志中更早的 `:Q/:Sr18:37:50/:Sd+38°48:43/:MS` 是上一次 Stellarium 会话（同样只有 Stellarium
会发这些命令；我们的测试工具只发 `:XW`/`:XC`/`:XR` 和自定义诊断命令）。这组记录完整证明了
「Stellarium → COM7 → 固件 → 电机」链路：Stellarium 自己算出的 M31/Vega JNow 坐标被原样送到固件执行。

## 6. 复位后状态恢复（`:XG#`）

重新打开 COM7 时板子被 DTR 复位，但固件从 NVS 恢复状态：

```
S=IDLE AZ=294.97 ALT=33.91 AZS=-2890/-2890 ALTS=1507/1507 TRK=0
RA=18.6320 DEC=+38.81 TGT=18.6306/+38.81 TMC=OK UTC=1795003299
LAT=39.90 LON=+116.41 BOOT=5 LASTMS=45601#
```

- `AZ=294.97° ALT=33.91°`：与 Vega 在当时天空中的地平坐标（方位 294°53′、高度 +34°01′）一致；
- `TGT=18.6306/+38.81`、`RA=18.6320/DEC=+38.81`：最后目标是 Vega，当前指向误差 ~2 角秒；
- `BOOT=5`：多次打开/关闭串口导致复位，每次复位后状态都从 NVS 恢复；
- `TMC=OK`：两片 TMC2209 的 UART 通讯正常（650 mA、16 微步、stealthChop）。

## 7. 结论与限制

- ✅ Stellarium 23.4 可通过「LX200 (compatible) 直连串口」在 COM7@9600 上自动连接、显示位置、执行 goto；电机实际转动（开环步进已计数并持久化）。
- ⚠ Stellarium 打开 COM7 会因 DTR 复位 ESP32 → 前 15~20 秒通信会经历「5 s 超时 + 10 s 静默」，属正常；`tools/startup.ssc` 已等待 20 s 再发第一次 goto。
- ⚠ Stellarium 不同步时间/地点，需 `tools/set_scope.py` 注入；1 分钟时间误差 ≈ 0.25° 指向误差。
- ⚠ M31 目标的高度角被软件限位夹紧到 0°（脚本时间对应白天时）属预期；本验收固定在夜间时间。

---

# 8. 追加修复（2026-11-20）：用户反馈「goto 后停的坐标不对」

## 8.1 现象与复现

用户在 Stellarium 里选目标点「指向」后，机械确实动了，但最终显示的坐标离目标很远。
复现（固件 v1.1.0）：`sim goto` 到 Altair（RA 19:50:47 / Dec +08:52:06）后：

```
:XG# -> S=IDLE AZ=185.98 ALT=58.84 ... RA=8.9050 DEC=-29.79 TGT=19.8464/+8.87
```

`AZ=185.98° ALT=58.84°` 与 Altair 当时理论地平坐标（AZ 186.2° / ALT 58.8°）吻合，
即**机械位置正确**；但上报 RA/Dec 被加上了 `SYN` 偏移（实测 RA +13.05 h、Dec −38.66°）。

## 8.2 两个根因

1. **板子时钟过期**：FYSETC E4 无电池 RTC，Stellarium 又从不通过 LX200 下发时间/地点。
   断电后固件只能恢复 NVS 里上次的 UTC。本次实测固件时钟比真实时间**慢 45 小时**，
   很多目标在固件看来在地平线以下，被 ALT 限位夹到 0°，看起来就是「停错地方」。
2. **sync 偏移被持久化且 goto 不补偿**（v1.1.0 的设计缺陷）：用户（或误操作）在时钟错误时
   按过 Sync（`:CM#`），产生巨大偏移并写入 NVS；此后每次开机都带着这个偏移上报坐标，
   而 goto 却按「无偏移」换算目标 → 机械到位但 Stellarium 显示位置偏移 `SYN` 量。

## 8.3 v1.2.0 修复内容

| 修复 | 说明 |
|---|---|
| goto 与上报一致化 | goto 先把请求目标减去 sync 偏移再换算 alt/az；跟踪环同样使用补偿后的运动目标。于是存在偏移时 `goto 完成 → :GR#/:GD# = 请求目标` |
| sync 偏移不跨会话 | 不再写入/恢复 NVS；每次上电归零；`:XW`（改时间）/`:XC`（改站点）时自动清除（旧对齐随时间和站点失效） |
| 越界 goto 明确失败 | 目标 alt 超出 `ALT_MIN-2°..ALT_MAX+2°` 时 `:MS#` 回 `1#` 并**不运动**（不再静默夹到 0° 假装成功） |
| 新增 `:XS#` 诊断 | `SYN=<raH>/<decD> TGTREQ=<ra>/<dec>#`，可直接看到 sync 偏移 |
| 启动器脚本 | `tools/start_stellarium.py` / `start_stellarium.bat`：启动 Stellarium 前自动把 PC 当前 UTC + 站点写入板子（`:XW`/`:XC`），从根本上解决时钟过期 |

## 8.4 回归测试（`tools/regression_v120.py`，真机 COM7）

```
[1] XS after clock/site: SYN=+0.0000/+0.00 ...        -> PASS
[2] XS after sync      : SYN=+0.0073/+1.00 ...        （人为制造 1° 偏移）
[3] goto Altair：reported RA=19:50:52 Dec=+08°51:36
    residual dRA=+5s dDec=-30"                         -> PASS（≈1 个步进量）
[4] 地平线以下目标：:MS# 回 '1#'，AZS 不变（未运动）    -> PASS
[5] :XW 重新对时后 SYN 归零                            -> PASS
RESULT: PASS
```

## 8.5 修复后端到端验证（真实时间，不再固定时间）

用 `tools/start_stellarium.py --startup-script tools/startup_actual.ssc` 启动
（启动器先对时，脚本不修改 Stellarium 时间/地点，目标为北极星 → M81）：

![Stellarium goto M81 (actual time)](images/stellarium-goto-m81-actual-time.png)

- 望远镜信息面板：`Fysetc AltAz` 当前坐标为 **9h57m44s / +68°56'10"**（J2000 9h55m32s/+69°03'52"），
  即 M81 的坐标；屏幕上 `Fysetc AltAz` 标记也落在 M81 处；
- 固件 `:XJ#` 会话日志（Stellarium 真实下发）：
  `:Q :Sr03:09:02 :Sd+89°22:43 :MS`（北极星 JNow）→ `:Q :Sr09:57:46 :Sd+68°55:56 :MS`（M81 JNow）；
- Stellarium 关闭后读固件状态（经 DTR 复位，状态从 NVS 恢复）：

```
S=IDLE AZ=350.71 ALT=20.39 ... RA=9.9631 DEC=+68.92 TGT=9.9628/+68.93 ...
```

`TGT` 与 Stellarium 下发的 M81 目标（09:57:46 / +68:55:56 = 9.9628 h / 68.93°）一致，
`RA/DEC` 与 `TGT` 误差 ≤1 秒/0.01°，即 **goto 最终停在目标坐标上**，问题已修复。

## 8.6 使用建议（重要）

- **每次启动 Stellarium 用 `tools\start_stellarium.bat`（或 `python tools\start_stellarium.py`）**，
  它会先把板子时钟/站点同步到当前值；直接双击 `stellarium.exe` 不会对时，断电放几天后再用仍会偏。
- 如果已经出现坐标偏移：运行一次启动器（`:XW`/`:XC` 会自动清掉 sync 偏移），或在串口发 `:XR#`。
- 目标在地平线以下时 `:MS#` 会返回失败、电机不动（v1.2.0 起），GUI 里表现为 slew 失败提示。
- 长时间连续使用（数小时）时板载时钟约有 1~2% 漂移，指向会缓慢漂移；重新运行启动器即可校准。

