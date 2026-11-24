"""Dev helper: send a state word, then grab a screenshot of the Stick's screen over USB.
usage: python tools/shot.py /dev/cu.usbserial-XXXX done out.png
"""
import sys, time
import serial
from PIL import Image

port, word, out = sys.argv[1:4]
s = serial.Serial(None, 115200, timeout=10)
s.port = port
s.dtr = s.rts = False  # set before open so opening the port doesn't reset the ESP32
s.open()
time.sleep(8)  # macOS FTDI driver may still reset on open; let it boot and join WiFi
s.reset_input_buffer()
s.write(f"{word}\n".encode())
time.sleep(2.5)  # some words beep for ~1.6 s before the next redraw
s.reset_input_buffer()
s.write(b"shot\n")
while True:
    line = s.readline().decode(errors="replace").strip()
    if not line:
        sys.exit("no SHOT header")
    if line.startswith("SHOT "):
        break
w, h = map(int, line.split()[1:3])
raw = s.read(w * h * 2)
assert len(raw) == w * h * 2, f"short read {len(raw)}"
img = Image.new("RGB", (w, h))
px = img.load()
for i in range(w * h):
    v = (raw[2 * i] << 8) | raw[2 * i + 1]  # sprite buffer is big-endian RGB565
    px[i % w, i // w] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
img.resize((w * 3, h * 3), Image.NEAREST).save(out)
print("saved", out, w, h)
