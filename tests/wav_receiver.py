import array
from contextlib import closing
import math
import os
from pathlib import Path
import re
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import wave
from urllib.error import HTTPError
import urllib.parse
from urllib.request import Request, urlopen
from process_control import popen, stop

tools, server, recorded = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    recording = root / "20m recording.wav"
    with wave.open(recorded, "rb") as audio:
        pcm = array.array("h", audio.readframes(audio.getnframes()))
    if sys.byteorder != "little":
        pcm.byteswap()
    iq = array.array("h")
    # Shift the fixture to a recording center distinct from the inferred dial + 1500.
    capture_rate = 25000
    for sample in range(len(pcm) * capture_rate // 12000):
        value = pcm[sample * 12000 // capture_rate]
        phase = -math.tau * 4500 * (sample % capture_rate) / capture_rate
        iq.extend((round(value * math.cos(phase)), round(value * math.sin(phase))))
    if sys.byteorder != "little":
        iq.byteswap()
    with wave.open(str(recording), "wb") as out:
        out.setparams((2, 2, capture_rate, 0, "NONE", ""))
        out.writeframes(iq.tobytes())

    # A single slot from stdin completes after flushing the decoder filter.
    stdin_iq = array.array("f")
    for sample, value in enumerate(pcm):
        phase = -math.tau * (sample % 8) / 8
        stdin_iq.extend((value / 32768 * math.cos(phase), value / 32768 * math.sin(phase)))
    stdin_db = root / "stdin.sqlite"
    subprocess.run([tools, "--receive", "FT8", "-", "14074000", "stdin", stdin_db, "0"],
                   input=stdin_iq.tobytes(), check=True, timeout=10, capture_output=True)
    with closing(sqlite3.connect(stdin_db)) as db, db:
        assert db.execute("SELECT count(*) FROM spots WHERE utc_ms = 0").fetchone()[0] > 0
        assert db.execute("SELECT count(*) FROM live_receptions").fetchone() == (0,)

    # Native receive knows its slice rate so it can reject an incompatible WAV.
    bad = subprocess.run([tools, "--receive", "FT8", f"wav:{recorded}",
                          "14074000", "bad", root / "bad.sqlite"],
                         capture_output=True, text=True, timeout=10)
    assert bad.returncode and "expected a stereo WAV" in bad.stderr
    bad = subprocess.run([tools, "--receive", "CW", f"wav:{recording}",
                          "14074000", "bad", root / "bad.sqlite"],
                         capture_output=True, text=True, timeout=10)
    assert bad.returncode and "does not match IQ WAV rate" in bad.stderr

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    config = root / "station.toml"
    config.write_text(f'port = {port}\n')
    proc = popen([server, config])
    try:
        base = f"http://127.0.0.1:{port}"
        endpoint = base + "/wizard"

        def add_receiver(name, path, repeat):
            for _ in range(100):
                try:
                    with urlopen(base + "/" if name == "recording" else endpoint, timeout=1) as response:
                        assert response.geturl().endswith("/wizard")
                        html = response.read().decode()
                    break
                except OSError:
                    if proc.poll() is not None:
                        raise
                    time.sleep(.05)
            else:
                raise AssertionError("setup wizard did not start")
            data = {field: re.search(fr'name="{field}" value="([^"]+)"', html)[1]
                    for field in ("csrf", "revision")}
            assert 'name="repeat" checked' in html
            # Express one recording relative to the server account's home
            # and the other relative to the station TOML.
            device = path.name
            if name == "recording" and path.drive == Path.home().drive:
                device = "~/" + os.path.relpath(path, Path.home())
            data.update(callsign="K1TST", email="", name="", grid="FN42", receiver=name, device=f"wav:{device}", rate=capture_rate,
                        dial_hz=14074000, center_hz=14078500, mode="FT8", utc_ms=45000)
            if repeat:
                data["repeat"] = "on"
            if name == "recording":
                denied = Request(endpoint, data=urllib.parse.urlencode(dict(data, csrf="")).encode())
                try:
                    urlopen(denied, timeout=2)
                    raise AssertionError("accepted a form without its token")
                except HTTPError as error:
                    assert error.code == 403
            if name == "recording":
                before = config.read_text()
                invalid = dict(data, dial_hz=0)
                try:
                    urlopen(Request(endpoint, data=urllib.parse.urlencode(invalid).encode()), timeout=3)
                    raise AssertionError("invalid tuning was accepted")
                except HTTPError as error:
                    assert error.code == 400
                    returned = error.read().decode()
                    assert "invalid dial_hz" in returned
                    assert 'name="dial_hz" value="0"' in returned
                    assert 'name="receiver" value="recording"' in returned
                    assert 'data-initial-step="2"' in returned
                    assert 'name="center_hz" value="14078500"' in returned
                assert config.read_text() == before
            with urlopen(Request(endpoint, data=urllib.parse.urlencode(data).encode()),
                         timeout=5) as response:
                assert response.status == 200

        add_receiver("recording", recording, False)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with closing(sqlite3.connect(root / "station.sqlite")) as db, db:
                    status = db.execute(
                        "SELECT status FROM workers WHERE id='recording'").fetchone()
                    if status == ("WAV playback complete",):
                        spots = db.execute(
                            "SELECT receiver, utc_ms, callsign FROM spots WHERE callsign='R7IW'"
                        ).fetchall()
                        assert spots == [("recording", 45000, "R7IW")], spots
                        assert db.execute("SELECT count(*) FROM live_receptions").fetchone() == (0,)
                        break
            except sqlite3.OperationalError:
                pass
            time.sleep(.05)
        else:
            raise AssertionError("WAV receiver did not complete playback")
        loop = root / "loop.wav"
        with wave.open(str(loop), "wb") as out:
            out.setparams((2, 2, capture_rate, 0, "NONE", ""))
            out.writeframes(iq[:4800].tobytes())
        add_receiver("loop", loop, True)
        assert "repeat = true" in config.read_text()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and proc.poll() is None:
            with closing(sqlite3.connect(root / "station.sqlite")) as db, db:
                frames = db.execute(
                    "SELECT count(*) FROM spectra WHERE receiver='loop'").fetchone()[0]
                if frames >= 2:
                    assert db.execute(
                        "SELECT status FROM workers WHERE id='loop'").fetchone() == ("Worker launched",)
                    break
            time.sleep(.05)
        else:
            raise AssertionError("short WAV did not produce data after wrapping")
    finally:
        stop(proc)
