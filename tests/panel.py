from contextlib import closing
from pathlib import Path
from datetime import datetime, timezone
import json
import os
import re
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import threading
import wave
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
from process_control import popen, stop
from flex_emulator import FlexRadio
from tci_emulator import TciServer

with tempfile.TemporaryDirectory() as directory:
    server = Path(sys.argv[2]).resolve()
    config = Path(directory) / "config.toml"
    first = popen([server], cwd=directory)
    try:
        for attempt in range(100):
            if config.exists() and 'gallery = "gallery"' in config.read_text():
                break
            if first.poll() is not None or attempt == 99:
                raise AssertionError("default configuration was not generated")
            time.sleep(.02)
        assert 'listen_address = "127.0.0.1"' in config.read_text()
    finally:
        stop(first)

    db = Path(directory) / "station.sqlite"
    subprocess.run([sys.argv[1], "--decode", "FT8", sys.argv[3], "14074000",
                    "0", "<antenna>", db], check=True, capture_output=True)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    settings = (f'port = {port}\nfirst_run_wizard = false\n'
                '[station]\ncallsign = "N0XYZ"\nemail = "op@example.net"\n'
                'name = "Panel test"\ngrid = "FN42"\n\n'
                '[services.cluster]\nenabled = false\n'
                'advanced_option = "preserve-me"\n')
    config.write_text(settings)
    proc = popen([server], cwd=directory, stderr=subprocess.PIPE, text=True)
    def get(path):
        with urlopen(f"http://127.0.0.1:{port}{path}", timeout=2) as response:
            return response.read()
    def post(path, data, status=200):
        try:
            with urlopen(Request(f"http://127.0.0.1:{port}{path}",
                                 urlencode(data).encode()), timeout=5) as response:
                assert response.status == status
                return response.read()
        except HTTPError as error:
            body = error.read()
            assert error.code == status, (path, error.code, body[:500])
            return body

    def form(path):
        return {k.decode(): v.decode() for k, v in
                re.findall(rb'name="(csrf|revision)" value="([^"]+)"', get(path))}
    try:
        for attempt in range(100):
            try:
                html = get("/")
                break
            except OSError:
                if proc.poll() is not None or attempt == 99:
                    raise
                time.sleep(.02)
        reference = (Path(__file__).resolve().parents[1] / "ReferenceDesign")

        def source(name):
            # embed.py reads these in text mode, so the embedded copies always
            # use LF; normalize so a CRLF checkout still compares equal.
            return (reference / name).read_bytes().replace(b"\r\n", b"\n")

        design = source("index.html")
        assert re.search(rb"<svg.*?</svg>", design, re.S)[0] in html
        assert b'class="sub-bar"' in html
        assert b'<div class="help-header">Help &amp; Parameter Notes</div>' in html
        assert b'id="page-system-summary"' in html
        assert b"Daemon &amp; Station Information" in html
        assert b"Decoder Engines Activity (24h Window)" in html
        assert re.findall(rb'data-page="([^"]+)"', html) == re.findall(rb'data-page="([^"]+)"', design)
        receiver_page = get("/receiver-status")
        assert b'data-page="receiver-status" aria-current="page"' in receiver_page
        assert b'<b>Receiver pool:</b>' in receiver_page
        source_offer = re.search(rb'<a href="(https?://[^"]+)">Source code</a>', receiver_page)
        assert source_offer is not None
        assert b'<div class="page-title">Slice Allocation Table</div>' in get("/slice-allocation")
        noise = get("/noise-floor")
        assert b"No noise-floor history yet" in noise and b'data-chart="noise"' in noise
        minute = int(time.time()) // 60 * 60
        history = [("hf", minute - 60 * (i + 1), 14074000, 192000,
                    -150.0 if i < 12 else -153.0, 60) for i in range(12)]
        history += [("hf", minute - 6 * 3600 - 60 * (i + 1), 14074000, 192000,
                     -153.0, 60) for i in range(12)]
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.executemany(
                "INSERT INTO noise_floor VALUES(?,?,?,?,?,?)", history)
        noise = get("/noise-floor?threshold=-151.5&smoothing=1")
        row = re.search(rb"<tr><td><b>20m</b></td><td>hf</td>(.*?)</tr>", noise)
        assert row, noise
        assert b">-153.0<" in row[1] and b">-151.5<" in row[1] and b">-150.0<" in row[1]
        assert b'class="status-warn">+3.0 dB / 6h' in row[1], row[1]
        assert b">24<" in row[1] and b">12<" in row[1], row[1]
        assert b"hf on 20m exceeded the threshold in 12 of 24 minutes" in noise
        points = json.loads(get("/data/noise?receiver=hf&smoothing=1"))
        assert len(points) == 24 and points[-1][0] == "hf", points
        assert len(json.loads(get("/data/noise?smoothing=15"))) <= 4
        noise_csv = get("/noise-floor?format=csv&receiver=hf").decode()
        assert noise_csv.startswith('"UTC","Receiver","Band","RF Center (Hz)"')
        assert noise_csv.count("\n") == 25, noise_csv
        assert '"hf","20m","14074000","192000","-150.00","60"' in noise_csv
        bad_threshold = get("/noise-floor?threshold=loud")
        assert b"invalid threshold" in bad_threshold
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.execute("DELETE FROM noise_floor")
        now_ms = int(time.time() * 1000)
        activity = [("hf", now_ms - 1000, "FT8", 14074500, "CQ A1AA", "A1AA"),
                    ("hf", now_ms - 2000, "FT8", 14075000, "CQ B1BB", "B1BB"),
                    ("hf", now_ms - 3000, "FT8", 14075500, "A1AA B1BB", "A1AA"),
                    ("lf", now_ms - 4000, "CW", 7030000, "K1CW", "K1CW"),
                    ("lf", now_ms - 5000, "CW", 7031000, "K2CW", "K2CW"),
                    ("lf", now_ms - 6000, "RTTY", 13000000, "CQ X", "X1X")]
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.executemany(
                "INSERT INTO spots(receiver,utc_ms,mode,frequency_hz,message,"
                "callsign) VALUES(?,?,?,?,?,?)", activity)
        bands = get("/band-mode-activity").decode()
        matrix = bands[bands.index('id="band-matrix"'):bands.index('id="hour-heatmap"')]
        assert re.search(r"<b>40m</b></td><td class=\"num heat-1\">2</td>", matrix), matrix
        assert re.search(r"<b>20m</b></td>(<td[^>]*>[^<]*</td>){2}<td class=\"num heat-1\">3</td>", matrix), matrix
        assert "<b>Out of band</b>" in matrix
        assert matrix.index("<b>40m</b>") < matrix.index("<b>20m</b>") < matrix.index("Out of band")
        assert "<td class=\"num\"><b>6</b></td>" in matrix, matrix
        totals = bands[bands.index('id="mode-totals"'):]
        assert re.search(r"<b>FT8</b></td><td class=\"num mono\">3</td>.*?bar-fill orange.*? 50\.0%</td><td class=\"num mono\">2</td>", totals, re.S), totals
        cw_only = get("/band-mode-activity?applied=1&include_CW=on&normalize=band&display=numeric").decode()
        cw_matrix = cw_only[cw_only.index('id="band-matrix"'):cw_only.index('id="hour-heatmap"')]
        assert "<th class=\"num\">CW</th><th class=\"num\">Total</th>" in cw_matrix, cw_matrix
        assert "100.0%" in cw_matrix and "heat-" not in cw_only.split('id="mode-totals"')[0].split('id="band-matrix"')[1]
        assert "20m" not in cw_matrix
        receiver_only = get("/band-mode-activity?receiver=hf").decode()
        assert "40m" not in receiver_only.split('id="band-matrix"')[1].split('id="hour-heatmap"')[0]
        matrix_csv = get("/band-mode-activity?format=csv").decode()
        assert matrix_csv.startswith('"Band","CW","RTTY","FT8","FT4","WSPR","Total"'), matrix_csv
        assert '"20m","0","0","3","0","0","3"' in matrix_csv, matrix_csv
        assert '"Out of band","0","1","0","0","0","1"' in matrix_csv, matrix_csv
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.execute("DELETE FROM spots WHERE utc_ms>=?", (now_ms - 10000,))
        assert b'canvas id="chart"' in get("/spectrum-waterfall")
        assert get("/style.css") == source("style.css")
        assert b"CQ R7IW LN35" in html and b"&lt;antenna&gt;" in html
        assert b"R7IW" in get("/stations")
        assert b"R7IW" not in get("/stations?call=ZZ9")
        digital = get("/digital-decoders?mode=FT8&pause=on")
        assert b"Recent Stored Decoder Results" in digital and b"R7IW" in digital
        assert b"&lt;antenna&gt;" in digital
        with urlopen(f"http://127.0.0.1:{port}/digital-decoders?mode=FT8&pause=on") as response:
            assert response.headers.get("Refresh") is None
        with urlopen(f"http://127.0.0.1:{port}/digital-decoders?mode=FT8&format=csv") as response:
            assert response.headers.get_content_type() == "text/csv"
            assert response.headers.get("Content-Disposition") == "attachment; filename=digital-decoders.csv"
            export = response.read()
            assert b"Callsign" in export and b"R7IW" in export
        stream_csv = get("/live-spot-stream?mode=FT8&call=R7&format=csv")
        assert b'"<antenna>","FT8"' in stream_csv
        assert b"R7IW" not in get("/live-spot-stream?mode=RTTY&format=csv")
        with urlopen(f"http://127.0.0.1:{port}/live-spot-stream?pause=on") as paused:
            assert "Refresh" not in paused.headers
        assert b"name=\"receiver\"" in get("/spectrum-waterfall")
        assert b"FT8" in get("/activity")
        assert b"No SSTV images match these filters" in get("/gallery")
        cluster_page = get("/dx-cluster")
        assert b"Local DX Cluster Status" in cluster_page
        assert b"Disabled" in cluster_page and b"127.0.0.1:7300" in cluster_page
        reporting_page = get("/psk-reporter")
        assert b"PSK Reporter Settings" in reporting_page
        assert b"WSPRnet Settings" in reporting_page
        assert b"https://wsprnet.org" in reporting_page
        gallery = Path(directory) / "gallery"
        sstv_test = Path(__file__).resolve().with_name("sstv.py")
        subprocess.run([sys.executable, sstv_test, sys.argv[1], "Martin2", gallery],
                       check=True, timeout=15, capture_output=True)
        today = datetime.now(timezone.utc).date().isoformat()
        gallery_path = f"/sstv-gallery?mode=Martin2&from={today}&to={today}"
        gallery_html = get(gallery_path)
        images = list(gallery.glob("*-Martin2.png"))
        assert len(images) == 1 and images[0].name.encode() in gallery_html
        filename = images[0].name
        with urlopen(f"http://127.0.0.1:{port}/image/{filename}") as response:
            image_bytes = response.read()
            assert response.headers.get_content_type() == "image/png"
            assert image_bytes.startswith(b"\x89PNG\r\n\x1a\n")
        with urlopen(f"http://127.0.0.1:{port}{gallery_path}&format=csv") as response:
            assert response.headers.get_content_type() == "text/csv"
            assert filename.encode() in response.read()
        try:
            with urlopen(f"http://127.0.0.1:{port}/sstv-gallery?from=2026-02-30"):
                raise AssertionError("accepted an invalid gallery date")
        except HTTPError as error:
            assert error.code == 400 and b"invalid gallery start date" in error.read()
        junk_timestamp = gallery / "9000000000000000000-Martin1.png"
        junk_timestamp.write_bytes(b"bad image")
        assert b"Martin2" in get("/sstv-gallery")
        symlink = gallery / "9999999999999999-Martin1.png"
        try:
            os.symlink(images[0], symlink)
        except (NotImplementedError, OSError):
            symlink = None
        if symlink:
            symlink_detected = symlink.is_symlink()
            try:
                with urlopen(f"http://127.0.0.1:{port}/image/{symlink.name}") as response:
                    raise AssertionError("served a symlink as an SSTV image")
            except HTTPError as error:
                assert error.code == 404
            denied = post("/gallery-delete", {**form("/sstv-gallery"),
                            "filename": symlink.name}, 400)
            assert b"not a regular file" in denied
            assert images[0].exists() and images[0].read_bytes() == image_bytes
            # Some Windows/Wine environments accept os.symlink() but expose the
            # resulting path as an ordinary file or a missing path. Check link
            # persistence only where Python can actually identify the link.
            if symlink_detected:
                assert symlink.is_symlink()
        deleted = post("/gallery-delete", {**form(gallery_path),
                        "filename": filename, "mode": "Martin2",
                        "from": today, "to": today})
        assert b"SSTV image deleted" in deleted and not images[0].exists()
        junk_timestamp.unlink()
        assert b".top-banner" in get("/style.css")
        assert config.read_text() == settings
        reset_service = post("/service", {
            **form("/dx-cluster"), "service": "cluster",
            "callsign": "", "grid": ""})
        assert b"Service settings saved" in reset_service
        cluster_config = re.search(
            r"\[services\.cluster\]\s*(.*?)(?:\n\[|\Z)",
            config.read_text(), re.S)
        assert cluster_config and "port =" not in cluster_config[1]
        with socket.socket() as service_probe:
            service_probe.bind(("127.0.0.1", 0))
            service_port = service_probe.getsockname()[1]
        service_form = {**form("/dx-cluster"), "service": "cluster",
                        "enabled": "on", "port": str(service_port),
                        "callsign": "", "grid": ""}
        saved_service = post("/service", service_form)
        assert b"Service settings saved" in saved_service
        for attempt in range(150):
            cluster_page = get("/dx-cluster")
            if b"Worker launched" in cluster_page:
                break
            time.sleep(.04)
        else:
            raise AssertionError("enabled local cluster worker did not launch")
        service_text = config.read_text()
        cluster_config = re.search(
            r"\[services\.cluster\]\s*(.*?)(?:\n\[|\Z)", service_text, re.S)
        assert cluster_config and "preserve-me" in cluster_config[1]
        assert "callsign" not in cluster_config[1] and "grid" not in cluster_config[1]
        with closing(sqlite3.connect(db)) as connection:
            before_restarts = connection.execute(
                "SELECT count(*) FROM system_log WHERE component='@cluster' "
                "AND message='Stopped allocation'").fetchone()[0]
        restarted = post("/service-restart", {
            **form("/dx-cluster"), "service": "cluster"})
        assert b"Local service worker restart requested" in restarted
        for attempt in range(150):
            with closing(sqlite3.connect(db)) as connection:
                after_restarts = connection.execute(
                    "SELECT count(*) FROM system_log WHERE component='@cluster' "
                    "AND message='Stopped allocation'").fetchone()[0]
            if after_restarts > before_restarts:
                break
            time.sleep(.04)
        else:
            raise AssertionError("local service restart was not applied")
        rtty_page = get("/rtty-skimmer")
        assert b"RTTY Decoder Settings" in rtty_page
        assert b'name="shift_hz"' in rtty_page and b'name="reverse"' in rtty_page
        digital_page = get("/digital-decoders")
        for mode in [b"FT8", b"FT4", b"WSPR"]:
            assert mode + b" Decoder Settings" in digital_page
        assert b'name="ldpc_iterations"' in digital_page
        assert b'name="passes"' in digital_page
        assert b"Live Channel Text" in rtty_page
        assert b"Live Channel Text" not in digital_page
        assert json.loads(get("/data/live-text?mode=CW")) == []
        now_ms = int(time.time() * 1000)
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.executemany(
                "INSERT INTO live_text VALUES(?,?,?,?,?,?,?,?)",
                [("s1", "hf", "CW", 7030500, "CQ CQ DE K1<b>X", 12.5, 24.0,
                  now_ms),
                 ("s1", "hf", "CW", 7031000, "stale", None, None,
                  now_ms - 120000)])
        live = json.loads(get("/data/live-text?mode=CW&receiver=hf"))
        assert live == [["hf", "7030500", "24 WPM", "12.5 dB",
                         "CQ CQ DE K1<b>X"]], live
        assert json.loads(get("/data/live-text?mode=CW&receiver=other")) == []
        cw_page = get("/cw-skimmer")
        assert b"CQ CQ DE K1&lt;b&gt;X" in cw_page and b"stale" not in cw_page
        assert b'data-mode="CW"' in cw_page
        try:
            get("/data/live-text?mode=FT8")
            raise AssertionError("live text accepted a slot mode")
        except HTTPError as error:
            assert error.code == 400
        with closing(sqlite3.connect(db)) as connection, connection:
            connection.execute("DELETE FROM live_text")
        def multipart(path, fields, files, status=200):
            boundary = "skimmaster-test-boundary"
            body = b""
            for name, value in fields.items():
                body += (f"--{boundary}\r\nContent-Disposition: form-data; "
                         f'name="{name}"\r\n\r\n{value}\r\n').encode()
            for name, (filename, content) in files.items():
                body += (f"--{boundary}\r\nContent-Disposition: form-data; "
                         f'name="{name}"; filename="{filename}"\r\n'
                         "Content-Type: audio/wav\r\n\r\n").encode()
                body += content + b"\r\n"
            body += f"--{boundary}--\r\n".encode()
            request = Request(f"http://127.0.0.1:{port}{path}", body, {
                "Content-Type": f"multipart/form-data; boundary={boundary}"})
            try:
                with urlopen(request, timeout=60) as response:
                    assert response.status == status
                    return response.read()
            except HTTPError as error:
                assert error.code == status, (error.code, error.read())
                return error.read()

        with closing(sqlite3.connect(db)) as connection:
            spots_before = connection.execute(
                "SELECT count(*) FROM spots").fetchone()[0]
        csrf = form("/digital-decoders")["csrf"]
        ft8 = multipart("/decode", {"csrf": csrf, "mode": "FT8",
                                    "reference_hz": "14074000"},
                        {"recording": ("slot.wav", Path(sys.argv[3]).read_bytes())})
        assert b"Recording Results" in ft8 and b"Nothing was stored" in ft8
        assert b"CQ R7IW LN35" in ft8 and b"14075291" in ft8, ft8
        assert b"Refresh" not in ft8
        # RTTY: 12 kHz stereo IQ carrying "CQ K1ABC" 1030 Hz below center.
        import array, cmath, math
        bits = [(1, 6)]
        for code in [14, 23, 4, 15, 27, 23, 31, 3, 25, 14, 8]:
            bits += [(0, 1)] + [(code >> i & 1, 1) for i in range(5)] + [(1, 1.5)]
        samples, phase = array.array("h"), 0
        for mark, duration in bits:
            step = math.tau * (-1030 if mark else -1200) / 12000
            for _ in range(round(duration * 12000 / 45.45)):
                z = cmath.exp(1j * phase)
                samples.extend((int(z.real * 16000), int(z.imag * 16000)))
                phase = (phase + step) % math.tau
        rtty_wav = Path(directory) / "rtty.wav"
        with wave.open(str(rtty_wav), "wb") as output:
            output.setparams((2, 2, 12000, 0, "NONE", ""))
            output.writeframes(samples.tobytes())
        rtty = multipart("/decode", {"csrf": csrf, "mode": "RTTY",
                                     "reference_hz": "14080000"},
                         {"recording": ("rtty.wav", rtty_wav.read_bytes())})
        assert b"CQ K1ABC" in rtty and re.search(rb">1407896[0-9]<", rtty), rtty
        assert b"Channel Text at End of Recording" in rtty
        wrong = multipart("/decode", {"csrf": csrf, "mode": "CW",
                                      "reference_hz": "7030000"},
                          {"recording": ("rtty.wav", rtty_wav.read_bytes())},
                          400)
        assert b"CW needs stereo IQ at 48000 Hz" in wrong
        # The WAV probe below expects one stereo recording in the directory.
        rtty_wav.unlink()
        forged = multipart("/decode", {"csrf": "forged", "mode": "FT8",
                                       "reference_hz": "14074000"},
                           {"recording": ("slot.wav", b"RIFF")}, 403)
        assert b"form expired" in forged
        with closing(sqlite3.connect(db)) as connection:
            assert connection.execute(
                "SELECT count(*) FROM spots").fetchone()[0] == spots_before
        rtty_form = {**form("/rtty-skimmer"), "mode": "RTTY",
                     "shift_hz": "850", "baud": "45.45", "reverse": "on",
                     "max_channels": "16", "threshold": "24"}
        saved_rtty = post("/decoder-settings", rtty_form)
        assert b"RTTY decoder settings saved" in saved_rtty
        assert b'<option value="850" selected>' in saved_rtty
        rtty_config = re.search(r"\[decoders\.RTTY\]\s*(.*?)(?:\n\[|\Z)",
                                config.read_text(), re.S)
        assert rtty_config, config.read_text()
        assert "shift_hz = 850" in rtty_config[1]
        assert "reverse = true" in rtty_config[1]
        assert "threshold = 24" in rtty_config[1]
        assert "baud" not in rtty_config[1] and "max_channels" not in rtty_config[1]
        rejected = post("/decoder-settings", {**form("/rtty-skimmer"),
                        **rtty_form, "shift_hz": "200"}, 400)
        assert b"Shift (Hz) is out of range" in rejected
        reverted = {**form("/rtty-skimmer"), "mode": "RTTY", "shift_hz": "170",
                    "baud": "45.45", "max_channels": "16", "threshold": "32"}
        post("/decoder-settings", reverted)
        assert "[decoders" not in config.read_text()
        assert b"No IQ sources configured." in get("/receiver-edit")
        assert b"Station TOML" not in get("/receiver-edit")
        wizard = get("/wizard")
        assert b"data-receiver-probe" in wizard
        assert source_offer[0] in wizard
        add = form("/receiver-edit?add=1")
        recording = Path(directory) / "short IQ.wav"
        for name, channels in [(recording, 2), (Path(directory) / "mono.wav", 1)]:
            with wave.open(str(name), "wb") as wav:
                wav.setparams((channels, 2, 48000, 0, "NONE", ""))
                wav.writeframes(bytes(4 * channels))
        found = json.loads(post("/receiver-probe", {**add, "driver": "wav"}))
        assert len(found) == 1 and found[0]["rate"] == 48000
        assert Path(found[0]["device"].removeprefix("wav:")) == recording
        capture = {"device": "wav:short IQ.wav", "rate": "48000",
                   "center_hz": "7050000"}
        assert b"IQ samples received" in post("/receiver-test", {**add, **capture})
        assert b"IQ capture failed" in post(
            "/receiver-test", {**add, **capture, "rate": "96000"}, 400)

        with socket.socket() as radio:
            radio.bind(("127.0.0.1", 0))
            radio.listen()
            radio.settimeout(5)
            def handshake():
                with radio.accept()[0] as client:
                    client.sendall(b"RTL0" + bytes(8))
            listener = threading.Thread(target=handshake)
            listener.start()
            target = f"127.0.0.1:{radio.getsockname()[1]}"
            found = json.loads(post("/receiver-probe", {**add, "driver": "rtltcp",
                                                        "target": target}))
            listener.join(5)
            assert found[0]["device"] == f"rtltcp:{target}"
            commands = []

            def stream_iq():
                with radio.accept()[0] as client:
                    client.sendall(b"RTL0" + bytes(8))
                    with client.makefile("rb") as stream:
                        commands.append(stream.read(25))
                    client.sendall(bytes(8192))

            listener = threading.Thread(target=stream_iq)
            listener.start()
            assert b"IQ samples received" in post(
                "/receiver-test", {**add, **capture, "device": f"rtltcp:{target}",
                                   "gain_db": "12.3", "ppm": "-4"})
            listener.join(5)
            assert commands == [struct.pack(">BIBIBIBIBi", 2, 48000, 1, 7050000,
                                            3, 1, 4, 123, 5, -4)]

        tci = TciServer()
        listener = threading.Thread(target=lambda: (tci.accept(), tci.close()))
        listener.start()
        found = json.loads(post("/receiver-probe", {**add, "driver": "tci",
                                                    "target": f"127.0.0.1:{tci.port}"}))
        listener.join(5)
        assert [(f["device"], f["label"]) for f in found] == [
            (f"tci:127.0.0.1:{tci.port}/{rx}", f"SunSDR2PRO receiver {rx} (TCI)")
            for rx in range(2)], found

        flex = FlexRadio()
        listener = threading.Thread(target=flex.serve_commands, daemon=True)
        listener.start()
        found = json.loads(post("/receiver-probe", {**add, "driver": "flex",
                                                    "target": f"127.0.0.1:{flex.port}"}))
        assert [(f["device"], f["label"]) for f in found] == [
            (f"flex:127.0.0.1:{flex.port}/{ch}", f"FLEX-6600 DAX IQ {ch}")
            for ch in range(1, 5)], found
        listener.join(5)
        announcing = threading.Event()

        def announce():
            while not announcing.wait(.1):
                flex.announce()

        announcer = threading.Thread(target=announce)
        announcer.start()
        try:
            found = json.loads(post("/receiver-probe", {**add, "driver": "flex",
                                                        "target": ""}))
        finally:
            announcing.set()
            announcer.join(5)
            flex.close()
        assert found and found[0]["device"] == f"flex:127.0.0.1:{flex.port}/1", found
        assert found[0]["label"] == "FLEX-6600 Shack DAX IQ 1", found

        receiver = {"action": "save", "receiver": "Desk & IQ", "device": "wav:short IQ.wav",
                    "rate": "48000", "enabled": "on", "center_hz": "7050000",
                    "mode": "CW", "repeat": "on"}
        assert b"Desk &amp; IQ" in post("/receiver-edit", {**add, **receiver})
        assert "dial_hz" not in config.read_text()
        service_text = config.read_text()
        cluster_config = re.search(
            r"\[services\.cluster\]\s*(.*?)(?:\n\[|\Z)", service_text, re.S)
        assert cluster_config and "callsign" not in cluster_config[1]
        assert "grid" not in cluster_config[1]
        active = get("/receiver-status")
        assert b"Desk &amp; IQ" in active and b"7050000" in active and b"IQ WAV" in active
        assert b"&quot;center_hz&quot;" not in active
        edit_path = "/receiver-edit?" + urlencode({"edit": "Desk & IQ"})
        assert b'value="wav:short IQ.wav"' in get(edit_path)
        params = {**receiver, "original": receiver["receiver"], "mode": "RTTY",
                  "slice_center": "7100000"}
        post("/receiver-edit", {**form(edit_path), **params})
        assert b"Outside bandwidth" in get(edit_path)
        for center, expected_rtty in [("7100000", True), ("7050000", False)]:
            post("/receiver-edit", {**form(edit_path), **params, "mode": "", "center_hz": center})
            for _ in range(150):
                status = get("/receiver-status")
                if (f"CW {center} Hz center".encode() in status and
                    (b"RTTY 7100000 Hz center" in status) == expected_rtty):
                    break
                time.sleep(.04)
            else:
                raise AssertionError("worker slices did not follow IQ tuning")
        slice_query = {"edit": receiver["receiver"], "slice_group": "slices",
                       "slice_index": "1"}
        slice_path = "/receiver-edit?" + urlencode(slice_query)
        assert b'Edit Decoder Slice' in get(slice_path)
        slice_form = {"original": receiver["receiver"], "slice_group": "slices",
                      "slice_index": "1"}
        result = post("/receiver-edit", {**form(slice_path), **slice_form,
                      "action": "slice-save", "mode": "RTTY",
                      "slice_center": "7052000"})
        assert b"7052000 Hz center" in result
        cw_page = get("/cw-skimmer?pause=on")
        assert b"Manage Slices" in cw_page and b"/receiver-edit?edit=" in cw_page
        for _ in range(150):
            rtty_page = get("/rtty-skimmer?pause=on")
            if (b"RTTY 7052000 Hz center" in get("/receiver-status") and
                    b"Manage Slices" in rtty_page):
                break
            time.sleep(.04)
        else:
            raise AssertionError("RTTY allocation did not appear on its decoder page")
        assert b"7052000 Hz center" not in post(
            "/receiver-edit", {**form(slice_path), **slice_form, "action": "slice-delete"})

        schedules = "/band-schedules"
        schedule_path = schedules + "?" + urlencode({"receiver": receiver["receiver"]})
        schedule = {"receiver": receiver["receiver"], "name": "Overnight & CW",
                    "start": "18:00", "end": "06:00", "day0": "on",
                    "priority": "2", "center_hz": "7100000", "mode": "CW"}
        assert b"Overnight &amp; CW" in post(
            schedules, {**form(schedule_path), **schedule, "action": "save"})
        schedule_path += "&index=0"
        assert b'value="18:00"' in get(schedule_path)
        assert b"Overnight &amp; CW (copy)" in post(
            schedules, {**form(schedule_path), **schedule, "index": "0",
                        "action": "duplicate"})
        assert b"Overnight &amp; CW (copy)" not in post(
            schedules, {**form(schedule_path), **schedule, "index": "1",
                        "action": "delete"})
        schedule_slice = "/receiver-edit?" + urlencode(
            {**slice_query, "slice_index": "0", "schedule_index": "0"})
        assert b"Edit Decoder Slice" in get(schedule_slice)
        post("/receiver-edit", {**form(schedule_slice), **slice_form,
                               "schedule_index": "0", "slice_index": "0",
                               "action": "slice-save", "mode": "RTTY"})
        override = {**schedule, "index": "0"}
        assert b"Override active" in post(
            schedules, {**form(schedule_path), **override, "action": "run"})

        def wait_allocation(expected):
            for _ in range(150):
                if expected in get("/receiver-status"):
                    return
                time.sleep(.04)
            raise AssertionError(f"allocation did not reach {expected!r}")

        wait_allocation(b"RTTY 7100000 Hz center")
        assert b"Override active" not in post(
            schedules, {**form(schedule_path), **override, "action": "clear"})
        wait_allocation(b"CW 7050000 Hz center")
        assert b"Overnight &amp; CW" not in post(
            schedules, {**form(schedule_path), **override, "action": "delete"})
        edit = {**form(edit_path), **receiver, "original": "Desk & IQ",
                "receiver": "Desk Archive", "enabled": "", "mode": ""}
        assert b"Desk Archive" in post("/receiver-edit", edit)
        assert "mode = 'CW'" in config.read_text() or 'mode = "CW"' in config.read_text()
        assert b"Disabled" in get("/receiver-status")
        spectrum_page = get("/spectrum-waterfall")
        assert b'id="tune-form"' in spectrum_page
        tune = {**form("/spectrum-waterfall"), "receiver": "Desk Archive",
                "mode": "FT8", "frequency": "7074000"}
        tuned = post("/spectrum-tune", tune)
        assert b"Added a FT8 slice at 7074000 Hz" in tuned, tuned
        assert "dial_hz = 7074000" in config.read_text()
        assert b"select a decoder mode" in post(
            "/spectrum-tune", {**form("/spectrum-waterfall"),
                               "receiver": "Desk Archive", "mode": "AM",
                               "frequency": "7074000"}, 400)
        assert b"Could not add the slice" in post(
            "/spectrum-tune", {**form("/spectrum-waterfall"),
                               "receiver": "Nowhere", "mode": "CW",
                               "frequency": "7030000"}, 400)
        assert json.loads(get("/data/slices?receiver=Desk%20Archive")) == []
        assert b"configuration changed" in post(
            "/receiver-edit", {**add, "action": "delete", "original": "Desk Archive"}, 400)
        delete_path = "/receiver-edit?" + urlencode({"delete": "Desk Archive"})
        assert b"Confirm Delete" in get(delete_path)
        assert b"No IQ sources configured." in post(
            "/receiver-edit", {**form(delete_path), "action": "delete",
                               "original": "Desk Archive"})
    finally:
        _, logs, graceful = stop(proc)
    assert "[controller] Station started with" in logs
    assert f"[web] Listening on http://127.0.0.1:{port}" in logs
    if graceful:
        assert "[controller] Station stopped" in logs
