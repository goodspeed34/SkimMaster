# Drive the SoapySDR receiver module through the fake module in
# SOAPY_SDR_PLUGIN_PATH: a capture, synthesizer rounding within the center
# tolerance, and a tuner that lands too far off to feed the slices.
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from morse_iq import keyed

with tempfile.TemporaryDirectory() as tmp:
    iq = Path(tmp) / "cw.f32"
    iq.write_bytes(keyed("CQ CQ DE K1ABC K1ABC TEST " * 3, 700).tobytes())

    def capture(name, center, step=1):
        db = Path(tmp) / f"{name}.sqlite"
        run = subprocess.run(
            [sys.argv[1], "--receive", "CW", f"driver=fake,iq={iq},step={step}",
             str(center), "test", db, "0"],
            capture_output=True, text=True, timeout=120)
        assert run.returncode == 0, run.stderr
        rows = []
        if db.exists():
            with closing(sqlite3.connect(db)) as con, con:
                rows = con.execute("SELECT callsign, frequency_hz FROM spots").fetchall()
        return run.stderr, rows

    stderr, rows = capture("exact", 14020000)
    assert "Device reports center 14020000 Hz rate 48000 Hz gain 0 dB" in stderr, stderr
    assert rows and all(call == "K1ABC" and abs(hz - 14020700) < 12
                        for call, hz in rows), rows

    stderr, rows = capture("rounded", 14020030, step=50)
    assert "center 14020050 Hz" in stderr and "holding" not in stderr, stderr
    assert rows and all(call == "K1ABC" for call, _ in rows), rows

    stderr, rows = capture("off", 14020500, step=1000)
    assert "holding IQ until the center returns to 14020500 Hz" in stderr, stderr
    assert not rows, rows
