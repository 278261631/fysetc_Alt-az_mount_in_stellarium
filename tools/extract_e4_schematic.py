"""Extract TMC2209-related text from the FYSETC E4 schematic PDF."""
import re
from pypdf import PdfReader

reader = PdfReader(r"%TEMP%\e4_sch.pdf".replace("%TEMP%", __import__("os").environ.get("TEMP", ".")))
print("pages:", len(reader.pages))
text = "\n".join((p.extract_text() or "") for p in reader.pages)
print("chars:", len(text))
open(r"%TEMP%\e4_sch.txt".replace("%TEMP%", __import__("os").environ.get("TEMP", ".")), "w", encoding="utf-8").write(text)

for kw in ("MS1", "MS2", "ADDR", "AD0", "AD1", "R_SENSE", "SENSE", "2209", "UART", "0.11", "R11", "R12"):
    hits = [ln.strip() for ln in text.splitlines() if kw.lower() in ln.lower()]
    print(f"--- {kw} ({len(hits)})")
    for h in hits[:12]:
        print("   ", h)
