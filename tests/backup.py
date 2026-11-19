from pathlib import Path
from contextlib import closing
import sqlite3
import subprocess
import sys
import tempfile
import zipfile

exe, fixture = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    source, target = directory / "original.db", directory / "restored.db"
    archive = directory / "snapshot.zip"
    gallery = directory / "gallery"
    gallery.mkdir()
    subprocess.run([exe, "--decode", "FT8", fixture, "14074000", "0", "test", source],
                   check=True, capture_output=True)
    subprocess.run([exe, "--backup",source,gallery,archive],check=True)
    subprocess.run([exe, "--restore",archive,target,directory/"restored-gallery"],check=True)
    with closing(sqlite3.connect(target)) as con, con:
        assert con.execute("SELECT count(*) FROM spots WHERE callsign='R7IW'").fetchone()==(1,)
    invalid = directory / "invalid.zip"
    with zipfile.ZipFile(invalid,"w") as z:
        z.writestr("../outside","bad")
        z.writestr("manifest.json","{}")
    assert subprocess.run([exe,"--restore",invalid,target,gallery],capture_output=True).returncode
    with closing(sqlite3.connect(target)) as con, con:
        assert con.execute("SELECT count(*) FROM spots WHERE callsign='R7IW'").fetchone()==(1,)
    broken = directory / "missing-spot-constraint.sqlite"
    with closing(sqlite3.connect(broken)) as db, db:
        db.execute("CREATE TABLE spots(receiver,utc_ms,mode,frequency_hz,message,callsign,grid)")
    with zipfile.ZipFile(invalid, "w") as z:
        z.write(broken, "station.sqlite")
        z.writestr("manifest.json", '{"format":"SkimMaster","version":1}')
    assert subprocess.run([exe, "--restore", invalid, target, gallery], capture_output=True).returncode
    assert not list(directory.glob("*.pre-restore-*.zip"))
    with closing(sqlite3.connect(target)) as db, db:
        assert db.execute("SELECT count(*) FROM spots WHERE callsign='R7IW'").fetchone() == (1,)
    with (gallery / "123-Robot36.png").open("wb") as oversized:
        oversized.truncate((32 << 20) + 1)
    rejected = directory / "too-large.zip"
    result = subprocess.run([exe, "--backup", source, gallery, rejected], capture_output=True)
    assert result.returncode and b"size limit" in result.stderr and not rejected.exists()
