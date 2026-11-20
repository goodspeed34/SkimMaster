import array
import cmath
from contextlib import closing
import math
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory() as tmp:
    db = Path(tmp) / "spots.sqlite"
    bits = [(1, 6)]
    for code in [14, 23, 4, 15, 27, 23, 31, 3, 25, 14, 8]:
        bits += [(0, 1)] + [(code >> i & 1, 1) for i in range(5)] + [(1, 1.5)]
    mark_hz = -1030

    def modulate(bits):
        iq = array.array("f")
        phase = 0
        for mark, duration in bits:
            step = math.tau * (mark_hz if mark else mark_hz - 170) / 12000
            for i in range(round(duration * 12000/45.45)):
                z = cmath.exp(1j*phase)
                iq.extend((z.real, z.imag))
                phase = (phase + step) % math.tau
        return iq

    iq = modulate(bits)
    subprocess.run([sys.argv[1], "--receive", "RTTY", "-", "14080000", "test", db, "0"],
                   input=iq.tobytes(), check=True)
    with closing(sqlite3.connect(db)) as con, con:
        rows = con.execute("select message,callsign,frequency_hz from spots").fetchall()
        assert len(rows) == 1 and rows[0][:2] == ("CQ K1ABC", "K1ABC"), rows
        assert abs(rows[0][2] - (14080000 + mark_hz)) < 10, rows

    # Live text: hold the stream open after the message, idling on LTRS so
    # the channel stays active, and read the transient live_text table.
    idle = []
    for _ in range(40):
        idle += [(0, 1)] + [(1, 1)] * 5 + [(1, 1.5)]
    live_db = Path(tmp) / "live.sqlite"
    proc = subprocess.Popen([sys.argv[1], "--receive", "RTTY", "-", "14080000",
                             "test", live_db, "0"], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL)
    try:
        proc.stdin.write(modulate(bits + idle).tobytes())
        proc.stdin.flush()
        for attempt in range(200):
            if live_db.exists():
                with closing(sqlite3.connect(live_db)) as con:
                    try:
                        live = con.execute(
                            "select receiver,mode,frequency_hz,text,speed "
                            "from live_text").fetchall()
                    except sqlite3.OperationalError:
                        live = []
                if any("CQ K1ABC" in row[3] for row in live):
                    break
            time.sleep(.05)
        else:
            raise AssertionError(f"live RTTY text not published: {live}")
        row = next(row for row in live if "CQ K1ABC" in row[3])
        assert row[:2] == ("test", "RTTY") and row[4] == 45.45, row
        assert abs(row[2] - (14080000 + mark_hz)) < 10, row
    finally:
        proc.stdin.close()
        assert proc.wait(timeout=30) == 0
    with closing(sqlite3.connect(live_db)) as con:
        assert con.execute("select count(*) from live_text").fetchone()[0] == 0
