from contextlib import closing
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

exe, generator, recording = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    db = Path(tmp) / "spots.sqlite"
    for mode in ("FT4", "FT8"):
        audio = Path(tmp) / (mode + ".wav")
        subprocess.run([generator, "CQ K1ABC FN42", audio, "1500", "-" + mode.lower()],
                       check=True, capture_output=True)
        command = [exe, "--decode", mode, audio, "14074000", "15000", "test", db]
        subprocess.run(command, check=True)
        subprocess.run(command, check=True)
        with closing(sqlite3.connect(db)) as con, con:
            rows = con.execute("SELECT frequency_hz,callsign,grid FROM spots WHERE mode=?",
                               (mode,)).fetchall()
        assert len(rows) == 1 and abs(rows[0][0] - 14075500) < 10, rows
        assert rows[0][1:] == ("K1ABC", "FN42"), rows
    result = subprocess.check_output([exe, "--decode", "FT8", recording, "14074000",
                                      "30000", "test", db], text=True)
    assert "CQ R7IW LN35" in result, result

    # Translate the recorded USB audio into a complex slice centered 1500 Hz
    # above dial. Include a filter tail so the slot's last samples are emitted.
    import array
    import math
    import wave
    with wave.open(recording, "rb") as stream:
        pcm = array.array("h", stream.readframes(stream.getnframes()))
    if sys.byteorder != "little":
        pcm.byteswap()
    iq = array.array("f")
    for i, sample in enumerate(list(pcm) + [0] * 128):
        phase = -math.tau * (i % 8) / 8
        iq.extend((sample * math.cos(phase) / 32768, sample * math.sin(phase) / 32768))
    result = subprocess.run([exe, "--receive", "FT8", "-", "14074000", "stream", db,
                             "45000"], input=iq.tobytes(), capture_output=True, check=True)
    assert b"CQ R7IW LN35" in result.stdout
