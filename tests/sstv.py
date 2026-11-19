import array
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

# Synthesize a phase-continuous Martin 2 transmission with solid RGB scan lines.
mode = sys.argv[2] if len(sys.argv)>2 else "Martin2"
codes={"Martin1":44,"Martin2":40,"Scottie1":60,"Scottie2":56,"Robot36":8,"Robot72":12}
rate = 12000
with tempfile.TemporaryDirectory() as directory:
    proc = subprocess.Popen([sys.argv[1], "--sstv", "-", "14230000", str(rate),
                             directory], stdin=subprocess.PIPE)
    phase = 0.0
    end = 0.0
    produced = 0

    def tone(hz, seconds):
        global phase, end, produced
        end += seconds * rate
        size = round(end) - produced
        produced += size
        step = 2 * math.pi * hz / rate
        data = array.array("f")
        for i in range(size):
            angle = phase + step * i
            data.extend((math.cos(angle), math.sin(angle)))
        phase = (phase + size * step) % (2 * math.pi)
        proc.stdin.write(data.tobytes())

    for hz, seconds in [(1900, .3), (1200, .01), (1900, .3), (1200, .03)]:
        tone(hz, seconds)
    code = codes[mode]
    for bit in range(8):
        tone(1100 if (code | ((code.bit_count()%2)<<7)) & (1 << bit) else 1300, .03)
    tone(1200, .03)
    height=240 if mode.startswith("Robot") else 256
    if mode.startswith("Scottie"): tone(1200,.009)
    for row in range(height):
        if mode.startswith("Martin"):
            tone(1200, .004862)
            for hz in (2300, 1500, 1900):
                tone(1500, .000572)
                tone(hz, .073216 if code==40 else .146432)
            tone(1500, .000572)
        elif mode.startswith("Scottie"):
            for c,hz in enumerate((2300,1500,1900)):
                if c==2: tone(1200,.009)
                tone(1500,.0015);tone(hz,.13824 if code==60 else .088064)
        else:
            tone(1200,.009);tone(1500,.003);tone(1900,.088 if code==8 else .138)
            channels=[row%2] if code==8 else [0,1]
            for c in channels:
                tone(1500 if c==0 else 2300,.0045);tone(1900,.0015)
                tone(1900,.044 if code==8 else .069)
    tone(1500, .05)
    proc.stdin.close()
    assert proc.wait(timeout=10) == 0
    image, = Path(directory).glob(f"*-{mode}.png")
    data = image.read_bytes()
    assert struct.unpack(">II", data[16:24]) == (320, height)
    offset = 8
    compressed = bytearray()
    while offset < len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        if kind == b"IDAT":
            compressed.extend(data[offset + 8:offset + 8 + length])
        offset += 12 + length
    first = zlib.decompress(compressed)[:961]
    # On row zero the PNG predictors reduce to zero, left, or half-left.
    pixels = bytearray(first[1:])
    for i in range(3, len(pixels)):
        left = pixels[i - 3]
        predictor = {0: 0, 1: left, 2: 0, 3: left // 2, 4: left}[first[0]]
        pixels[i] = (pixels[i] + predictor) % 256
    r, g, b = pixels[480:483]
    if mode.startswith("Robot"):
        assert all(120<=v<=135 for v in (r,g,b)),(r,g,b)
    else:
        assert 120 <= r <= 135 and g >= 250 and b <= 5, (r, g, b)
    if len(sys.argv) > 3:
        destination = Path(sys.argv[3])
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, destination / image.name)
