"""Draws docs/ym2413_pinout.png: the YM2413 (DIP-18, top view) with the Arduino Mega pin each leg
goes to, in the same colours as the web pages. Re-run it if the pin map in the firmware changes.

  python tools/make_pinout.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 1700, 1380
BG, PANEL, LINE = "#121211", "#1c1b18", "#3a382f"
CREAM, DIM = "#ebe5d3", "#938d7a"
GOLD, GREEN, CYAN, LAV = "#a8944a", "#2f9a68", "#58d3c8", "#8b8fb3"

FONTS = r"C:\Windows\Fonts"
def font(name, size):
    try:
        return ImageFont.truetype(os.path.join(FONTS, name), size)
    except OSError:
        return ImageFont.load_default()
F_TITLE, F_SUB = font("segoeuib.ttf", 58), font("segoeui.ttf", 30)
F_PIN, F_NAME, F_MEGA, F_NOTE = font("segoeuib.ttf", 28), font("segoeuib.ttf", 38), font("segoeuib.ttf", 37), font("segoeui.ttf", 27)
F_LEG = font("segoeui.ttf", 28)

# pin: (name, mega pin or None, note, kind)   kind: data / ctrl / gnd / none
PINS = {
    1: ("GND", "GND", "any board ground", "gnd"),
    2: ("D2", "D6", "data bit 2", "data"),
    3: ("D3", "D7", "data bit 3", "data"),
    4: ("D4", "D8", "data bit 4", "data"),
    5: ("D5", "D9", "data bit 5", "data"),
    6: ("D6", "D10", "data bit 6", "data"),
    7: ("D7", "D23", "data bit 7 (moved from D11)", "data"),
    8: ("XIN", None, "crystal, leave alone", "none"),
    9: ("XOUT", None, "crystal, leave alone", "none"),
    10: ("A0", "D12", "address / data select", "ctrl"),
    11: ("/WE", None, "tied low on the PSS board", "none"),
    12: ("/CS", "D3", "write strobe", "ctrl"),
    13: ("/IC", "D13", "chip reset", "ctrl"),
    14: ("MO", None, "audio out, leave alone", "none"),
    15: ("RO", None, "audio out, leave alone", "none"),
    16: ("VCC", None, "5 V from the PSS, leave alone", "none"),
    17: ("D0", "D4", "data bit 0", "data"),
    18: ("D1", "D5", "data bit 1", "data"),
}
COLOR = {"data": GREEN, "ctrl": CYAN, "gnd": LAV, "none": "#55524a"}

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

def text(xy, s, f, fill, anchor="lm"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)

def dashed(x0, x1, y, fill, dash=14, gap=10, width=4):
    x = x0
    while x < x1:
        d.line([(x, y), (min(x + dash, x1), y)], fill=fill, width=width)
        x += dash + gap

text((70, 62), "YM2413 (OPLL)  ->  Arduino Mega", F_TITLE, CREAM)
text((70, 118), "DIP-18, top view, notch up.   Every Arduino pin is a Mega pin, all 5 V logic.", F_SUB, DIM)

# chip body
CX0, CX1, CY0, CY1 = 640, 1060, 215, 1135
d.rounded_rectangle([CX0, CY0, CX1, CY1], radius=18, fill=PANEL, outline=GOLD, width=5)
cx = (CX0 + CX1) // 2
d.pieslice([cx - 44, CY0 - 44, cx + 44, CY0 + 44], 0, 180, fill=BG, outline=GOLD, width=5)
text((cx, 640), "YM2413", font("segoeuib.ttf", 64), GOLD, "mm")
text((cx, 705), "OPLL", F_SUB, DIM, "mm")
text((cx - 8, CY0 + 70), "1", F_PIN, DIM, "rm")   # pin 1 marker beside the notch

STEP, Y0 = 100, 295
LEG, WIRE_END = 56, 470
def y_of(i):                        # i = 0..8 from the top
    return Y0 + i * STEP

for pin, (name, mega, note, kind) in PINS.items():
    left = pin <= 9
    i = (pin - 1) if left else (18 - pin)
    y = y_of(i)
    col = COLOR[kind]
    # leg
    if left:
        d.rectangle([CX0 - LEG, y - 13, CX0, y + 13], fill="#c9b366", outline="#17140a")
        text((CX0 - LEG // 2, y), str(pin), font("segoeuib.ttf", 24), "#17140a", "mm")
        text((CX0 + 26, y), name, F_NAME, CREAM, "lm")
    else:
        d.rectangle([CX1, y - 13, CX1 + LEG, y + 13], fill="#c9b366", outline="#17140a")
        text((CX1 + LEG // 2, y), str(pin), font("segoeuib.ttf", 24), "#17140a", "mm")
        text((CX1 - 26, y), name, F_NAME, CREAM, "rm")
    # wire to the Mega label
    if left:
        xa, xb = CX0 - LEG, WIRE_END
        box = [60, y - 33, WIRE_END - 30, y + 33]
    else:
        xa, xb = CX1 + LEG, W - WIRE_END
        box = [W - WIRE_END + 30, y - 33, W - 60, y + 33]
    if mega:
        if left: d.line([(xb, y), (xa, y)], fill=col, width=7)
        else:    d.line([(xa, y), (xb, y)], fill=col, width=7)
        d.rounded_rectangle(box, radius=14, fill=col)
        dark = kind != "data"
        text(((box[0] + box[2]) // 2, y - 12), "Mega " + mega, F_MEGA, "#0d0d0c" if dark else "white", "mm")
        text(((box[0] + box[2]) // 2, y + 22), note, font("segoeui.ttf", 20), "#0d0d0c" if dark else "#e9fff3", "mm")
    else:
        if left: dashed(xb, xa, y, col)
        else:    dashed(xa, xb, y, col)
        d.rounded_rectangle(box, radius=14, outline=col, width=3)
        text(((box[0] + box[2]) // 2, y - 10), "not connected", F_NOTE, DIM, "mm")
        text(((box[0] + box[2]) // 2, y + 22), note, font("segoeui.ttf", 20), DIM, "mm")

# legend + extra wires
ly = 1215
d.rounded_rectangle([60, ly - 40, W - 60, H - 36], radius=16, fill=PANEL, outline=LINE, width=2)
x = 100
for col, label in ((GREEN, "data bus D0-D7"), (CYAN, "control: A0, /CS, /IC"), (LAV, "ground"), ("#55524a", "leave alone")):
    d.rounded_rectangle([x, ly - 14, x + 44, ly + 14], radius=7, fill=col)
    text((x + 58, ly), label, F_LEG, CREAM)
    x += 58 + int(d.textlength(label, font=F_LEG)) + 60
text((100, ly + 62), "Not on the chip:  D2 <- CPU side of the cut /CS jumper    D22 -> 4.7k-10k -> CPU reset transistor    D18/D19 MIDI    D20/D21 OLED    5V pin <- supply", font("segoeui.ttf", 23), DIM)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "ym2413_pinout.png")
img.save(out, optimize=True)
print("wrote", os.path.abspath(out), img.size)
