# Capture CW over TCI from an emulated two-receiver server. Receiver 0
# carries a decoy; receiver 1 carries the wanted signal, then is retuned
# from the radio's side, which must hold IQ until it returns.
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading

from morse_iq import keyed
from tci_emulator import TciServer

def chunks(samples, size=4096):
    for start in range(0, len(samples), size):
        yield samples[start:start + size]


server = TciServer()
errors = []


def radio():
    try:
        server.accept()
        server.wait_for("iq_start:1")
        wanted = keyed("CQ CQ DE K1ABC K1ABC TEST " * 3, 700)
        decoy = keyed("CQ CQ DE W2DCY W2DCY TEST " * 3, 700)
        for ours, other in zip(chunks(wanted), chunks(decoy)):
            server.iq(0, other)
            server.iq(1, ours)
        # Another client retunes receiver 1; its IQ is no longer on plan.
        server.text("dds:1,7050000;")
        for block in chunks(keyed("CQ CQ DE W9OFF W9OFF TEST " * 3, 700)):
            server.iq(1, block)
        server.text("dds:0,7001000;")
    except Exception as error:  # reported on the main thread
        errors.append(error)
    finally:
        server.close()


thread = threading.Thread(target=radio)
thread.start()
with tempfile.TemporaryDirectory() as tmp:
    db = Path(tmp) / "spots.sqlite"
    run = subprocess.run([sys.argv[1], "--receive", "CW",
                          f"tci:127.0.0.1:{server.port}/1", "14020000", "test",
                          db, "0"], capture_output=True, text=True, timeout=120)
    thread.join(10)
    assert not errors, errors
    assert run.returncode == 0, run.stderr
    sent = server.received
    assert sent[:3] == ["iq_samplerate:48000", "dds:1,14020000", "iq_start:1"], sent
    assert "holding IQ until the center returns to 14020000 Hz" in run.stderr, run.stderr
    with closing(sqlite3.connect(db)) as con, con:
        rows = con.execute("SELECT callsign, frequency_hz FROM spots").fetchall()
    assert rows and all(call == "K1ABC" and abs(hz - 14020700) < 12
                        for call, hz in rows), rows
