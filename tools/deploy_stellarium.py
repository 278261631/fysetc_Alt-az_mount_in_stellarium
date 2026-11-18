"""Deploy the Fysetc AltAz telescope preset into Stellarium 23.4.

What it does (all reversible, config.ini is backed up first):
  1. backup  %APPDATA%\\Stellarium\\config.ini
  2. set      [plugins_load_at_startup] TelescopeControl = true
  3. install  %APPDATA%\\Stellarium\\modules\\TelescopeControl\\telescopes.json
     (preset: LX200 direct / COM7 / connect_at_startup / equinox JNow)

Usage:
    python tools/deploy_stellarium.py            # deploy
    python tools/deploy_stellarium.py --check    # only report state
    python tools/deploy_stellarium.py --restore  # restore newest config.ini backup
"""
import argparse
import glob
import json
import os
import shutil
import sys
import time

APPDATA = os.environ.get("APPDATA", os.path.expanduser("~/AppData/Roaming"))
STEL = os.path.join(APPDATA, "Stellarium")
CONFIG = os.path.join(STEL, "config.ini")
MOD_DIR = os.path.join(STEL, "modules", "TelescopeControl")
TELESCOPES = os.path.join(MOD_DIR, "telescopes.json")

PRESET = {
    "version": "0.4.1",
    "1": {
        "name": "Fysetc AltAz",
        "connection": "internal",
        "equinox": "JNow",
        "device_model": "Meade LX200 (compatible)",
        "serial_port": "COM7",
        "tcp_port": 10001,
        "delay": 500000,
        "connect_at_startup": True,
    },
}


def read_config():
    with open(CONFIG, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def write_config(lines):
    with open(CONFIG, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def backup_config():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dst = CONFIG + f".bak.{stamp}"
    shutil.copy2(CONFIG, dst)
    return dst


def set_plugin_enabled(lines, enabled=True):
    """Set TelescopeControl = true in [plugins_load_at_startup]; return (lines, changed)."""
    section = None
    changed = False
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
            continue
        if section == "plugins_load_at_startup" and s.lower().startswith("telescopecontrol"):
            key, _, _ = s.partition("=")
            new = f"{key.strip():<42}= {'true' if enabled else 'false'}"
            if lines[i] != new:
                lines[i] = new
                changed = True
            return lines, changed
    # section missing -> append
    lines.append("[plugins_load_at_startup]")
    lines.append(f"{'TelescopeControl':<42}= {'true' if enabled else 'false'}")
    return lines, True


def deploy():
    if not os.path.exists(CONFIG):
        print(f"config.ini not found: {CONFIG} (is Stellarium installed for this user?)")
        return 2
    bak = backup_config()
    print("config backup:", bak)

    lines = read_config()
    lines, changed = set_plugin_enabled(lines)
    if changed:
        write_config(lines)
        print("config.ini: [plugins_load_at_startup] TelescopeControl = true")
    else:
        print("config.ini: TelescopeControl already enabled")

    os.makedirs(MOD_DIR, exist_ok=True)
    old = None
    if os.path.exists(TELESCOPES):
        old = TELESCOPES + f".bak.{time.strftime('%Y%m%d-%H%M%S')}"
        shutil.copy2(TELESCOPES, old)
        print("telescopes.json backup:", old)
    with open(TELESCOPES, "w", encoding="utf-8", newline="\n") as f:
        json.dump(PRESET, f, indent=4, ensure_ascii=False)
        f.write("\n")
    print("installed:", TELESCOPES)
    print("\nDone. Start Stellarium; the mount connects to COM7 automatically on startup.")
    print("Tip: run tools/set_scope.py first to sync the mount clock/site.")
    return 0


def check():
    print("config.ini:", CONFIG, "exists" if os.path.exists(CONFIG) else "MISSING")
    if os.path.exists(CONFIG):
        lines = read_config()
        section = None
        for line in lines:
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                section = s[1:-1]
            elif section == "plugins_load_at_startup" and s.lower().startswith("telescopecontrol"):
                print("  plugin at startup:", s)
    if os.path.exists(TELESCOPES):
        print("telescopes.json:", TELESCOPES)
        with open(TELESCOPES, encoding="utf-8") as f:
            print(" ", f.read().strip().replace("\n", "\n  "))
        try:
            data = json.loads(open(TELESCOPES, encoding="utf-8").read())
            print("  version:", data.get("version"), "slots:", [k for k in data if k != "version"])
        except Exception as exc:  # noqa: BLE001
            print("  JSON ERROR:", exc)
    else:
        print("telescopes.json: MISSING at", TELESCOPES)
    return 0


def restore():
    baks = sorted(glob.glob(CONFIG + ".bak.*"))
    if not baks:
        print("no config.ini backup found")
        return 1
    shutil.copy2(baks[-1], CONFIG)
    print("restored config.ini from", baks[-1])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--restore", action="store_true")
    args = ap.parse_args()
    if args.check:
        return check()
    if args.restore:
        return restore()
    return deploy()


if __name__ == "__main__":
    sys.exit(main())
