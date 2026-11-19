import array
from contextlib import closing
import math
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import wave

# Upstream's reference stores conjugated IQ at 375 Hz. Place it on a
# 1500 Hz audio carrier to exercise the same path as a USB recording.
raw = Path(sys.argv[2]).read_bytes()
samples = list(struct.iter_unpack("<ff", raw))
gain = 20000 / max(math.hypot(i, q) for i, q in samples)
carrier = [(math.cos(math.tau*i/8), math.sin(math.tau*i/8)) for i in range(8)]
with tempfile.TemporaryDirectory() as directory:
    audio = Path(directory) / "wspr.wav"
    db = Path(directory) / "spots.db"
    with wave.open(str(audio), "wb") as stream:
        stream.setparams((1, 2, 12000, 0, "NONE", ""))
        pcm = array.array("h")
        for i, q in samples:
            cycle = [round(gain*(i*c + q*s)) for c, s in carrier]
            pcm.extend(cycle * 4)
        if sys.byteorder != "little":
            pcm.byteswap()
        stream.writeframes(pcm.tobytes())
    subprocess.run([sys.argv[1], "--decode", "WSPR", audio, "144489000", "120000",
                    "test", db], check=True)
    with closing(sqlite3.connect(db)) as con, con:
        rows = con.execute("SELECT frequency_hz,callsign,grid FROM spots").fetchall()
    assert any(abs(hz-144490550) <= 2 and call=="K1JT" and grid=="FN20"
               for hz, call, grid in rows), rows
