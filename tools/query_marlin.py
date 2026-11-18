"""Query Marlin on a serial port with a few read-only info commands."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
CMDS = [b"M115\n", b"M503\n", b"M119\n", b"M114\n", b"M43\n"]


def read_until_quiet(sp, timeout=1.2):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        n = sp.in_waiting
        if n:
            buf += sp.read(n)
            end = time.time() + 0.35
        else:
            time.sleep(0.02)
    return buf


def main():
    sp = serial.Serial(PORT, BAUD, timeout=0.2, write_timeout=0.5)
    try:
        sp.reset_input_buffer()
        print("passive:", repr(read_until_quiet(sp, 0.5)[:200]))
        for cmd in CMDS:
            sp.reset_input_buffer()
            sp.write(cmd)
            sp.flush()
            resp = read_until_quiet(sp)
            print(f"\n--- {cmd!r} ({len(resp)} bytes) ---")
            print(resp.decode("utf-8", "replace")[:4000])
    finally:
        sp.close()


if __name__ == "__main__":
    main()
