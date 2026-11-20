"""One-shot diagnostics helper: query the mount's :XG# / :XJ# state over COM7."""
import sys
import time

import serial

xgs = []
xjs = []


def main(port="COM7"):
    sp = serial.Serial(port, 9600, timeout=0.4)
    try:
        t0 = time.time()
        while time.time() - t0 < 10:
            sp.reset_input_buffer()
            sp.write(b"#:GVP#")
            sp.flush()
            time.sleep(0.15)
            if b"FYSETC" in sp.read(100):
                break

        def cmd(cmd, wait=0.6):
            sp.reset_input_buffer()
            sp.write(cmd)
            sp.flush()
            time.sleep(wait)
            return sp.read(4000)

        xg = cmd(b"#:XG#")
        xj = cmd(b"#:XJ#", 0.8)
        print("XG:", xg.decode("latin-1").strip())
        print("XJ:", ascii(xj))
        return xg, xj
    finally:
        sp.close()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "COM7")
