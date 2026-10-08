"""Plays every note through the Mega's synth path, records the PSS audio on a sound card, and
reports each note's measured pitch against the expected one (in cents).

  python pitch_check.py [first_note] [last_note]      (default 36..84)

Needs: the PSS audio out (mono is fine) into a sound-card input, the Mega parked, and the board
reachable over WiFi. Settings come from environment variables:

  PSS270_IP      board address (default pss270.local)
  PSS270_AUDIO   input device: a number from `python -m sounddevice`, or part of its name
                 (default: the system default input)
  PSS270_CH      input channel on that device, 1-based (default 1)

Expect 0 notes off. A flaky data wire shows up as random errors of exactly 128 pitch units.
"""
import os, sys, time, urllib.request
import numpy as np
import sounddevice as sd

ESP_IP = os.environ.get("PSS270_IP", "pss270.local")
RATE = 48000
CH = int(os.environ.get("PSS270_CH", "1")) - 1       # 0-based
NOTE_MS = 700
GAP_S = 0.6


def cmd(c, timeout=6):
    try:
        return urllib.request.urlopen(f"http://{ESP_IP}/cmd?c={c.replace(' ', '%20')}", timeout=timeout).read().decode()
    except Exception as e:
        return f"err {e}"


def pick_device():
    want = os.environ.get("PSS270_AUDIO", "")
    if not want:
        return sd.default.device[0]
    if want.isdigit():
        return int(want)
    for i, d in enumerate(sd.query_devices()):
        if want.lower() in d["name"].lower() and d["max_input_channels"] > CH:
            return i
    raise SystemExit(f"no input device matching '{want}' with {CH + 1} channels")


def fundamental(x, fs, fmin=50, fmax=2200):
    """Harmonic-product-spectrum pitch estimate with parabolic peak refinement."""
    x = x - x.mean()
    x = x * np.hanning(len(x))
    n = 1 << 18
    spec = np.abs(np.fft.rfft(x, n))
    hps = spec.copy()
    for h in (2, 3, 4):
        d = spec[::h]
        hps[:len(d)] *= d
    freqs = np.fft.rfftfreq(n, 1 / fs)
    lo, hi = int(fmin * n / fs), int(fmax * n / fs)
    k = lo + int(np.argmax(hps[lo:hi]))
    a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
    k += 0.5 * (a - c) / (a - 2 * b + c)
    return k * fs / n


def main():
    first = int(sys.argv[1]) if len(sys.argv) > 1 else 36
    last = int(sys.argv[2]) if len(sys.argv) > 2 else 84
    dev = pick_device()
    print("input:", sd.query_devices(dev)["name"], f"(dev {dev})")
    print(cmd("park").strip())
    # level check
    rec = sd.rec(int(0.5 * RATE), RATE, channels=CH + 1, device=dev, dtype="float32"); sd.wait()
    print(f"noise floor rms {np.sqrt(np.mean(rec**2)):.5f}")
    bad = 0
    print("note  expected  measured   cents   level")
    for note in range(first, last + 1):
        exp = 440.0 * 2 ** ((note - 69) / 12)
        n = int((NOTE_MS / 1000 + 0.5) * RATE)
        rec = sd.rec(n, RATE, channels=CH + 1, device=dev, dtype="float32")
        time.sleep(0.15)
        cmd(f"play {note} {NOTE_MS}")
        sd.wait()
        x = rec[:, CH]
        # the loudest 0.35 s window = the sustained note
        w = int(0.35 * RATE)
        env = np.convolve(np.abs(x), np.ones(w) / w, "valid")
        s = int(np.argmax(env))
        seg = x[s + int(0.05 * RATE): s + w]
        lvl = float(np.sqrt(np.mean(seg ** 2)))
        if lvl < 0.003:
            print(f"{note:4d}  {exp:8.1f}  (silent)   level {lvl:.4f}")
            bad += 1
            time.sleep(GAP_S); continue
        f = fundamental(seg, RATE)
        cents = 1200 * np.log2(f / exp)
        flag = "" if abs(cents) < 25 else ("   <-- off by %+.0f" % cents)
        if abs(cents) >= 25: bad += 1
        print(f"{note:4d}  {exp:8.1f}  {f:8.1f}  {cents:+6.0f}   {lvl:.3f}{flag}")
        time.sleep(GAP_S)
    print(f"\n{bad} of {last - first + 1} notes off")


if __name__ == "__main__":
    main()
