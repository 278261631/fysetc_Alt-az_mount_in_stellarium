"""Probe an unknown serial device: passive banner read, then harmless ID queries.

No motion commands are sent. Safe for motor drivers that may be attached.
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUDS = [115200, 9600, 57600, 250000, 38400]


def read_for(sp, seconds):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        n = sp.in_waiting
        if n:
            buf += sp.read(n)
            end = time.time() + 0.4
        else:
            time.sleep(0.02)
    return buf


def main():
    for baud in BAUDS:
        print(f"=== {PORT} @ {baud} ===")
        try:
            sp = serial.Serial(PORT, baud, timeout=0.2, write_timeout=0.5)
        except Exception as exc:  # noqa: BLE001
            print("  open failed:", exc)
            continue
        try:
            sp.reset_input_buffer()
            banner = read_for(sp, 1.0)
            print("  passive:", repr(banner[:400]))
            for cmd, label in ((b":GVP#", "LX200/OnStep product"), (b"M115\n", "Marlin info")):
                sp.reset_input_buffer()
                sp.write(cmd)
                sp.flush()
                time.sleep(0.6)
                resp = read_for(sp, 1.0)
                if resp:
                    print(f"  {label} ->", repr(resp[:400]))
                else:
                    print(f"  {label} -> (no response)")
        finally:
            sp.close()


if __name__ == "__main__":
    main()
