"""Set / read the Fysetc AltAz mount clock + site over the LX200 serial link.

Stellarium 23.4 (direct LX200) never sends time or location to the mount, so the
firmware keeps its own UTC / site.  Run this helper *before* starting Stellarium
(or after closing it) to sync the mount with the sky the way Stellarium shows it.

Examples
--------
    python tools/set_scope.py --status
    python tools/set_scope.py --lat 39.9042 --lon 116.4074 --date 2026-11-18T14:00:00
    python tools/set_scope.py --lat 39.9042 --lon 116.4074          # use current UTC

The LX200 link of Stellarium 23.4 is fixed at 9600 8N1.
"""
import argparse
import datetime as dt
import sys
import time

import serial

DEFAULT_PORT = "COM7"
BAUD = 9600
PRODUCT_MARKERS = (b"Fysetc", b"AltAz", b"FYSETC")


def open_port(port):
    sp = serial.Serial(port, BAUD, timeout=0.3, write_timeout=0.5)
    # Avoid a spurious reset from the CH340 auto-reset circuit where possible.
    try:
        sp.dtr = False
        sp.rts = False
    except Exception:  # noqa: BLE001
        pass
    sp.reset_input_buffer()
    return sp


def read_answer(sp, timeout=1.5):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        n = sp.in_waiting
        if n:
            buf += sp.read(n)
            end = time.time() + 0.25
        else:
            time.sleep(0.02)
    return buf


def send(sp, cmd, wait=0.4):
    sp.write(cmd.encode("latin-1"))
    sp.flush()
    return read_answer(sp, wait)


def wait_ready(sp, timeout=20.0):
    """Probe :GVP# until the firmware answers (covers a DTR reset after open)."""
    end = time.time() + timeout
    while time.time() < end:
        ans = send(sp, ":GVP#", 0.8)
        if ans and any(m in ans for m in PRODUCT_MARKERS):
            return ans
        time.sleep(0.4)
    return None


def status(sp):
    return send(sp, ":XG#", 0.8)


def main():
    ap = argparse.ArgumentParser(description="Fysetc AltAz LX200 clock/site helper")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--lat", type=float, help="site latitude, north positive [deg]")
    ap.add_argument("--lon", type=float, help="site longitude, east positive [deg]")
    ap.add_argument("--date", help="UTC time, e.g. 2026-11-18T14:00:00; default now")
    ap.add_argument("--status", action="store_true", help="only print mount status")
    args = ap.parse_args()

    try:
        sp = open_port(args.port)
    except Exception as exc:  # noqa: BLE001
        print(f"cannot open {args.port}: {exc}")
        return 2

    try:
        ready = wait_ready(sp)
        if ready is None:
            print(f"no answer from mount on {args.port} @ {BAUD} (is Stellarium holding the port?)")
            return 3
        print("product:", ready.decode("latin-1", "replace").strip())
        print("status :", status(sp).decode("latin-1", "replace").strip())
        if args.status:
            return 0

        if args.date:
            when = dt.datetime.fromisoformat(args.date)
            if when.tzinfo is None:
                when = when.replace(tzinfo=dt.timezone.utc)
            unix = int(when.timestamp())
        else:
            unix = int(time.time())
        print("set UTC:", dt.datetime.fromtimestamp(unix, dt.timezone.utc).isoformat())
        print("  :XW# ->", send(sp, f":XW{unix}#", 0.6).decode("latin-1", "replace").strip())

        if args.lat is not None and args.lon is not None:
            print(f"set site: lat={args.lat} lon={args.lon}")
            ans = send(sp, f":XC{args.lat:.4f},{args.lon:.4f}#", 0.6)
            print("  :XC# ->", ans.decode("latin-1", "replace").strip())

        print("status :", status(sp).decode("latin-1", "replace").strip())
        return 0
    finally:
        sp.close()


if __name__ == "__main__":
    sys.exit(main())
