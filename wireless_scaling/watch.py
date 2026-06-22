"""
watch.py — mentésre újraépíti a házmodellt és frissíti az ocp_vscode nézetet.

Használat (a venv aktiválva, a VS Code-ban a viewer megnyitva):
    python watch.py

Az enclosure.py minden mentésekor lefuttatja azt egy külön processzben (így egy
szintaktikai hiba nem öli meg a figyelőt), ami push-olja a modellt a viewerbe és
újraexportálja az STL/STEP fájlokat. Ctrl+C a leállításhoz.
"""

import subprocess
import sys
import time
from pathlib import Path

TARGET = Path(__file__).parent / "enclosure.py"
POLL_S = 0.5

print(f"Figyelem alatt: {TARGET.name}  — mentésre újrafut. (Ctrl+C = leállítás)")
last_mtime = 0.0
try:
    while True:
        mtime = TARGET.stat().st_mtime
        if mtime != last_mtime:
            last_mtime = mtime
            print("\n=== változás észlelve → modell újraépítése ===")
            subprocess.run([sys.executable, str(TARGET)])
        time.sleep(POLL_S)
except KeyboardInterrupt:
    print("\nFigyelő leállítva.")
