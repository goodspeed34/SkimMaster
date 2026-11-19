import io
import os
from contextlib import closing
import zipfile
from urllib.error import HTTPError
from pathlib import Path
import re
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request
from process_control import popen, stop

with tempfile.TemporaryDirectory() as tmp, socket.socket() as radio, socket.socket() as probe:
    server, tools = sys.argv[1:]
    radio.bind(("127.0.0.1",0))
    radio.listen()
    radio.settimeout(8)
    probe.bind(("127.0.0.1",0))
    port=probe.getsockname()[1]
    probe.close()
    config=Path(tmp)/"station.toml"
    base = f"""listen_address = "127.0.0.1"
port = {port}
first_run_wizard = true
"""
    config.write_text(base + 'user = "operator"\npassword = "legacy-secret"\n' + f"""
[[receivers]]
receiver = "west"
device = "rtltcp:127.0.0.1:{radio.getsockname()[1]}"
rate = 48000

[[receivers.attached_slices]]
mode = "RTTY"
center_hz = 14093000

[[receivers.attached_slices]]
mode = "RTTY"
center_hz = 14093001

[[receivers.schedules]]
start_minute = 0
end_minute = 0
priority = 1
center_hz = 14075000

[[receivers.schedules.slices]]
mode = "FT8"
dial_hz = 14074000

[[receivers.schedules.slices]]
mode = "RTTY"
center_hz = 14080000
""")
    assert subprocess.run([sys.argv[1], "--daemon"], capture_output=True).returncode != 0
    proc=popen([server,config])
    try:
        with radio.accept()[0] as client:
            client.settimeout(5)
            client.sendall(b"RTL0"+bytes(8))
            with client.makefile("rb") as stream:
                commands = stream.read(10)
            assert commands==b"\x02"+(48000).to_bytes(4,"big")+b"\x01"+(14075000).to_bytes(4,"big"),commands
            client.sendall(bytes([127,128])*8192)
            url=f"http://127.0.0.1:{port}"
            html = b""
            for i in range(100):
                try:
                    with urllib.request.urlopen(urllib.request.Request(url+"/receivers")) as resp:
                        html=resp.read()
                    if b"west" in html: break
                except OSError:
                    pass
                time.sleep(.05)
            assert b"14074000" in html and b"14080000" in html
            database = Path(tmp) / "station.sqlite"
            gallery = Path(tmp) / "gallery"
            archive = Path(tmp) / "before-restore.zip"
            subprocess.run([tools, "--backup", database, gallery, archive],
                           check=True, capture_output=True)
            with closing(sqlite3.connect(database, timeout=5)) as db, db:
                db.execute(
                    "INSERT INTO system_log VALUES(unixepoch(),'test',?)",
                    ("restore alias lock marker",),
                )

            def assert_restore_blocked(path):
                restore = subprocess.run(
                    [tools, "--restore", archive, path, gallery],
                    capture_output=True, text=True,
                )
                assert restore.returncode and "stop the station before restoring" in restore.stderr
                with closing(sqlite3.connect(database)) as db, db:
                    assert db.execute(
                        "SELECT count(*) FROM system_log WHERE message=?",
                        ("restore alias lock marker",),
                    ).fetchone() == (1,)

            assert_restore_blocked(database)
            alias = Path(tmp) / "station-alias.sqlite"
            try:
                alias.symlink_to(database)
                same_target = alias.samefile(database)
            except (NotImplementedError, OSError) as error:
                if os.name != "nt":
                    raise
                same_target = False
                print(f"Skipping symlink alias check: {error}")
            if same_target:
                assert_restore_blocked(alias)
            elif os.name == "nt":
                print("Skipping symlink alias check: runtime cannot resolve the alias")
            skimmer = urllib.request.urlopen(url+"/rtty-skimmer").read()
            assert b"Slice Center (Hz)" in skimmer and b"14080000" in skimmer
            assert b"14093000" in skimmer and b"14093001" not in skimmer
            assert b"Dial (Hz)" not in skimmer
            html=urllib.request.urlopen(url+"/settings").read().decode()
            assert "legacy-secret" not in html and "password =" not in html
            csrf=re.search(r'name="csrf" value="([^"]+)"',html)[1]
            request = urllib.request.Request(
                url+"/backup", data=urllib.parse.urlencode({"csrf":csrf}).encode())
            with urllib.request.urlopen(request) as response:
                assert response.headers.get_content_type() == "application/zip"
                with zipfile.ZipFile(io.BytesIO(response.read())) as archive:
                    assert set(archive.namelist()) == {"manifest.json", "station.sqlite"}
            revision=re.search(r'name="revision" value="([^"]+)"',html)[1]
            for expected, malformed in [("stale", base), (revision, 'port = "broken"')]:
                data = urllib.parse.urlencode({
                    "csrf": csrf, "revision": expected, "config": malformed,
                }).encode()
                try:
                    urllib.request.urlopen(urllib.request.Request(url+"/settings",data=data))
                    raise AssertionError("accepted a stale or invalid configuration")
                except HTTPError as error:
                    assert error.code == 400
                assert "receiver = \"west\"" in config.read_text()
            original = config.read_text()
            malformed = original + "\nport = [\n"
            config.write_text(malformed)
            deadline = time.monotonic() + 5
            while True:
                with closing(sqlite3.connect(Path(tmp) / "station.sqlite")) as db, db:
                    rejected = db.execute(
                        "SELECT 1 FROM system_log WHERE message LIKE 'TOML reload failed:%'"
                    ).fetchone()
                    if rejected:
                        assert db.execute(
                            "SELECT status FROM workers WHERE id='west'"
                        ).fetchone() == ("Worker launched",)
                        break
                assert proc.poll() is None and time.monotonic() < deadline
                time.sleep(.05)
            for path in ("", "/system-summary", "/receivers"):
                with urllib.request.urlopen(url + path, timeout=2) as page:
                    assert page.status == 200
                    recovered = page.read()
                assert b"Repair the TOML file on the server" in recovered
                assert b"Station TOML error:" in recovered
                assert b"Error while parsing array" in recovered
                if path == "/receivers":
                    assert b"west" in recovered and b"Worker launched" in recovered
            assert config.read_text() == malformed
            config.write_text(original)
            data=urllib.parse.urlencode({"csrf":csrf,"revision":revision,"config":base}).encode()
            urllib.request.urlopen(urllib.request.Request(url+"/settings",data=data)).close()
            try:
                assert client.recv(1)==b""
            except ConnectionResetError:
                # Windows resets a connection whose peer was terminated with
                # data still queued; either signal means the allocation went away.
                pass
            assert config.read_text() == base
            with urllib.request.urlopen(urllib.request.Request(url)) as page:
                assert page.geturl().endswith("/wizard")
            with urllib.request.urlopen(urllib.request.Request(url+"/wizard")) as page:
                html = page.read().decode()
            revision=re.search(r'name="revision" value="([^"]+)"',html)[1]
            data=urllib.parse.urlencode({
                "csrf":csrf,"revision":revision,"receiver":"east",
                "callsign":"w1xyz","email":"operator@example.test","name":"Station Operator","grid":"FN42",
                "device":f"rtltcp:127.0.0.1:{radio.getsockname()[1]}",
                "rate":48000,"center_hz":14075500,"mode":"CW",
            }).encode()
            urllib.request.urlopen(urllib.request.Request(url+"/wizard",data=data)).close()
            saved = config.read_text()
            assert re.search(r"""receiver = ['"]east['"]""", saved)
            assert "dial_hz" not in saved and re.search(r"""mode = ['"]CW['"]""", saved)
            assert "first_run_wizard = false" in saved
            assert "[station]" in saved and "operator@example.test" in saved
            with urllib.request.urlopen(urllib.request.Request(url+"/system-summary")) as response:
                summary = response.read()
                assert b"W1XYZ" in summary and b"FN42" in summary
                assert b"operator@example.test" not in summary
            with urllib.request.urlopen(urllib.request.Request(url+"/wizard")) as response:
                wizard = response.read()
                first_step, hardware = wizard.split(b'id="pane-1"', 1)
                for field in (b"callsign", b"email", b"name", b"grid"):
                    assert b'name="' + field + b'"' in first_step
                assert b'name="receiver"' not in first_step
                assert b'name="receiver"' in hardware
                assert b'name="callsign" value="W1XYZ"' in first_step
        with radio.accept()[0] as client:
            client.sendall(b"RTL0"+bytes(8))
            with client.makefile("rb") as stream:
                assert stream.read(10) == b"\x02"+(48000).to_bytes(4,"big")+b"\x01"+(14075500).to_bytes(4,"big")
    finally:
        stop(proc, timeout=6)
