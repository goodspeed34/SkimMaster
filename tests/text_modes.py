import array
import cmath
from contextlib import closing
import math
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as tmp:
    db = Path(tmp) / "spots.sqlite"
    bits = [(1, 6)]
    for code in [14, 23, 4, 15, 27, 23, 31, 3, 25, 14, 8]:
        bits += [(0, 1)] + [(code >> i & 1, 1) for i in range(5)] + [(1, 1.5)]
    iq = array.array("f")
    phase = 0
    mark_hz = -1030
    for mark, duration in bits:
        step = math.tau * (mark_hz if mark else mark_hz - 170) / 12000
        for i in range(round(duration * 12000/45.45)):
            z = cmath.exp(1j*phase)
            iq.extend((z.real, z.imag))
            phase = (phase + step) % math.tau
    subprocess.run([sys.argv[1], "--receive", "RTTY", "-", "14080000", "test", db, "0"],
                   input=iq.tobytes(), check=True)
    with closing(sqlite3.connect(db)) as con, con:
        rows = con.execute("select message,callsign,frequency_hz from spots").fetchall()
        assert len(rows) == 1 and rows[0][:2] == ("CQ K1ABC", "K1ABC"), rows
        assert abs(rows[0][2] - (14080000 + mark_hz)) < 10, rows
