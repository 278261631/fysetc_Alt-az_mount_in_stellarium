#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mock_lx200_mount.py — 无硬件时模拟 Fysetc AltAz 固件的 LX200 应答，用于自测 tools/stellarium_sim.py。

严格按 docs/stellarium-lx200-spec.md 的 P0/P1 子集实现：
  #:GR# -> "HH:MM:SS#"
  #:GD# -> "+DD\\xDFMM:SS#"
  :SrHH:MM:SS# -> "1"      :Sd+DD\\xDFMM:SS# -> "1"      :MS# -> "0"（异步转动）
  #:Q# -> 无应答（停止）   :CM# -> "0"（sync）
  :GVP# -> "Mock LX200#"（P1，Stellarium 不发）
  :XG# -> 状态行（自定义） :XW<unix># / :XC<lat>,<lon># -> "OK#"（自定义）
  :GA#/:GZ# -> 高度/方位（P1，Stellarium 23.4 不查）
连接为 TCP，方便本机自测（不占用 COM 口、也绝不碰 COM6/COM7）。

用法：
  python tools/mock_lx200_mount.py --port 5555
  python tools/stellarium_sim.py --tcp 127.0.0.1:5555 status
  python tools/stellarium_sim.py --tcp 127.0.0.1:5555 goto --ra 06:45:09 --dec -16:42:58
"""

import argparse
import datetime
import math
import socket
import socketserver
import sys
import threading
import time

DEG = 0xDF


def fmt_ra(ra_hours):
    total = int(round(ra_hours * 3600.0)) % 86400
    return "%02d:%02d:%02d#" % (total // 3600, (total // 60) % 60, total % 60)


def fmt_dec(dec_deg):
    arc = int(round(dec_deg * 3600.0))
    sign = "+" if arc >= 0 else "-"
    a = abs(arc)
    return ("%s%02d" % (sign, a // 3600)).encode() + bytes([DEG]) + \
           ("%02d:%02d#" % ((a // 60) % 60, a % 60)).encode()


class MountState:
    """极简的假望远镜：赤道坐标 + 匀速 goto。"""

    def __init__(self, ra_hours=6.0, dec_deg=-16.0):
        self.ra = ra_hours
        self.dec = dec_deg
        self.target_ra = ra_hours
        self.target_dec = dec_deg
        self.slewing = False
        self.rate_deg_per_s = 15.0
        self.lock = threading.Lock()
        self.clock = datetime.datetime.now(datetime.timezone.utc)
        self.lat = 39.9
        self.lon = 116.4
        self._stop = False
        threading.Thread(target=self._loop, daemon=True).start()

    def _angdiff(self, a, b):
        return (a - b + 12.0) % 24.0 - 12.0

    def _loop(self):
        last = time.time()
        while not self._stop:
            time.sleep(0.02)
            now = time.time()
            dt = now - last
            last = now
            with self.lock:
                if not self.slewing:
                    continue
                dra = self._angdiff(self.target_ra, self.ra) * 15.0  # deg
                ddec = self.target_dec - self.dec
                dist = math.hypot(dra, ddec)
                step = self.rate_deg_per_s * dt
                if dist <= step or dist < 1e-6:
                    self.ra = self.target_ra
                    self.dec = self.target_dec
                    self.slewing = False
                else:
                    self.ra = (self.ra + dra / dist * step / 15.0) % 24.0
                    self.dec = self.dec + ddec / dist * step

    def stop(self):
        self._stop = True

    def altaz(self):
        """用当前时间/站点把 RA/Dec 粗算成 alt/az（仅用于 :GA#/:GZ# 应答，足够测试）。"""
        lst = (self.clock.hour + self.clock.minute / 60.0 + self.clock.second / 3600.0
               + self.lon / 15.0) * 15.0
        ha = math.radians(lst - self.ra * 15.0)
        dec = math.radians(self.dec)
        lat = math.radians(self.lat)
        alt = math.asin(math.sin(dec) * math.sin(lat) + math.cos(dec) * math.cos(lat) * math.cos(ha))
        az = math.atan2(-math.sin(ha), math.tan(dec) * math.cos(lat) - math.sin(lat) * math.cos(ha))
        return math.degrees(alt), (math.degrees(az) + 360.0) % 360.0


class Handler(socketserver.BaseRequestHandler):
    state = None

    def handle(self):
        buf = bytearray()
        peer = "%s:%d" % self.client_address
        print("[mock] client connected %s" % peer, file=sys.stderr)
        self.request.settimeout(0.2)
        try:
            while True:
                try:
                    chunk = self.request.recv(256)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf.extend(chunk)
                self.process(buf)
        except Exception as e:
            print("[mock] conn error: %s" % e, file=sys.stderr)
        finally:
            print("[mock] client disconnected %s" % peer, file=sys.stderr)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.request.sendall(data)

    def process(self, buf):
        while True:
            start = buf.find(b":")
            if start < 0:
                # 只有 '#'/垃圾 -> 全部丢弃
                del buf[:]
                return
            end = buf.find(b"#", start)
            if end < 0:
                # 未收完；丢掉 ':' 之前的垃圾
                del buf[:start]
                return
            cmd = bytes(buf[start:end + 1])  # 含结尾 '#'
            del buf[:end + 1]
            try:
                self.dispatch(cmd)
            except Exception as e:
                print("[mock] dispatch error on %r: %s" % (cmd, e), file=sys.stderr)
                self.send(b"0#")

    def dispatch(self, cmd):
        st = self.state
        body = cmd  # e.g. b':GR#'
        if body.startswith(b":GR"):
            with st.lock:
                self.send(fmt_ra(st.ra))
        elif body.startswith(b":GD"):
            with st.lock:
                self.send(fmt_dec(st.dec))
        elif body.startswith(b":Sr"):
            ra = body[3:-1].decode("latin-1")  # HH:MM:SS
            h, m, s = [float(x) for x in ra.split(":")]
            with st.lock:
                st.target_ra = (h + m / 60.0 + s / 3600.0) % 24.0
            self.send(b"1")
        elif body.startswith(b":Sd"):
            raw = body[3:-1]
            sign = -1.0 if raw[:1] == b"-" else 1.0
            if raw[:1] in (b"+", b"-"):
                raw = raw[1:]
            raw = raw.replace(b"*", bytes([DEG])).replace(b"\xc2\xb0", bytes([DEG]))
            dd, rest = raw.split(bytes([DEG]))
            mm, ss = rest.split(b":")
            dec = sign * (float(dd) + float(mm) / 60.0 + float(ss) / 3600.0)
            with st.lock:
                st.target_dec = dec
            self.send(b"1")
        elif body.startswith(b":MS"):
            with st.lock:
                st.target_ra = st.target_ra  # 已在 Sr 设置
                st.slewing = True
            self.send(b"0")
        elif body.startswith(b":Q"):
            with st.lock:
                st.slewing = False
                st.target_ra = st.ra
                st.target_dec = st.dec
            # needsNoAnswer：不回包
        elif body.startswith(b":CM"):
            self.send(b"0")
        elif body.startswith(b":GA"):
            alt, az = st.altaz()
            self.send(b"%+03d%c%02d#" % (int(alt), DEG, int(round((alt - int(alt)) * 60)) % 60))
        elif body.startswith(b":GZ"):
            alt, az = st.altaz()
            self.send(b"%03d%c%02d#" % (int(az), DEG, int(round((az - int(az)) * 60)) % 60))
        elif body.startswith(b":GVP"):
            self.send(b"Mock LX200 (Fysetc AltAz)#")
        elif body.startswith(b":XG"):
            with st.lock:
                if st.slewing:
                    self.send(("SLEWING,RA=%.5f,DEC=%.5f#" % (st.ra, st.dec)).encode())
                else:
                    alt, az = st.altaz()
                    self.send(("IDLE,AZ=%.2f,ALT=%.2f,RA=%.5f,DEC=%.5f#" % (az, alt, st.ra, st.dec)).encode())
        elif body.startswith(b":XW"):
            try:
                st.clock = datetime.datetime.fromtimestamp(int(body[3:-1]), datetime.timezone.utc)
            except ValueError:
                pass
            self.send(b"OK#")
        elif body.startswith(b":XC"):
            try:
                lat, lon = body[3:-1].split(b",")
                st.lat, st.lon = float(lat), float(lon)
            except ValueError:
                pass
            self.send(b"OK#")
        elif body.startswith(b":U"):
            pass  # 保持长格式，不回包
        else:
            print("[mock] unknown command %r (ignored)" % cmd, file=sys.stderr)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    p = argparse.ArgumentParser(description="模拟 Fysetc AltAz 固件的 LX200 应答（TCP，供 stellarium_sim.py 自测）")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5555)
    args = p.parse_args()
    Handler.state = MountState()
    srv = Server((args.host, args.port), Handler)
    print("[mock] listening on %s:%d (Ctrl+C to stop)" % (args.host, args.port), file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        Handler.state.stop()
        srv.server_close()


if __name__ == "__main__":
    main()
