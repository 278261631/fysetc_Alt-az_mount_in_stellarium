"""Chunked, resumable flash backup for the FYSETC E4 on COM7.

The CH340 link drops bytes during long single-shot esptool reads, so the 16 MB
image is read in 1 MB chunks (each chunk starts its own esptool session, with
retries at a lower baud rate). Chunks are appended to a raw .bin file and a
progress JSON lets the script resume after an interruption.
"""
import json
import os
import subprocess
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
OUT = sys.argv[2] if len(sys.argv) > 2 else "backup/marlin_e4_original_16MB.bin"
FLASH_SIZE = 0x1000000
CHUNK = 0x100000
BAUDS = [460800, 230400, 115200]

STATE = OUT + ".progress.json"


def run_esptool(offset, size, path, baud):
    cmd = [
        sys.executable, "-m", "esptool", "-p", PORT, "-b", str(baud),
        "read-flash", hex(offset), hex(size), path,
    ]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    if os.path.exists(OUT):
        have = os.path.getsize(OUT)
        done = have // CHUNK
        print(f"resume: {have} bytes already present ({done} chunks)")
    else:
        open(OUT, "wb").close()
        done = 0

    total = FLASH_SIZE // CHUNK
    tmp = OUT + ".chunk"
    with open(OUT, "ab") as fout:
        for idx in range(done, total):
            offset = idx * CHUNK
            ok = False
            for baud in BAUDS:
                if os.path.exists(tmp):
                    os.remove(tmp)
                t0 = time.time()
                res = run_esptool(offset, CHUNK, tmp, baud)
                if res.returncode == 0 and os.path.exists(tmp) and os.path.getsize(tmp) == CHUNK:
                    with open(tmp, "rb") as f:
                        fout.write(f.read())
                    fout.flush()
                    ok = True
                    print(f"chunk {idx+1}/{total} @0x{offset:06X} baud {baud} ok "
                          f"({time.time()-t0:.0f}s)")
                    break
                tail = (res.stdout or "")[-160:] + (res.stderr or "")[-160:]
                print(f"chunk {idx+1}/{total} @0x{offset:06X} baud {baud} FAILED: {tail}")
            if not ok:
                print("giving up on chunk, keeping partial image")
                return 1
            with open(STATE, "w") as f:
                json.dump({"chunks_done": idx + 1, "chunk_size": CHUNK}, f)
    if os.path.getsize(OUT) == FLASH_SIZE:
        print(f"DONE: {OUT} ({FLASH_SIZE} bytes)")
        os.remove(STATE) if os.path.exists(STATE) else None
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
