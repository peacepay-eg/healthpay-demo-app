"""Makes the box's Arabic voice clips (16 kHz, 8-bit unsigned, mono, raw).

Runs on the GitHub build machine: python tools/make_voice.py data
v0.12.2: a natural Egyptian female voice, Microsoft's neural "ar-EG-SalmaNeural"
(through edge-tts; demo use only: production uses a licensed voice or a recorded
voice actor). Falls back to Piper ("ar_JO-kareem-medium"), then espeak-ng.
Each clip is trimmed, shaped for a small speaker (low cut, gentle compression)
and its loudness evened out so "received 1,250 pounds" sounds like one voice.
Writes rate.txt (the box plays at that rate) and sample.wav to listen to first.
"""
import math
import os
import struct
import subprocess
import sys
import urllib.request
import wave

OUT = sys.argv[1] if len(sys.argv) > 1 else "data"
RATE = 16000
EDGE_VOICE = "ar-EG-SalmaNeural"
MAX_TOTAL = 2_150_000  # the box's file area is 2.4 MB
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


class Edge:
    """Microsoft's neural voices through edge-tts (needs the internet)."""
    def __init__(self):
        import edge_tts  # noqa
        self.edge_tts = edge_tts
        self.say("تم", "edge_probe.wav")

    def say(self, text, path):
        import asyncio
        mp3 = path + ".mp3"
        async def run():
            await self.edge_tts.Communicate(text.replace("\u0640", ""), EDGE_VOICE, rate="+5%").save(mp3)
        asyncio.run(run())
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", mp3, path], check=True)
        os.remove(mp3)


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
            "silenceremove=start_periods=1:start_threshold=-42dB:start_silence=0.02,areverse,"
            # a small speaker: cut the low hum it can't play, even out the loudness, soften the top
            "highpass=f=170,lowpass=f=%d,acompressor=threshold=-20dB:ratio=3:attack=5:release=80:makeup=4" % (RATE // 2 - 600))
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-af", trim, "-ar", str(RATE), "-ac", "1",
                          "-f", "s16le", "-"], check=True, capture_output=True).stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def to_u8(samples, peak_to=0.92, tail_ms=25):
    peak = max(1, max(abs(s) for s in samples)) if samples else 1
    g = peak_to * 32767 / peak
    # a tiny random dither hides the 8-bit graininess
    import random
    rnd = random.Random(7)
    out = bytearray(max(0, min(255, int(round(128 + s * g / 256 + rnd.uniform(-0.5, 0.5))))) for s in samples)
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
    global RATE
    os.makedirs(OUT, exist_ok=True)
    try:
        build()
    except SystemExit as e:
        if "too large" not in str(e):
            raise
        print("Too large at %d Hz; making the clips at 12 kHz" % RATE)
        RATE = 12000
        build()


def build():
    os.makedirs(OUT, exist_ok=True)
    engine = None
    for cls, nm in ((Edge, "edge " + EDGE_VOICE), (Piper, "piper"), (Espeak, "espeak-ng")):
        try:
            engine, name = cls(), nm
            break
        except Exception as e:  # noqa
            print("%s not available (%s)" % (nm, e))
    if engine is None:
        sys.exit("No voice engine")
    total = 0
    tmp = "clip_tmp.wav"
    made = {}
    for key, text in clips().items():
        engine.say(text, tmp)
        data = to_u8(to_pcm16(tmp))
        if len(data) > 48000:  # the box plays clips of up to 3 seconds
            data = data[:48000]
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
    if total > MAX_TOTAL:
        sys.exit("Voice clips too large for the flash")
    with open(os.path.join(OUT, "rate.txt"), "w") as f:
        f.write(str(RATE))
    # A whole phrase to listen to: received 1,250 pounds from ...620
    phrase = ["ding", "recv", "k1", "wa", "h200", "wa", "n50", "egp", "from", "n6", "n2", "d0"]
    with wave.open("sample.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(RATE)
        w.writeframes(b"".join(made[k] for k in phrase))


if __name__ == "__main__":
    main()
