"""Scan a Windows PE file for ASCII protocol/action strings of interest."""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\Stellarium\stellarium.exe"
data = open(path, "rb").read()

patterns = [
    rb"actionSlew[^\x00]{0,40}",
    rb"actionSync[^\x00]{0,40}",
    rb"actionAbort[^\x00]{0,40}",
    rb"actionTelescope[^\x00]{0,40}",
    rb"TelescopeServer[^\x00]{0,40}",
    rb"telescoped[^\x00]{0,30}",
    rb"LX200[^\x00]{0,40}",
    rb"SerialPort[^\x00]{0,30}",
    rb"TcpConnection[^\x00]{0,30}",
    rb"telescope_control[^\x00]{0,40}",
]
for pat in patterns:
    hits = sorted(set(m.group(0) for m in re.finditer(pat, data)))
    print(f"--- {pat!r} ({len(hits)}) ---")
    for h in hits[:30]:
        print("   ", ascii(h[:60]))
