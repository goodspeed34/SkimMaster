"""Build and run two standalone plugins using only the installed SDK."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

prefix = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "standalone"
    shutil.copytree(Path(__file__).with_suffix(""), source)
    build = root / "build"
    subprocess.run(
        ["meson", "setup", build, source, f"-Dsdk_include={prefix / 'include/skimmaster'}"],
        check=True,
    )
    subprocess.run(["meson", "compile", "-C", build], check=True)

    suffix = ".dll" if os.name == "nt" else ".so"
    name = "SkimMasterTools.exe" if os.name == "nt" else "SkimMasterTools"
    tools = root / name
    shutil.copy2(prefix / "bin" / name, tools)
    for family in ("rcvr", "decoder"):
        module = f"lib{family}_external{suffix}"
        shutil.copy2(build / module, root / module)

    request = json.dumps({"device": "fixture:loop", "rate": 12000, "center_hz": 7074000})
    subprocess.run([tools, "--test-receiver", request], check=True, timeout=10)
    audio = root / "slot.wav"
    with wave.open(str(audio), "wb") as slot:
        slot.setparams((1, 2, 12000, 180000, "NONE", ""))
        slot.writeframes(bytes(360000))
    result = subprocess.run(
        [tools, "--decode", "FT8", audio, "7074000", "0", "test", root / "spots.sqlite"],
        check=True, timeout=10, capture_output=True, text=True,
    )
    assert result.stdout.splitlines() == ["7075234 CQ External"], result.stdout
