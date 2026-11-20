"""A full station restart from the panel applies a listener change."""
import re
import socket
import sqlite3
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from contextlib import closing
from pathlib import Path

from process_control import popen, stop


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for(url, deadline=15):
    end = time.monotonic() + deadline
    while True:
        try:
            with urllib.request.urlopen(url, timeout=2) as page:
                return page.read().decode()
        except OSError:
            assert time.monotonic() < end, f"{url} did not come up"
            time.sleep(0.1)


server = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    first, second = free_port(), free_port()
    config = Path(tmp) / "station.toml"
    config.write_text(
        f'listen_address = "127.0.0.1"\nport = {first}\nfirst_run_wizard = false\n'
    )
    proc = popen([server, config])
    try:
        page = wait_for(f"http://127.0.0.1:{first}/restart-daemon")
        csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
        assert 'action="/restart-station"' in page
        # The listener only changes on a full restart.
        config.write_text(config.read_text().replace(str(first), str(second)))
        time.sleep(1)
        wait_for(f"http://127.0.0.1:{first}/system-summary", deadline=2)
        request = urllib.request.Request(
            f"http://127.0.0.1:{first}/restart-station",
            data=urllib.parse.urlencode({"csrf": csrf}).encode(),
        )
        with urllib.request.urlopen(request) as response:
            assert b"Station restart requested" in response.read()
        summary = wait_for(f"http://127.0.0.1:{second}/system-summary")
        assert "@controller" in summary
        assert proc.poll() is None, "the supervisor exited instead of restarting"
        with closing(sqlite3.connect(Path(tmp) / "station.sqlite")) as db:
            messages = [row[0] for row in db.execute(
                "SELECT message FROM system_log WHERE component='controller' "
                "ORDER BY rowid")]
        assert "Station restarting" in messages, messages
        assert sum(m.startswith("Station started") for m in messages) == 2, messages
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{first}/", timeout=1)
            raise AssertionError("the old listener is still serving")
        except OSError:
            pass
    finally:
        stop(proc, timeout=6)
