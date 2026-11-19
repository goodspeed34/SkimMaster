import contextlib
import hashlib
import http.server
import json
from pathlib import Path
import queue
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from process_control import popen, stop

exe, fixture = sys.argv[1:]
events = queue.Queue()
verification_requests = queue.Queue()
uploads = queue.Queue()

class Mock(http.server.BaseHTTPRequestHandler):
    accept_wspr = False
    rbn_response = None

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        events.put((self.path, body))
        if self.path == "/rx/6/s.php":
            uploads.put(body)
        self.send_response(200)
        self.end_headers()
        response = self.rbn_response if self.path == "/rx/6/id.php" else None
        if response is None:
            response = {
            "nodeId": "local-test", "serverUploadInterval": 1, "Limit3Min": 2,
            "spotSettings": [{"startFreq": 14000, "endFreq": 14100, "cq": True,
                               "de": False, "bcn": False, "modes": "FT8"}]
            }
        self.wfile.write(json.dumps(response).encode())

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        if parsed.path == "/fb0.php":
            verification_requests.put(params)
            body = b"[]"
        else:
            events.put(params)
            body = b"1 spot(s) added" if self.accept_wspr else b"0 spot(s) added"
        self.send_response(200)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass

@contextlib.contextmanager
def run(*args):
    proc = popen([exe, *args])
    try:
        yield
    finally:
        stop(proc)

with tempfile.TemporaryDirectory() as tmp, http.server.HTTPServer(("127.0.0.1", 0), Mock) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_port
    db = Path(tmp) / "spots.db"
    config = Path(tmp) / "station.json"
    common = dict(callsign="W1XYZ", grid="FN42", host="127.0.0.1",
                  sign_in="test", band_limits="14000-14100")
    def write(**extra):
        config.write_text(json.dumps(dict(common, **extra)))
    now = int(time.time()) * 1000
    subprocess.run([exe, "--decode", "FT8", fixture, "14074000", str(now//15000*15000),
                    "archive", db], check=True, capture_output=True)
    with contextlib.closing(sqlite3.connect(db)) as con, con:
        assert con.execute("SELECT count(*) FROM live_receptions").fetchone() == (0,)
        def insert(mode, hz, message, call, grid):
            cursor = con.execute("INSERT INTO spots VALUES(?,?,?,?,?,?,?)",
                                 ("radio", now, mode, hz, message, call, grid))
            con.execute("INSERT INTO live_receptions VALUES(?,?,?)", (cursor.lastrowid,-10,None))
            return cursor.lastrowid
        fid = insert("FT8", 14075200, "CQ W1ABC FN30", "W1ABC", "FN30")
        insert("FT8", 14075201, "invalid metadata", "INVALID\nCALL", "FN30")
        wid = insert("WSPR", 14097100, "K1JT FN20 30", "K1JT", "FN20")
        con.execute("INSERT INTO wspr_details VALUES(?,?,?,?)", (wid,14095600,.2,"30"))
    write(port=port)
    with run("--report","rbn",db,config):
        path, ident = events.get(timeout=5)
        assert path=="/rx/6/id.php" and ident["shortHash"]==hashlib.md5(b"hellotestworld").hexdigest()[:8]
        path, batch = events.get(timeout=5)
        assert path=="/rx/6/s.php" and batch["s"][0][:7]==["14075.20","W1ABC","1","-10","","0","34"]
        assert len(batch["s"]) == 1
        assert batch["h"]==hashlib.md5(b"W1abc").hexdigest()[:8]
        assert uploads.get(timeout=5) == batch
        for attempt in range(100):
            with contextlib.closing(sqlite3.connect(db)) as con, con:
                if con.execute("SELECT count(*) FROM report_receipts WHERE feed='rbn'").fetchone()==(1,):
                    break
            time.sleep(.01)
        else:
            raise AssertionError("acknowledgement was not persisted")

    # A valid partial response opts out of uploading when the server omits
    # cadence and windows. The verification request proves it was accepted.
    with contextlib.closing(sqlite3.connect(db)) as con, con:
        cursor = con.execute("INSERT INTO spots VALUES(?,?,?,?,?,?,?)",
                             ("radio", now, "FT8", 14075300,
                              "CQ W2TEST FN31", "W2TEST", "FN31"))
        con.execute("INSERT INTO live_receptions VALUES(?,?,?)",
                    (cursor.lastrowid, -8, None))
    Mock.rbn_response = {
        "nodeId": "partial-node",
        "statusFeedbackHttpStatusInterval": 1,
    }
    write(port=port, verification_endpoint=f"http://127.0.0.1:{port}")
    with run("--report", "rbn", db, config):
        path, ident = events.get(timeout=5)
        assert path == "/rx/6/id.php"
        assert verification_requests.get(timeout=5) == {"n": ["partial-node"]}
        time.sleep(1.1)
        assert uploads.empty(), "partial handshake must not authorize spot uploads"
    with contextlib.closing(sqlite3.connect(db)) as con, con:
        assert con.execute(
            "SELECT count(*) FROM report_receipts WHERE feed='rbn'"
        ).fetchone() == (1,)

    # A field that is present with the wrong type remains a rejected reply.
    Mock.rbn_response = {"serverUploadInterval": "1", "spotSettings": []}
    write(port=port, verification_endpoint=f"http://127.0.0.1:{port}")
    with run("--report", "rbn", db, config):
        path, _ = events.get(timeout=5)
        assert path == "/rx/6/id.php"
        time.sleep(.2)
        assert verification_requests.empty()
        assert uploads.empty()
    Mock.rbn_response = None

    write(endpoint=f"http://127.0.0.1:{port}")
    # A successful HTTP exchange can still contain a WSPRnet rejection.
    with run("--report","wsprnet",db,config):
        assert events.get(timeout=5)["tcall"] == ["K1JT"]
        time.sleep(.1)
        with contextlib.closing(sqlite3.connect(db)) as con, con:
            assert con.execute(
                "SELECT count(*) FROM report_receipts WHERE feed='wsprnet'"
            ).fetchone() == (0,)
    Mock.accept_wspr = True
    with run("--report","wsprnet",db,config):
        params = events.get(timeout=5)
        assert params["tcall"]==["K1JT"] and params["dbm"]==["30"] and params["dt"]==["0.2"]
        for attempt in range(100):
            with contextlib.closing(sqlite3.connect(db)) as con, con:
                if con.execute("SELECT spot_id FROM report_receipts WHERE feed='wsprnet'").fetchone() == (wid,):
                    break
            time.sleep(.01)
        else:
            raise AssertionError("WSPRnet acknowledgement was not persisted")
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
        udp.bind(("127.0.0.1",0))
        udp.settimeout(5)
        write(port=udp.getsockname()[1])
        with run("--report","psk",db,config):
            packet=udp.recv(1500)
            assert struct.unpack(">HH",packet[:4])==(10,len(packet))
            assert b"W1ABC" in packet and b"K1JT" not in packet
            assert b"R7IW" not in packet
            assert b"INVALID" not in packet
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        cluster_port = probe.getsockname()[1]
    with run("--cluster", db, "W1XYZ", "FN42", str(cluster_port)):
        for attempt in range(100):
            try:
                client = socket.create_connection(("127.0.0.1", cluster_port), timeout=2)
                break
            except OSError:
                if attempt == 99:
                    raise
                time.sleep(.02)
        with client, client.makefile("rb") as stream:
            client.sendall(b"W1XYZ\r\n")
            for line in stream:
                if b"Connected to Skim Master" in line:
                    break
            else:
                raise AssertionError("cluster disconnected during login")
            with contextlib.closing(sqlite3.connect(db)) as con, con:
                con.execute("INSERT INTO spots VALUES(?,?,?,?,?,?,?)",
                            ("archive", now, "FT8", 14075300, "CQ W1FILE FN30", "W1FILE", "FN30"))
                insert("FT8", 14075400, "CQ W1LIVE FN30", "W1LIVE", "FN30")
            line = stream.readline()
            assert b"W1LIVE" in line and b"W1FILE" not in line, line
    server.shutdown()
