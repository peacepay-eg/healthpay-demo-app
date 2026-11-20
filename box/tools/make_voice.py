"""Makes the box's Arabic voice clips (12 kHz, 8-bit unsigned, mono, raw).

Runs on the GitHub build machine: python tools/make_voice.py data
Uses the Piper voice "ar_JO-kareem-medium" (open licence); falls back to
espeak-ng if Piper is not available. Each clip is trimmed and its loudness
evened out so the phrase "received 1,250 pounds" sounds like one voice.
Also writes sample.wav (a whole phrase) so a person can listen before flashing.
"""
import math
import os
import struct
import subprocess
import sys
import urllib.request
import wave

OUT = sys.argv[1] if len(sys.argv) > 1 else "data"
RATE = 12000
SPEED = 0.9  # a little faster than normal speech
VOICE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main/ar/ar_JO/kareem/medium/"
VOICE = "ar_JO-kareem-medium.onnx"

UNITS = ["", "واحِد", "اِثْنان", "ثَلاثَة", "أَرْبَعَة", "خَمْسَة", "سِتَّة", "سَبْعَة", "ثَمانِيَة", "تِسْعَة"]
TEENS = ["عَشَرَة", "أَحَدَ عَشَر", "اِثْنا عَشَر", "ثَلاثَةَ عَشَر", "أَرْبَعَةَ عَشَر", "خَمْسَةَ عَشَر",
         "سِتَّةَ عَشَر", "سَبْعَةَ عَشَر", "ثَمانِيَةَ عَشَر", "تِسْعَةَ عَشَر"]
TENS = {20: "عِشْرون", 30: "ثَلاثون", 40: "أَرْبَعون", 50: "خَمْسون", 60: "سِتّون", 70: "سَبْعون",
        80: "ثَمانون", 90: "تِسْعون"}
HUNDREDS = {100: "مِئَة", 200: "مِئَتان", 300: "ثَلاثُمِئَة", 400: "أَرْبَعُمِئَة", 500: "خَمْسُمِئَة",
            600: "سِتُّمِئَة", 700: "سَبْعُمِئَة", 800: "ثَمانِمِئَة", 900: "تِسْعُمِئَة"}


def number_text(n):
    if n < 10:
        return UNITS[n]
    if n < 20:
        return TEENS[n - 10]
    u, t = n % 10, n - n % 10
    return TENS[t] if not u else UNITS[u] + " وَ" + TENS[t]


def clips():
    c = {"n%d" % n: number_text(n) for n in range(1, 100)}
    c.update({"h%d" % h: t for h, t in HUNDREDS.items()})
    c.update({
        "k1": "أَلْف", "k2": "أَلْفان", "kpl": "آلاف", "kacc": "أَلْفًا", "wa": "وَ",
        "recv": "تَمَّ اسْتِلام", "egp": "جُنَيْه", "from": "مِن رَقَم يَنْتَهي بِـ", "d0": "صِفْر",
        "left": "باقي", "notpaid": "لَمْ يَدْفَعْ", "allpaid": "الكُلُّ دَفَع",
    })
    return c


class Piper:
    def __init__(self):
        from piper import PiperVoice  # noqa
        for f in (VOICE, VOICE + ".json"):
            if not os.path.exists(f):
                urllib.request.urlretrieve(VOICE_URL + f, f)
        self.voice = PiperVoice.load(VOICE)

    def say(self, text, path):
        with wave.open(path, "wb") as w:
            if hasattr(self.voice, "synthesize_wav"):
                try:
                    from piper import SynthesisConfig
                    self.voice.synthesize_wav(text, w, syn_config=SynthesisConfig(length_scale=SPEED))
                except ImportError:
                    self.voice.synthesize_wav(text, w)
            else:
                self.voice.synthesize(text, w, length_scale=SPEED)


class Espeak:
    def say(self, text, path):
        subprocess.run(["espeak-ng", "-v", "ar", "-s", "165", "-w", path, text], check=True)


def to_pcm16(path):
    """Trim silence at both ends and resample to 16 kHz mono 16-bit."""
    trim = ("silenceremove=start_periods=1:start_threshold=-42dB:start_silence=0.02,areverse,"
            "silenceremove=start_periods=1:start_threshold=-42dB:start_silence=0.02,areverse")
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-af", trim, "-ar", str(RATE), "-ac", "1",
                          "-f", "s16le", "-"], check=True, capture_output=True).stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def to_u8(samples, peak_to=0.92, tail_ms=40):
    peak = max(1, max(abs(s) for s in samples)) if samples else 1
    g = peak_to * 32767 / peak
    out = bytearray(max(0, min(255, int(round(128 + s * g / 256)))) for s in samples)
    out += bytes([128]) * int(RATE * tail_ms / 1000)
    return bytes(out)


def ding():
    out = []
    for f, ms in ((988, 110), (1319, 170)):
        n = int(RATE * ms / 1000)
        for i in range(n):
            env = min(1, i / 80) * math.exp(-3.0 * i / n)
            out.append(int(9000 * env * math.sin(2 * math.pi * f * i / RATE)))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    try:
        engine, name = Piper(), "piper"
    except Exception as e:  # noqa
        print("Piper not available (%s); using espeak-ng" % e)
        engine, name = Espeak(), "espeak-ng"
    total = 0
    tmp = "clip_tmp.wav"
    made = {}
    for key, text in clips().items():
        engine.say(text, tmp)
        data = to_u8(to_pcm16(tmp))
        if len(data) > 40000:  # the box plays clips of up to 3.3 seconds
            data = data[:40000]
        with open(os.path.join(OUT, key + ".u8"), "wb") as f:
            f.write(data)
        made[key] = data
        total += len(data)
    d = to_u8(ding(), peak_to=0.6, tail_ms=60)
    with open(os.path.join(OUT, "ding.u8"), "wb") as f:
        f.write(d)
    made["ding"] = d
    total += len(d)
    print("voice: %s, %d clips, %d bytes" % (name, len(made), total))
    if total > 2_000_000:
        sys.exit("Voice clips too large for the flash")
    # A whole phrase to listen to: received 1,250 pounds from ...620
    phrase = ["ding", "recv", "k1", "wa", "h200", "wa", "n50", "egp", "from", "n6", "n2", "d0"]
    with wave.open("sample.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(RATE)
        w.writeframes(b"".join(made[k] for k in phrase))


if __name__ == "__main__":
    main()
