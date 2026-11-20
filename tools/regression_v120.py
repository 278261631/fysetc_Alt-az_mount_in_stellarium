"""Regression test for the v1.2.0 pointing fixes (run after flashing).

Checks:
  1. :XW/:XC clear the sync offset  -> :XS# SYN=+0.0000/+0.00
  2. a small :CM# sync (simulating a normal alignment) must NOT shift where the
     mount reports after a goto: goto target -> :GR#/:GD# converge to the target.
  3. a target below the horizon is rejected (:MS# -> '1', no motion).
  4. setting the clock again (:XW) clears the sync offset.

Run:  python tools\\regression_v120.py
"""
import datetime
import re
import time

import serial

PORT = "COM7"
BAUD = 9600
DEG = 0xDF


def read(sp, timeout=0.8):
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


def cmd(sp, c, wait=0.6):
    sp.reset_input_buffer()
    sp.write(c)
    sp.flush()
    time.sleep(wait)
    return read(sp, 0.8)


def hms(b):
    h, m, s = b.decode("latin-1").rstrip("#").split(":")
    return int(h) * 3600 + int(m) * 60 + int(s)


def dms(b):
    t = b.decode("latin-1").rstrip("#").replace(chr(DEG), "*")
    sign = -1 if t.startswith("-") else 1
    t = t.lstrip("+-")
    d, rest = t.split("*")
    m, s = rest.split(":")
    return sign * (int(d) * 3600 + int(m) * 60 + int(s))


def sec_to_hms(sec):
    sec %= 86400
    return f"{sec // 3600:02d}:{(sec // 60) % 60:02d}:{sec % 60:02d}"


def arcsec_to_dms(a, sep=None):
    sep = chr(DEG) if sep is None else sep
    sign = "-" if a < 0 else "+"
    a = abs(int(round(a)))
    return f"{sign}{a // 3600:02d}{sep}{(a // 60) % 60:02d}:{a % 60:02d}"


def dms_disp(a):
    return arcsec_to_dms(a, sep="*")


def xs(sp):
    return cmd(sp, b"#:XS#").decode("latin-1").strip()


def main():
    ok = True
    sp = serial.Serial(PORT, BAUD, timeout=0.4)
    try:
        t0 = time.time()
        while time.time() - t0 < 10:
            sp.reset_input_buffer()
            sp.write(b"#:GVP#")
            sp.flush()
            time.sleep(0.15)
            if b"FYSETC" in sp.read(200):
                break
        print("GVN:", cmd(sp, b"#:GVN#"))

        now = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
        print(":XW/:XC ->", cmd(sp, f":XW{now}#".encode("latin-1")),
              cmd(sp, f":XC39.9042,116.4074#".encode("latin-1")))
        print("[1] XS after clock/site:", xs(sp))
        r1 = xs(sp)
        c1 = "SYN=+0.0000/+0.00" in r1
        ok = ok and c1
        print("check1", c1, repr(r1))

        # small, realistic sync offset: +1 deg Dec, +30 s RA
        raw_ra = hms(cmd(sp, b"#:GR#"))
        raw_dec = dms(cmd(sp, b"#:GD#"))
        print("current pointing: RA", sec_to_hms(raw_ra), "Dec", dms_disp(raw_dec))
        print("sync to current+30s/+1deg:",
              cmd(sp, f":Sr{sec_to_hms(raw_ra + 30)}#".encode("latin-1")),
              cmd(sp, f":Sd{arcsec_to_dms(raw_dec + 3600)}#".encode("latin-1")),
              cmd(sp, b"#:CM#"))
        print("[2] XS after sync      :", xs(sp))

        # goto Altair and check reported position converges to the request
        req_ra, req_dec = (19, 50, 47), (8, 52, 6)
        req_ra_s = req_ra[0] * 3600 + req_ra[1] * 60 + req_ra[2]
        req_dec_s = req_dec[0] * 3600 + req_dec[1] * 60 + req_dec[2]
        print("goto Altair:",
              cmd(sp, b"#:Q#", 0.3),
              cmd(sp, f":Sr{req_ra[0]:02d}:{req_ra[1]:02d}:{req_ra[2]:02d}#".encode("latin-1")),
              cmd(sp, f":Sd+{req_dec[0]:02d}{chr(DEG)}{req_dec[1]:02d}:{req_dec[2]:02d}#".encode("latin-1")),
              cmd(sp, b":MS#"))
        time.sleep(9)
        graw = cmd(sp, b"#:GR#")
        draw = cmd(sp, b"#:GD#")
        d_ra = hms(graw) - req_ra_s
        d_dec = dms(draw) - req_dec_s
        print(f"[3] reported RA={graw!r} Dec={ascii(draw)}")
        print(f"    residual dRA={d_ra:+d}s dDec={d_dec:+d}\"")
        print("    XS:", xs(sp))
        c3 = abs(d_ra) <= 8 and abs(d_dec) <= 120
        ok = ok and c3
        print("check3", c3)

        # below-horizon target must be rejected without motion
        before = cmd(sp, b"#:XG#").decode("latin-1")
        azs_before = re.search(r"AZS=(-?\d+)/", before).group(1)
        ack = (cmd(sp, b":Sr06:00:00#")
               + cmd(sp, b":Sd-80" + bytes([DEG]) + b"00:00#")
               + cmd(sp, b":MS#"))
        time.sleep(1.0)
        after = cmd(sp, b"#:XG#").decode("latin-1")
        azs_after = re.search(r"AZS=(-?\d+)/", after).group(1)
        print("[4] below-horizon ack:", ascii(ack), "AZS", azs_before, "->", azs_after)
        c4 = ack.endswith(b"1#") and azs_before == azs_after
        ok = ok and c4
        print("check4", c4)

        # changing the clock clears the sync offset
        now = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
        cmd(sp, f":XW{now}#".encode("latin-1"))
        print("[5] XS after :XW again:", xs(sp))
        r5 = xs(sp)
        c5 = "SYN=+0.0000/+0.00" in r5
        ok = ok and c5
        print("check5", c5, repr(r5))

        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        sp.close()


if __name__ == "__main__":
    raise SystemExit(main())
