"""Browser restore, diagnostics and factory reset through a running station."""
import io
import json
import re
import socket
import sqlite3
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
from contextlib import closing
from pathlib import Path

from process_control import popen, stop

server = sys.argv[1]


def post(url, fields, files=None):
    """POSTs a form, multipart when files are given. Returns (status, body)."""
    if files is None:
        data = urllib.parse.urlencode(fields).encode()
        headers = {}
    else:
        boundary = uuid.uuid4().hex
        body = io.BytesIO()
        for name, value in fields.items():
            body.write(f'--{boundary}\r\nContent-Disposition: form-data; '
                       f'name="{name}"\r\n\r\n{value}\r\n'.encode())
        for name, (filename, content) in files.items():
            body.write(f'--{boundary}\r\nContent-Disposition: form-data; '
                       f'name="{name}"; filename="{filename}"\r\n'
                       'Content-Type: application/zip\r\n\r\n'.encode())
            body.write(content + b"\r\n")
        body.write(f"--{boundary}--\r\n".encode())
        data = body.getvalue()
        headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    try:
        with urllib.request.urlopen(
                urllib.request.Request(url, data=data, headers=headers)) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def wait_for(url, condition=lambda body: True, deadline=20):
    end = time.monotonic() + deadline
    while True:
        try:
            with urllib.request.urlopen(url, timeout=2) as page:
                body = page.read().decode()
                if condition(body):
                    return body
        except OSError:
            pass
        assert time.monotonic() < end, f"timed out waiting for {url}"
        time.sleep(0.1)


def spots(database):
    with closing(sqlite3.connect(database, timeout=5)) as db:
        return db.execute("SELECT count(*) FROM spots").fetchone()[0]


def log(database):
    with closing(sqlite3.connect(database, timeout=5)) as db:
        return [row[0] for row in db.execute(
            "SELECT message FROM system_log ORDER BY rowid")]


with tempfile.TemporaryDirectory() as tmp, socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    url = f"http://127.0.0.1:{port}"
    tmp = Path(tmp)
    config = tmp / "station.toml"
    config.write_text(f'''listen_address = "127.0.0.1"
port = {port}
first_run_wizard = false

[station]
callsign = "W1XYZ"
email = "private@example.test"
name = "Private Operator"
grid = "FN42"
''')
    database = tmp / "station.sqlite"
    proc = popen([server, config])
    try:
        page = wait_for(url + "/backup-restore")
        csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
        assert 'action="/restore"' in page and 'action="/factory-reset"' in page
        with closing(sqlite3.connect(database, timeout=5)) as db, db:
            db.execute("INSERT INTO spots(receiver,utc_ms,mode,frequency_hz,"
                       "message,callsign) VALUES('hf',1,'FT8',14074000,"
                       "'CQ W1AW FN31','W1AW')")
        status, snapshot = post(url + "/backup", {"csrf": csrf})
        assert status == 200 and snapshot[:2] == b"PK"
        with closing(sqlite3.connect(database, timeout=5)) as db, db:
            db.execute("DELETE FROM spots")
        assert spots(database) == 0

        # Diagnostics are redacted and contain no receptions.
        status, bundle = post(url + "/diagnostics", {"csrf": csrf})
        assert status == 200
        with zipfile.ZipFile(io.BytesIO(bundle)) as archive:
            names = set(archive.namelist())
            assert {"manifest.json", "about.json", "station.toml",
                    "workers.json", "system_log.json", "errors.json"} <= names
            toml = archive.read("station.toml").decode()
            assert "W1XYZ" in toml and "private@example.test" not in toml
            assert "Private Operator" not in toml
            assert json.loads(archive.read("errors.json")) == {}
            about = json.loads(archive.read("about.json"))
            assert about["schema_version"] >= 2 and about["receiver_plugins"]
            workers = {w["id"] for w in json.loads(archive.read("workers.json"))}
            assert "@controller" in workers and "@panel" in workers

        # Forged and invalid uploads are refused without restarting.
        status, _ = post(url + "/restore", {"csrf": "forged"},
                         {"snapshot": ("s.zip", snapshot)})
        assert status == 403, status
        status, body = post(url + "/restore", {"csrf": csrf},
                            {"snapshot": ("s.zip", b"not a zip")})
        assert status == 400 and b"Snapshot rejected" in body, body
        status, _ = post(url + "/factory-reset", {"csrf": csrf, "confirm": "no"})
        assert status == 400

        status, body = post(url + "/restore", {"csrf": csrf},
                            {"snapshot": ("s.zip", snapshot)})
        assert status == 200, body
        wait_for(url + "/system-log",
                 lambda body: "Restored snapshot uploaded" in body)
        assert spots(database) == 1
        assert list(tmp.glob("station.sqlite.pre-restore-*.zip"))
        assert not list(tmp.glob("*.pending-restore.zip*"))

        page = wait_for(url + "/backup-restore")
        csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
        status, body = post(url + "/factory-reset",
                            {"csrf": csrf, "confirm": "RESET"})
        assert status == 200, body
        wait_for(url + "/system-log", lambda body: "Factory reset" in body)
        assert spots(database) == 0
        assert list(tmp.glob("station.sqlite.pre-reset-*.zip"))
        assert "private@example.test" in config.read_text(), "TOML was kept"
        assert proc.poll() is None
    finally:
        stop(proc, timeout=6)
