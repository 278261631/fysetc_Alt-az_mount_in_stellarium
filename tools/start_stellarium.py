"""Start Stellarium with a correctly synced mount clock/site.

The FYSETC E4 has no battery-backed RTC and Stellarium 23.4 never sends time or
location over its LX200 link, so the mount clock goes stale as soon as the board
is power-cycled (and drifts ~1 minute per hour while powered).  A stale clock
makes goto targets land in the wrong place (targets can even be clamped to the
horizon), which looks like "the mount stops at the wrong coordinates".

This launcher fixes that in one step:
  1. opens COM7, sets the mount UTC to the PC clock and the site to your
     latitude/longitude (persisted in NVS, survives the DTR reset),
  2. then starts Stellarium (config already has the auto-connecting telescope).

Usage (double-click tools\\start_stellarium.bat for the default site, or):
    python tools\\start_stellarium.py
    python tools\\start_stellarium.py --lat 39.9042 --lon 116.4074
    python tools\\start_stellarium.py --stellarium "E:\\Stellarium\\stellarium.exe"
    python tools\\start_stellarium.py --startup-script tools\\startup.ssc

If Stellarium is already running (COM7 busy) the clock step is skipped and the
existing Stellarium window is left alone.
"""
import argparse
import datetime as dt
import os
import subprocess
import sys
import time

import serial

DEFAULT_PORT = "COM7"
DEFAULT_STELLARIUM = r"E:\Stellarium\stellarium.exe"
DEFAULT_LAT = 39.9042   # Beijing
DEFAULT_LON = 116.4074
BAUD = 9600


def mount_ready(sp, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        sp.reset_input_buffer()
        sp.write(b"#:GVP#")
        sp.flush()
        time.sleep(0.15)
        if b"FYSETC" in sp.read(200):
            return True
    return False


def send(sp, cmd, wait=0.5):
    sp.reset_input_buffer()
    sp.write(cmd)
    sp.flush()
    time.sleep(wait)
    return sp.read(2000)


def sync_mount(port, lat, lon, when=None):
    try:
        sp = serial.Serial(port, BAUD, timeout=0.4)
    except Exception as exc:  # noqa: BLE001
        print(f"[mount] cannot open {port}: {exc}")
        print("[mount] (Stellarium already has the port? clock sync skipped)")
        return False
    try:
        if not mount_ready(sp):
            print("[mount] no LX200 answer on", port, "- clock sync skipped")
            return False
        unix = int(when.timestamp()) if when else int(time.time())
        a = send(sp, f":XW{unix}#".encode())
        b = send(sp, f":XC{lat:.4f},{lon:.4f}#".encode())
        print(f"[mount] :XW{unix}# -> {a!r}   :XC{lat:.4f},{lon:.4f}# -> {b!r}")
        st = send(sp, b"#:XG#", 0.6).decode("latin-1", "replace").strip()
        print("[mount]", st)
        return a.startswith(b"1")
    finally:
        sp.close()


def main():
    ap = argparse.ArgumentParser(description="Sync mount clock/site then start Stellarium")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--lat", type=float, default=DEFAULT_LAT)
    ap.add_argument("--lon", type=float, default=DEFAULT_LON,
                    help="east positive [deg]")
    ap.add_argument("--stellarium", default=DEFAULT_STELLARIUM)
    ap.add_argument("--startup-script", default=None,
                    help="optional Stellarium .ssc script (e.g. tools/startup.ssc)")
    ap.add_argument("--date", default=None,
                    help="fixed UTC time instead of now (ISO 8601)")
    ap.add_argument("--no-launch", action="store_true", help="only sync clock/site")
    args = ap.parse_args()

    when = None
    if args.date:
        when = dt.datetime.fromisoformat(args.date.replace("Z", "+00:00"))
        if when.tzinfo is None:
            when = when.replace(tzinfo=dt.timezone.utc)

    sync_mount(args.port, args.lat, args.lon, when)
    if args.no_launch:
        return 0

    if not os.path.exists(args.stellarium):
        print("[stellarium] not found:", args.stellarium)
        return 2
    cmd = [args.stellarium]
    if args.startup_script:
        cmd += ["--startup-script", os.path.abspath(args.startup_script)]
    print("[stellarium] starting", " ".join(cmd))
    subprocess.Popen(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
