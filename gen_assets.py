#!/usr/bin/env python3
"""Generate Vita sce_sys PNG assets."""
import struct, zlib, os

def make_png(width, height, pixels):
    """Create PNG from RGBA pixel list, row-major."""
    def chunk(ctype, data):
        c = ctype + data
        crc = struct.pack('>I', zlib.crc32(c) & 0xffffffff)
        return struct.pack('>I', len(data)) + c + crc

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter None
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            raw.extend((r, g, b, a))

    compressed = zlib.compress(bytes(raw), 9)
    sig = b'\x89PNG\r\n\x1a\n'
    return sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', compressed) + chunk(b'IEND', b'')

base = os.path.join('C:', os.sep, 'Users', 'D', 'Documents', 'GitHub', 'yabausevita', 'sce_sys')
print(f'Output dir: {base}')
os.makedirs(base, exist_ok=True)
os.makedirs(os.path.join(base, 'livearea', 'contents'), exist_ok=True)

# --- icon0.png: 128x128 Saturn planet icon ---
w, h = 128, 128
pixels = []
for y in range(h):
    for x in range(w):
        border = x < 4 or x >= 124 or y < 4 or y >= 124
        inner = x < 8 or x >= 120 or y < 8 or y >= 120
        if border:
            pixels.append((0, 136, 255, 255))
        elif inner:
            pixels.append((0, 68, 136, 255))
        else:
            cx, cy = 64, 60
            rx2, ry2 = 38.0, 12.0
            dx, dy = (x - cx) / rx2, (y - cy) / ry2
            ring_val = dx * dx + dy * dy - 1.0
            in_ring = abs(ring_val) < 0.20 and y >= cy - 2
            dist = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            in_planet = dist < 22
            if in_planet and not in_ring:
                pixels.append((0, 100, 200, 255))
            elif in_ring:
                pixels.append((200, 220, 255, 255))
            else:
                pixels.append((0, 17, 34, 255))

path = os.path.join(base, 'icon0.png')
with open(path, 'wb') as f:
    f.write(make_png(w, h, pixels))
print(f'icon0.png: {os.path.getsize(path)} bytes')

# --- bg0.png: 840x512 gradient ---
w, h = 840, 512
pixels = []
for y in range(h):
    t = y / h
    g = int(30 * (1 - t) + 10 * t)
    b = int(80 * (1 - t) + 40 * t)
    row = [(0, g, b, 255)] * w
    pixels.extend(row)

path = os.path.join(base, 'bg0.png')
with open(path, 'wb') as f:
    f.write(make_png(w, h, pixels))
print(f'bg0.png: {os.path.getsize(path)} bytes')

# --- startup.png: 840x512 solid dark blue ---
w, h = 840, 512
pixels = [(0, 20, 60, 255)] * (w * h)
path = os.path.join(base, 'startup.png')
with open(path, 'wb') as f:
    f.write(make_png(w, h, pixels))
print(f'startup.png: {os.path.getsize(path)} bytes')

# --- LiveArea template.xml ---
template = '''<?xml version="1.0" encoding="UTF-8"?>
<livearea style="a1" content="start">
  <background>
    <image>bg0.png</image>
  </background>
  <gate>
    <image bg="0" x="390" y="200" w="60" h="112" align="center" base="start">startup.png</image>
  </gate>
  <frame id="frame1" multi="o" affix="10,10,10,10">
  </frame>
  <frame id="frame2" multi="u" affix="0,30,0,0" origin="bg">
  </frame>
</livearea>
'''
path = os.path.join(base, 'livearea', 'contents', 'template.xml')
with open(path, 'w') as f:
    f.write(template)
print(f'template.xml: {os.path.getsize(path)} bytes')

print('All assets generated successfully!')
