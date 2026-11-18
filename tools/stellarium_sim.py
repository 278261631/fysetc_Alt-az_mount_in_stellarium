#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stellarium_sim.py — 按 Stellarium 23.4 真实命令序列驱动/验收 LX200 固件。

为什么需要它：Stellarium 23.4 的 TelescopeControl「LX200 (compatible)」直连
只依赖极小的命令子集（详见 docs/stellarium-lx200-spec.md）：
  连接后每 500ms:  "#:GR#" -> "HH:MM:SS#" (9B)
                    "#:GD#" -> "+DD\\xDFMM:SS#" (10B, 0xDF=度数符号)
  goto:            "#:Q#" (无应答) -> ":SrHH:MM:SS#" ('1') -> ":Sd+DD\\xDFMM:SS#" ('1')
                    -> ":MS#" ('0')
  sync:            同上但最后是 ":CM#" ('0')
  注意：Stellarium 23.4 **不发** :GVP#、:GA#、:GZ#、:D#、时间/地点命令。
  本工具在 connect 时可选发送产品名/自定义扩展命令，用于识别固件与设置时钟/站点
  （真实 Stellarium 不会发，固件可只把这部分当 P1）。

用法示例：
  python tools/stellarium_sim.py --port COM7 connect
  python tools/stellarium_sim.py --port COM7 status
  python tools/stellarium_sim.py --port COM7 goto --ra 06:45:09 --dec -16:42:58
  python tools/stellarium_sim.py --port COM7 abort
  python tools/stellarium_sim.py --tcp 127.0.0.1:5555 connect      # 配合 mock 自测
  python tools/stellarium_sim.py --port COM7 connect --utc 2026-11-18T04:00:00 --lat 39.9 --lon 116.4

串口默认 9600 8N1 无流控 —— 这是 Stellarium 23.4 硬编码的值（SerialPort.cpp:75/127），
不是可选项。--baud 115200 仅供固件自定义调试使用，对真实 Stellarium 无效。

退出码：0=通过，1=协议/验收失败，2=参数或串口/TCP 打不开。
"""

import argparse
import datetime
import math
import re
import socket
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # 允许只用 --tcp 模式
    serial = None

DEG_SYM = 0xDF  # ISO-8859-1 degree sign, Stellarium 的 :Sd 和 :GD# 都要求这个单字节
DEFAULT_POLL = 0.5  # Stellarium 固定 500ms 轮询 (TCDL.cpp:243)


def disp_answer(data):
    """把应答 bytes 转成可打印字符串（0xDF -> '°'，其它非 ASCII 转义），避免 Windows GBK 控制台崩溃。"""
    if isinstance(data, (bytes, bytearray)):
        text = bytes(data).decode("latin-1")
    else:
        text = str(data)
    out = []
    for ch in text:
        if ch == "\xdf":
            out.append("\u00b0")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("\\x%02x" % ord(ch))
    return "".join(out)


# --------------------------------------------------------------------------- #
# 传输层
# --------------------------------------------------------------------------- #
class TransportError(Exception):
    pass


class Transport:
    name = "?"

    def open(self):
        raise NotImplementedError

    def close(self):
        raise NotImplementedError

    def write(self, data):
        raise NotImplementedError

    def read(self, n, timeout):
        raise NotImplementedError

    def reset_input(self):
        raise NotImplementedError


class SerialTransport(Transport):
    def __init__(self, port, baud, read_timeout=1.0, pulse_reset=False):
        if serial is None:
            raise TransportError("未安装 pyserial（pip install pyserial）")
        self.port = port
        self.baud = baud
        self.read_timeout = read_timeout
        self.pulse_reset = pulse_reset
        self.ser = None
        self.name = "serial:%s@%d" % (port, baud)

    def open(self):
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.read_timeout,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False,
            )
        except Exception as e:
            raise TransportError("打开 %s 失败: %s" % (self.port, e))
        # 尽量别复位 ESP32（Marlin 板常用 DTR/RTS 自动复位）。
        # 注意：pyserial 打开瞬间可能已拉高，硬件上是否复位取决于板子电路。
        try:
            if self.pulse_reset:
                self.ser.dtr = False
                self.ser.rts = True
                time.sleep(0.1)
                self.ser.rts = False
                time.sleep(0.1)
            else:
                self.ser.dtr = False
                self.ser.rts = False
        except Exception:
            pass

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def write(self, data):
        try:
            self.ser.write(data)
            self.ser.flush()
        except Exception as e:
            raise TransportError("串口写失败: %s" % e)

    def read(self, n, timeout):
        # 返回"当前已到达的字节"（最多 n），不要等满 n：这样轮询才不会被拖慢。
        old = self.ser.timeout
        self.ser.timeout = timeout
        try:
            avail = self.ser.in_waiting
            if avail > 0:
                return self.ser.read(min(n, avail))
            return self.ser.read(1)
        finally:
            self.ser.timeout = old

    def reset_input(self):
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass


class TcpTransport(Transport):
    def __init__(self, host, port, connect_timeout=3.0):
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.sock = None
        self.name = "tcp:%s:%d" % (host, port)

    @classmethod
    def parse(cls, spec):
        if ":" not in spec:
            raise TransportError("--tcp 需要 host:port 形式，例如 127.0.0.1:5555")
        host, _, port = spec.rpartition(":")
        try:
            return cls(host, int(port))
        except ValueError:
            raise TransportError("--tcp 端口不是整数: %r" % spec)

    def open(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), self.connect_timeout)
            self.sock.settimeout(0.2)
        except Exception as e:
            raise TransportError("连接 %s:%d 失败: %s" % (self.host, self.port, e))

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def write(self, data):
        try:
            self.sock.sendall(data)
        except Exception as e:
            raise TransportError("TCP 写失败: %s" % e)

    def read(self, n, timeout):
        # 单次 recv：有数据立即返回（可能少于 n），超时返回空；由调用方循环填充。
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(n)
        except socket.timeout:
            return b""
        except Exception as e:
            raise TransportError("TCP 读失败: %s" % e)
        if not data:
            raise TransportError("TCP 对端关闭连接")
        return data

    def reset_input(self):
        self.sock.settimeout(0.05)
        try:
            while True:
                if not self.sock.recv(4096):
                    break
        except Exception:
            pass


# --------------------------------------------------------------------------- #
# LX200 协议实现（严格按 Stellarium 23.4 行为）
# --------------------------------------------------------------------------- #
class ProtocolError(Exception):
    pass


class Lx200:
    def __init__(self, transport, verbose=False, timeout=5.0):
        self.t = transport
        self.verbose = verbose
        self.timeout = timeout
        self._buf = bytearray()
        self._t0 = time.time()

    # ---------------- 基础 IO ----------------
    def _log(self, msg):
        if self.verbose:
            print("  [%+7.3fs] %s" % (time.time() - self._t0, msg))

    def tx(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self._log("TX %r" % (data,))
        self.t.write(data)

    def _fill(self, need, timeout):
        end = time.time() + timeout
        while len(self._buf) < need:
            remain = end - time.time()
            if remain <= 0:
                return False
            chunk = self.t.read(need - len(self._buf), remain)
            if not chunk:
                continue
            self._buf.extend(chunk)
            self._log("RX raw %r" % (chunk,))
        return True

    def read_until_hash(self, timeout=None):
        """读到第一个 '#'（含）。返回含 '#' 的 bytes；超时返回已有内容。"""
        timeout = self.timeout if timeout is None else timeout
        end = time.time() + timeout
        while b"#" not in self._buf:
            remain = end - time.time()
            if remain <= 0:
                break
            chunk = self.t.read(64, remain)
            if not chunk:
                continue
            self._buf.extend(chunk)
            self._log("RX raw %r" % (chunk,))
        idx = self._buf.find(b"#")
        if idx < 0:
            out = bytes(self._buf)
            self._buf.clear()
            if out:
                self._log("RX (no '#') %r" % (out,))
            return b""
        out = bytes(self._buf[: idx + 1])
        del self._buf[: idx + 1]
        return out

    def read_exact(self, n, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        if not self._fill(n, timeout):
            got = bytes(self._buf)
            self._buf.clear()
            raise ProtocolError("期望 %d 字节，实际 %r" % (n, got))
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def read_answer(self, timeout=None):
        """读一条 '#' 结尾的应答；忽略首字节 '#'（Stellarium 的 readAnswerFromBuffer 也这么做）。"""
        for _ in range(3):
            ans = self.read_until_hash(timeout)
            if not ans:
                raise ProtocolError("应答超时（%ss）" % (self.timeout if timeout is None else timeout))
            if ans == b"#":
                self._log("丢弃前导 '#'")
                continue
            return ans
        raise ProtocolError("连续只收到 '#' 字节")

    def drain(self, quiet=0.15):
        """清掉可能残留的字节（固件启动 banner 等）。"""
        self.t.reset_input()
        time.sleep(quiet)
        self.t.reset_input()
        self._buf.clear()

    def wait_ready(self, timeout=25.0):
        """Wait until the mount answers a position query.

        Opening COM7 usually pulls DTR/RTS and resets the ESP32 board; the boot
        ROM then spits garbage at 115200 (read as noise at 9600) and the first
        commands are lost.  Stellarium itself only recovers via its 5 s timeout
        + 10 s communication reset, so a test tool should just wait it out.
        """
        end = time.time() + timeout
        attempt = 0
        while time.time() < end:
            attempt += 1
            self.t.reset_input()
            self._buf.clear()
            self.tx(b"#:GR#")
            try:
                ans = self.read_until_hash(0.4)
                if ans:
                    hours, long_fmt = self.parse_ra_hms(ans)
                    self._log("ready after %.1fs (%d attempts): RA=%s"
                              % (timeout - (end - time.time()), attempt, disp_answer(ans)))
                    return True
            except (ProtocolError, ValueError):
                pass
            time.sleep(0.2)
        raise ProtocolError("固件在 %.1fs 内无有效应答（是否被其它程序占用 / 波特率不对 / 固件没跑起来）"
                            % timeout)

    # ---------------- 格式化/解析 ----------------
    @staticmethod
    def fmt_ra_hms(ra_seconds):
        ra_seconds = int(round(ra_seconds)) % 86400
        h = ra_seconds // 3600
        m = (ra_seconds // 60) % 60
        s = ra_seconds % 60
        return "%02d:%02d:%02d#" % (h, m, s)

    @staticmethod
    def fmt_dec_dms(dec_arcsec):
        dec_arcsec = int(round(dec_arcsec))
        sign = "+" if dec_arcsec >= 0 else "-"
        a = abs(dec_arcsec)
        d = a // 3600
        m = (a // 60) % 60
        s = a % 60
        return b"%s%02d%c%02d:%02d#" % (sign.encode(), d, DEG_SYM, m, s)

    @staticmethod
    def parse_ra_hms(ans):
        """'HH:MM:SS#' / '#HH:MM:SS#' / 'HH:MM.S#' -> 小时(float)"""
        a = ans.strip()
        if a.startswith(b"#"):
            a = a[1:]
        if a.endswith(b"#"):
            a = a[:-1]
        text = a.decode("latin-1")
        m = re.match(r"^(\d{1,2}):(\d{2})(?::(\d{2}(?:\.\d*)?))?$", text)
        if not m:
            raise ProtocolError("RA 应答格式不合法: %r" % ans)
        h = int(m.group(1))
        mi = int(m.group(2))
        sec = float(m.group(3)) if m.group(3) else 0.0
        return (h + mi / 60.0 + sec / 3600.0) % 24.0, (m.group(3) is not None)

    @staticmethod
    def parse_dec_dms(ans):
        """'+DD\\xDFMM:SS#' / '#...' / 兼容 '*' 与 UTF-8 '°' -> 度(float)"""
        a = ans.strip()
        if a.startswith(b"#"):
            a = a[1:]
        if a.endswith(b"#"):
            a = a[:-1]
        sign = 1.0
        if a[:1] in (b"+", b"-"):
            sign = -1.0 if a[:1] == b"-" else 1.0
            a = a[1:]
        # 统一度符号为 0xDF
        a = a.replace(b"*", bytes([DEG_SYM])).replace("\u00b0".encode("utf-8"), bytes([DEG_SYM]))
        m = re.match(rb"^(\d{1,2})\xdf(\d{2})(?::(\d{2}(?:\.\d*)?))?$", a)
        if not m:
            raise ProtocolError("Dec 应答格式不合法: %r" % ans)
        d = int(m.group(1))
        mi = int(m.group(2))
        sec = float(m.group(3)) if m.group(3) else 0.0
        return sign * (d + mi / 60.0 + sec / 3600.0), (m.group(3) is not None)

    # ---------------- Stellarium 实际使用的查询 ----------------
    def get_ra(self):
        self.tx(b"#:GR#")               # Stellarium 真实字节（带前导 '#'）
        ans = self.read_answer()
        hours, long_fmt = self.parse_ra_hms(ans)
        return hours, ans, long_fmt

    def get_dec(self):
        self.tx(b"#:GD#")
        ans = self.read_answer()
        deg, long_fmt = self.parse_dec_dms(ans)
        return deg, ans, long_fmt

    def get_alt(self):
        self.tx(b":GA#")
        return self.read_answer(timeout=1.0)

    def get_az(self):
        self.tx(b":GZ#")
        return self.read_answer(timeout=1.0)

    def get_status(self):
        self.tx(b":XG#")
        return self.read_answer(timeout=1.5)

    def product_name(self):
        self.tx(b":GVP#")
        return self.read_answer(timeout=1.0)

    # ---------------- goto / sync / abort（Stellarium 序列） ----------------
    def stop(self):
        self.tx(b"#:Q#")                # needsNoAnswer：不读应答
        time.sleep(0.1)

    def set_selected_ra(self, ra_seconds):
        self.tx(b":Sr" + self.fmt_ra_hms(ra_seconds).encode("ascii"))
        ans = self.read_exact(1)
        if ans != b"1":
            raise ProtocolError(":Sr 应答 %r（期望 '1'）" % ans)
        self._log(":Sr OK")

    def set_selected_dec(self, dec_arcsec):
        self.tx(b":Sd" + self.fmt_dec_dms(dec_arcsec))
        ans = self.read_exact(1)
        if ans != b"1":
            raise ProtocolError(":Sd 应答 %r（期望 '1'）" % ans)
        self._log(":Sd OK")

    def goto_selected(self, timeout=None):
        self.tx(b":MS#")
        first = self.read_exact(1, timeout=timeout or 3.0)   # Stellarium 只消费 1 字节
        if first == b"0":
            self._log(":MS# OK（slew started）")
            return
        if first in (b"1", b"2"):
            rest = self.read_until_hash(timeout=2.0)
            raise ProtocolError(":MS# 报告失败 %r: %r" % (first, rest))
        raise ProtocolError(":MS# 应答 %r（期望 '0'/'1'/'2'）" % first)

    def sync_selected(self, timeout=None):
        self.tx(b":CM#")
        first = self.read_exact(1, timeout=timeout or 3.0)
        if first != b"0":
            raise ProtocolError(":CM# 应答 %r（期望 '0'）" % first)

    # ---------------- 自定义扩展（Stellarium 不发，仅测试设置用） ----------------
    def set_clock(self, dt_utc):
        if dt_utc.tzinfo is None:
            dt_utc = dt_utc.replace(tzinfo=datetime.timezone.utc)
        unix = int(dt_utc.timestamp())
        self.tx(":XW%d#" % unix)
        try:
            ans = self.read_answer(timeout=1.0)
            return ans
        except ProtocolError:
            return None

    def set_site(self, lat, lon):
        self.tx(":XC%.6f,%.6f#" % (lat, lon))
        try:
            ans = self.read_answer(timeout=1.0)
            return ans
        except ProtocolError:
            return None


# --------------------------------------------------------------------------- #
# 子命令
# --------------------------------------------------------------------------- #
def cmd_connect(lx, args):
    print("[connect] %s" % lx.t.name)
    lx.drain()
    lx.wait_ready()

    # 1) Stellarium 连接后立刻做的就是位置轮询，这里先复现确认固件在线
    ok = True
    ra = dec = None
    for i in range(3):
        try:
            ra, ra_raw, ra_long = lx.get_ra()
            dec, dec_raw, dec_long = lx.get_dec()
        except ProtocolError as e:
            print("[connect] 第 %d 轮位置查询失败: %s" % (i + 1, e))
            ok = False
            break
        print("[connect] 第 %d 轮  RA=%s (%.5fh)   Dec=%s (%+.4f deg)  long_format=%s"
              % (i + 1, disp_answer(ra_raw), ra, disp_answer(dec_raw), dec, ra_long and dec_long))
        if not (ra_long and dec_long):
            print("[connect] 警告: 固件返回短格式；Stellarium 会发 '#:U#' 切换。请改用长格式。")
        time.sleep(DEFAULT_POLL)
    if not ok:
        return 1

    # 2) 产品名（Stellarium 不发；固件可选）
    try:
        name = lx.product_name()
        print("[connect] :GVP# -> %r（Stellarium 23.4 不会发这条，仅诊断用）" % name)
    except ProtocolError:
        print("[connect] :GVP# 无应答（P1 可选，不影响 Stellarium）")
        lx.drain(0.05)

    # 3) 状态（自定义 :XG#）
    try:
        st = lx.get_status()
        print("[connect] :XG# -> %r" % st)
    except ProtocolError:
        print("[connect] :XG# 无应答（P1 可选）")
        lx.drain(0.05)

    # 4) 时间/站点（Stellarium 不会同步；固件必须自带，可用自定义命令设置）
    if args.utc or args.lat is not None or args.lon is not None:
        if args.utc:
            dt = parse_utc_arg(args.utc)
        else:
            dt = datetime.datetime.now(datetime.timezone.utc)
        ans = lx.set_clock(dt)
        print("[connect] :XW%d# (UTC %s) -> %r" % (int(dt.timestamp()), dt.isoformat(), ans))
        if ans is None:
            print("[connect] 警告: 固件未实现 :XW（自定义命令），时钟未同步")
        if args.lat is not None and args.lon is not None:
            ans = lx.set_site(args.lat, args.lon)
            print("[connect] :XC%.6f,%.6f# -> %r" % (args.lat, args.lon, ans))
            if ans is None:
                print("[connect] 警告: 固件未实现 :XC（自定义命令），站点未同步")
        lx.drain(0.05)

    print("[connect] 通过")
    return 0


def cmd_status(lx, args):
    print("[status] %s" % lx.t.name)
    lx.drain()
    lx.wait_ready()
    exit_code = 0
    try:
        ra, ra_raw, ra_long = lx.get_ra()
        print("  RA  = %-12s  %.6f h" % (disp_answer(ra_raw), ra))
        if not ra_long:
            print("  警告: RA 短格式（Stellarium 会发 #:U# 切换）")
    except ProtocolError as e:
        print("  RA  失败: %s" % e)
        exit_code = 1
    try:
        dec, dec_raw, dec_long = lx.get_dec()
        print("  Dec = %-12s  %+.6f deg" % (disp_answer(dec_raw), dec))
        if not dec_long:
            print("  警告: Dec 短格式")
    except ProtocolError as e:
        print("  Dec 失败: %s" % e)
        exit_code = 1

    try:
        print("  ALT = %r" % lx.get_alt())
    except ProtocolError:
        print("  ALT 无应答（Stellarium 23.4 不查 :GA#，P1 可选）")
    try:
        print("  AZ  = %r" % lx.get_az())
    except ProtocolError:
        print("  AZ  无应答（Stellarium 23.4 不查 :GZ#，P1 可选）")
    try:
        print("  XG  = %r" % lx.get_status())
    except ProtocolError:
        print("  XG  无应答（自定义诊断，P1 可选）")

    print("[status] %s" % ("通过" if exit_code == 0 else "失败"))
    return exit_code


def cmd_goto(lx, args):
    print("[goto] %s" % lx.t.name)
    target_ra_sec = args.ra_seconds
    target_dec_arcsec = args.dec_arcsec
    print("[goto] target RA=%s (%.3f s = %.5f h)  Dec=%s (%+.2f arcsec = %+.4f deg)"
          % (argparse_ra_display(args), target_ra_sec, target_ra_sec / 3600.0,
             argparse_dec_display(args), target_dec_arcsec, target_dec_arcsec / 3600.0))
    lx.drain()
    lx.wait_ready()

    # 与 Stellarium sendGoto() 完全一致的顺序
    lx.stop()                              # "#:Q#"，无应答
    lx.set_selected_ra(target_ra_sec)      # ":SrHH:MM:SS#" -> '1'
    lx.set_selected_dec(target_dec_arcsec) # ":Sd+DD\xDFMM:SS#" -> '1'
    lx.goto_selected()                     # ":MS#" -> '0'

    # 之后 Stellarium 每 500ms 轮询 #:GR# / #:GD#，这里复现并等稳定
    t_start = time.time()
    last = None
    stable = 0
    last_ra = last_dec = None
    while time.time() - t_start < args.goto_timeout:
        try:
            ra, ra_raw, _ = lx.get_ra()
            dec, dec_raw, _ = lx.get_dec()
        except ProtocolError as e:
            print("[goto] 轮询失败: %s" % e)
            return 1
        cur = (ra * 3600.0, dec * 3600.0)  # 秒 / 角秒
        err_ra = 0.0
        if target_ra_sec is not None:
            d = (cur[0] - target_ra_sec + 43200.0) % 86400.0 - 43200.0
            err_ra = d
        err_dec = (cur[1] - target_dec_arcsec) if target_dec_arcsec is not None else 0.0
        dec_abs = abs(cur[1])
        dec_sign = "-" if cur[1] < 0 else "+"
        print("[goto] t=%5.2fs  RA=%02d:%02d:%02d  Dec=%s%02d:%02d:%02d  dRA=%+8.2f\"  dDec=%+8.2f\""
              % (time.time() - t_start,
                 int(cur[0] // 3600) % 24, int(cur[0] // 60) % 60, int(cur[0]) % 60,
                 dec_sign, int(dec_abs // 3600), int(dec_abs // 60) % 60, int(dec_abs) % 60,
                 err_ra, err_dec))
        if last is not None:
            moved = abs(cur[0] - last[0]) + abs(cur[1] - last[1])
            if moved <= args.tolerance:
                stable += 1
            else:
                stable = 0
        if stable >= args.stable:
            print("[goto] 位置已稳定（连续 %d 次变化 <= %.1f\"）；相对目标残差 dRA=%+.1f\" dDec=%+.1f\""
                  % (stable, args.tolerance, err_ra, err_dec))
            if abs(err_ra) > args.tolerance or abs(err_dec) > args.tolerance:
                print("[goto] 警告：机械已停但未到达目标（固件限位/换算/零点问题？）")
            return 0
        last = cur
        last_ra, last_dec = ra_raw, dec_raw
        time.sleep(args.poll)

    print("[goto] 超时（%.1fs）仍未稳定；最后 RA=%r Dec=%r" % (args.goto_timeout, last_ra, last_dec))
    return 1


def cmd_abort(lx, args):
    print("[abort] %s" % lx.t.name)
    lx.drain()
    lx.wait_ready()
    lx.stop()                     # "#:Q#"，无应答
    time.sleep(0.3)
    try:
        ra, ra_raw, _ = lx.get_ra()
        dec, dec_raw, _ = lx.get_dec()
        print("[abort] 停止后仍在线: RA=%r Dec=%r" % (ra_raw, dec_raw))
    except ProtocolError as e:
        print("[abort] 停止后查询失败（固件可能已停串口/无响应）: %s" % e)
        return 1
    # 再发一次并确认没有继续移动
    time.sleep(1.0)
    try:
        ra2, ra_raw2, _ = lx.get_ra()
        dec2, dec_raw2, _ = lx.get_dec()
    except ProtocolError as e:
        print("[abort] 复查失败: %s" % e)
        return 1
    moved = abs((ra2 - ra) * 3600.0) + abs((dec2 - dec) * 3600.0)
    print("[abort] 1s 内位移 %.2f\"（RA %r→%r, Dec %r→%r）" % (moved, ra_raw, ra_raw2, dec_raw, dec_raw2))
    if moved > args.tolerance:
        print("[abort] 失败：:Q# 后仍在移动")
        return 1
    print("[abort] 通过")
    return 0


def cmd_setclock(lx, args):
    print("[setclock] %s" % lx.t.name)
    lx.drain()
    lx.wait_ready()
    dt = parse_utc_arg(args.utc) if args.utc else datetime.datetime.now(datetime.timezone.utc)
    ans = lx.set_clock(dt)
    print("  :XW%d# (UTC %s) -> %r" % (int(dt.timestamp()), dt.isoformat(), ans))
    if args.lat is not None and args.lon is not None:
        ans2 = lx.set_site(args.lat, args.lon)
        print("  :XC%.6f,%.6f# -> %r" % (args.lat, args.lon, ans2))
        if ans is None or ans2 is None:
            print("  警告: 固件未实现 :XW/:XC（自定义扩展），设置未生效")
            return 1
    elif ans is None:
        print("  警告: 固件未实现 :XW（自定义扩展），设置未生效")
        return 1
    print("[setclock] 通过")
    return 0


# --------------------------------------------------------------------------- #
# 参数
# --------------------------------------------------------------------------- #
def parse_utc_arg(text):
    """--utc / --date helper: ISO 8601; naive timestamps mean UTC (not local)."""
    dt = datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=datetime.timezone.utc)
    return dt


def argparse_ra_display(args):
    return args.ra


def argparse_dec_display(args):
    return args.dec


def parse_ra_arg(text):
    """支持 'HH:MM:SS'（小时）或十进制小时（--ra 6.75）"""
    text = text.strip()
    m = re.match(r"^(\d{1,2}):(\d{1,2}):(\d{1,2}(?:\.\d+)?)$", text)
    if m:
        return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
    m = re.match(r"^(\d{1,3}):(\d{1,2}(?:\.\d+)?)$", text)
    if m:
        return int(m.group(1)) * 3600 + float(m.group(2)) * 60
    try:
        return float(text) * 3600.0  # 十进制小时
    except ValueError:
        raise argparse.ArgumentTypeError("无法解析 RA=%r（支持 HH:MM:SS 或十进制小时）" % text)


def parse_dec_arg(text):
    """支持 '+DD:MM:SS'/'-DD:MM:SS' 或十进制度（--dec -16.7）"""
    text = text.strip()
    m = re.match(r"^([+-]?)(\d{1,2}):(\d{1,2}):(\d{1,2}(?:\.\d+)?)$", text)
    if m:
        sign = -1 if m.group(1) == "-" else 1
        return sign * (int(m.group(2)) * 3600 + int(m.group(3)) * 60 + float(m.group(4)))
    try:
        return float(text) * 3600.0
    except ValueError:
        raise argparse.ArgumentTypeError("无法解析 Dec=%r（支持 +DD:MM:SS 或十进制度）" % text)


def build_parser():
    p = argparse.ArgumentParser(
        prog="stellarium_sim.py",
        description="按 Stellarium 23.4 真实序列驱动/验收 LX200 固件（默认串口 COM7@9600 8N1）",
        epilog="退出码: 0=通过, 1=协议失败, 2=参数/连接失败",
    )
    g = p.add_mutually_exclusive_group()
    g.add_argument("--port", help="串口名，例如 COM7（默认 COM7）")
    g.add_argument("--tcp", metavar="HOST:PORT", help="改为 TCP 连接（例如 127.0.0.1:5555）")
    p.add_argument("--baud", type=int, default=9600,
                   help="波特率（默认 9600 = Stellarium 23.4 硬编码值；115200 仅固件自定义调试用）")
    p.add_argument("--timeout", type=float, default=5.0, help="单条命令应答超时秒数（默认 5）")
    p.add_argument("--pulse-reset", action="store_true", help="打开串口时脉冲 DTR/RTS 复位固件（默认不复位）")
    p.add_argument("-v", "--verbose", action="store_true", help="打印 TX/RX 原始字节")

    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("connect", help="握手/读产品名/可选设置时钟站点 + 首轮位置查询")
    c.add_argument("--utc", help="设置固件时钟（自定义 :XW，ISO8601，如 2026-11-18T04:00:00）")
    c.add_argument("--lat", type=float, help="设置站点纬度（自定义 :XC，北纬为正）")
    c.add_argument("--lon", type=float, help="设置站点经度（自定义 :XC，东经为正）")
    c.set_defaults(func=cmd_connect)

    s = sub.add_parser("status", help="查询 :GR#/:GD#/:GA#/:GZ#/:XG#")
    s.set_defaults(func=cmd_status)

    gt = sub.add_parser("goto", help="按 Stellarium 顺序 :Q#/:Sr/:Sd/:MS# 并轮询到稳定")
    gt.add_argument("--ra", required=True, help="目标赤经：HH:MM:SS（小时）或十进制小时")
    gt.add_argument("--dec", required=True, help="目标赤纬：+DD:MM:SS / -DD:MM:SS 或十进制度")
    gt.add_argument("--poll", type=float, default=DEFAULT_POLL, help="轮询间隔秒（默认 0.5=Stellarium）")
    gt.add_argument("--timeout", dest="goto_timeout", type=float, default=90.0,
                    help="等待稳定总超时秒（默认 90）")
    gt.add_argument("--tolerance", type=float, default=10.0, help="稳定判定阈值（角秒，默认 10）")
    gt.add_argument("--stable", type=int, default=3, help="连续多少次变化小于阈值算稳定（默认 3）")
    gt.set_defaults(func=cmd_goto)

    a = sub.add_parser("abort", help="发 :Q# 并确认停止转动")
    a.add_argument("--tolerance", type=float, default=10.0, help="停止判定阈值（角秒，默认 10）")
    a.set_defaults(func=cmd_abort)

    sc = sub.add_parser("setclock", help="仅用自定义 :XW/:XC 设置固件时间/站点")
    sc.add_argument("--utc", help="UTC 时间 ISO8601（默认当前时间）")
    sc.add_argument("--lat", type=float, help="纬度")
    sc.add_argument("--lon", type=float, help="经度")
    sc.set_defaults(func=cmd_setclock)

    return p


def normalize_argv(argv):
    """允许 --dec -16:42:58 这种以 '-' 开头的值（argparse 默认会当成选项）。"""
    if argv is None:
        argv = sys.argv[1:]
    argv = list(argv)
    out = []
    i = 0
    while i < len(argv):
        if argv[i] in ("--dec", "--ra") and i + 1 < len(argv) and argv[i + 1].startswith("-") \
                and not argv[i + 1].startswith("--"):
            out.append(argv[i] + "=" + argv[i + 1])
            i += 2
            continue
        out.append(argv[i])
        i += 1
    return out


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    argv = normalize_argv(argv)
    parser = build_parser()
    args = parser.parse_args(argv)

    # 全局 --timeout 是"单条命令应答超时"
    cmd_timeout = args.timeout

    # goto：把 --ra/--dec 字符串转成秒/角秒
    if args.cmd == "goto":
        try:
            args.ra_seconds = parse_ra_arg(args.ra)
            args.dec_arcsec = parse_dec_arg(args.dec)
        except argparse.ArgumentTypeError as e:
            print("[error] %s" % e, file=sys.stderr)
            return 2

    try:
        if args.tcp:
            transport = TcpTransport.parse(args.tcp)
            args.port = None
        else:
            port = args.port or "COM7"
            if args.baud != 9600:
                print("[warn] Stellarium 23.4 的 LX200 串口硬编码为 9600 8N1；--baud %d 仅用于固件自定义调试。"
                      % args.baud, file=sys.stderr)
            transport = SerialTransport(port, args.baud, pulse_reset=args.pulse_reset)
        transport.open()
    except TransportError as e:
        print("[error] %s" % e, file=sys.stderr)
        return 2

    lx = Lx200(transport, verbose=args.verbose, timeout=cmd_timeout)
    try:
        rc = args.func(lx, args)
    except ProtocolError as e:
        print("[error] %s" % e, file=sys.stderr)
        rc = 1
    except TransportError as e:
        print("[error] %s" % e, file=sys.stderr)
        rc = 2
    except KeyboardInterrupt:
        print("[interrupt]")
        rc = 130
    finally:
        transport.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
