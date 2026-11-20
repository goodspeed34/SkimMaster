# Capture CW from an emulated FlexRadio over DAX IQ channel 2. Channel 1
# carries a decoy; the panadapter is then retuned from SmartSDR, which must
# hold IQ until it returns.
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

from flex_emulator import FlexRadio, PAN
from morse_iq import RATE, keyed

BLOCK = 256  # complex samples per datagram
# Faster than real time, but slower than the CW decoder under ThreadSanitizer
# (about 4x), since UDP has no backpressure.
SPEEDUP = 3

radio = FlexRadio()
errors = []


def paced(streams):
    """Send equal-length float arrays on several streams, interleaved."""
    start = time.monotonic()
    for offset in range(0, len(streams[0][1]), 2 * BLOCK):
        for stream, floats in streams:
            radio.iq(stream, floats[offset:offset + 2 * BLOCK])
        ahead = offset / 2 / RATE / SPEEDUP - (time.monotonic() - start)
        if ahead > 0:
            time.sleep(ahead)


def serve():
    try:
        radio.serve_commands()
    except Exception as error:
        errors.append(error)


def stream_iq():
    try:
        if not radio.started.wait(10):
            raise AssertionError("client never configured the DAX IQ stream")
        wanted = keyed("CQ CQ DE K1ABC K1ABC TEST " * 2, 700)
        decoy = keyed("CQ CQ DE W2DCY W2DCY TEST " * 2, 700)
        paced([(radio.streams[2], wanted), (0x20000001, decoy)])
        radio.status(f"display pan {PAN} center=7.050000")
        paced([(radio.streams[2], keyed("CQ CQ DE W9OFF W9OFF TEST", 700))])
        time.sleep(.5)
    except Exception as error:
        errors.append(error)
    finally:
        radio.client.shutdown(2)


threading.Thread(target=serve, daemon=True).start()
streamer = threading.Thread(target=stream_iq)
streamer.start()
with tempfile.TemporaryDirectory() as tmp:
    db = Path(tmp) / "spots.sqlite"
    run = subprocess.run([sys.argv[1], "--receive", "CW",
                          f"flex:127.0.0.1:{radio.port}/2", "14020000", "test",
                          db, "0"], capture_output=True, text=True, timeout=120)
    streamer.join(10)
    radio.close()
    assert not errors and run.returncode == 0, (errors, run.returncode, run.stderr)
    sent = radio.commands
    assert sent[:5] == [
        f"client udpport {radio.udp_port}", "display panafall create x=100 y=100",
        f"display pan set {PAN} center=14.020000 daxiq_channel=2",
        "stream create type=dax_iq daxiq_channel=2",
        "stream set 0x20000002 daxiq_rate=48000"], sent
    assert "holding IQ until the center returns to 14020000 Hz" in run.stderr, run.stderr
    with closing(sqlite3.connect(db)) as con, con:
        rows = con.execute("SELECT callsign, frequency_hz FROM spots").fetchall()
    assert rows and all(call == "K1ABC" and abs(hz - 14020700) < 12
                        for call, hz in rows), rows
