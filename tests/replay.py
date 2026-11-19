import cmath
import math
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory) / "tones.iq"
    segments = [(13, 7000, 708), (950, 4800, 601)]
    expected = []
    with fixture.open("w") as stream:
        for start, tone, length in segments:
            expected.append((start, (length - 65) // 4 + 1, 14070000 + tone))
            for index in range(start, start + length):
                phase = 2j * math.pi * index / 48000
                # Strong out-of-band interferer would alias without filtering.
                sample = cmath.exp(phase * tone) + 3 * cmath.exp(phase * 22000)
                print(index, sample.real, sample.imag, file=stream)
    command = [sys.argv[1], "--replay", str(fixture)]
    lines = subprocess.check_output(command, text=True).splitlines()
    assert len(lines) == len(expected), lines
    for line, (start, count, frequency) in zip(lines, expected):
        first, size, hz = line.split()
        assert (int(first), int(size)) == (start, count), line
        assert abs(float(hz) - frequency) < 1, line
    fixture.write_text("5 1 0\n4 1 0\n")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode and "line 2" in result.stderr
