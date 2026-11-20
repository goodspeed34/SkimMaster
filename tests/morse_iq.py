"""Keyed CW as interleaved float32 I/Q at 48 kHz, 22 WPM."""
import array
import math

MORSE = dict(A=".-", B="-...", C="-.-.", D="-..", E=".", F="..-.", G="--.",
             H="....", I="..", J=".---", K="-.-", L=".-..", M="--", N="-.",
             O="---", P=".--.", Q="--.-", R=".-.", S="...", T="-", U="..-",
             V="...-", W=".--", X="-..-", Y="-.--", Z="--..")
MORSE.update({str(d): s for d, s in enumerate(
    ["-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...",
     "---..", "----."])})
RATE = 48000
DOT = round(RATE * 1.2 / 22)


def keyed(text, offset_hz):
    tone = array.array("f")
    for i in range(DOT):
        phase = math.tau * offset_hz * i / RATE
        tone.extend((.4 * math.cos(phase), .4 * math.sin(phase)))
    quiet = array.array("f", bytes(DOT * 8))
    out = quiet * 15
    for word in text.split():
        for ch in word:
            for mark in MORSE[ch]:
                out += tone * (1 if mark == "." else 3) + quiet
            out += quiet * 2
        out += quiet * 4
    return out + quiet * 50
