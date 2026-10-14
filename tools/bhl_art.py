#!/usr/bin/env python3
"""bhl_art.py <bhl mod folder> <art folder>

The launch window's and the icon's art, made from the mod's own files (never stored in this project):
  backdrop.png  the main menu's background, assembled from resource/BackgroundLayout.txt's tiles (1920 x 1080)
  shot.png      chapter thumbnails from gfx/vgui/vanchap_*.tga side by side, for the launch window's banner
  logo.png      the largest image in game.ico (the mod's icon), when it has one of at least 64 pixels
Only the standard library: TGA (uncompressed or run-length, 24/32-bit), ICO (PNG or 32-bit DIB) and PNG output.
"""
import os, struct, sys, zlib


def tga_read(path):
    """width, height, RGBA rows (top row first)"""
    d = open(path, 'rb').read()
    idlen, cmtype, itype = d[0], d[1], d[2]
    w, h, bpp, desc = struct.unpack_from('<HHBB', d, 12)
    if cmtype or itype not in (2, 10) or bpp not in (24, 32):
        raise ValueError(f'{path}: unsupported TGA (type {itype}, {bpp} bits)')
    i = 18 + idlen; n = w * h; step = bpp // 8; px = bytearray()
    if itype == 2:
        px = d[i:i + n * step]
    else:
        while len(px) < n * step:
            c = d[i]; i += 1; count = (c & 0x7f) + 1
            if c & 0x80: px += d[i:i + step] * count; i += step
            else: px += d[i:i + count * step]; i += count * step
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            p = px[(y * w + x) * step:(y * w + x + 1) * step]
            r += bytes((p[2], p[1], p[0], p[3] if step == 4 else 255))
        rows.append(bytes(r))
    if not desc & 0x20: rows.reverse()            # bottom-up unless the descriptor says top-down
    return w, h, rows


def png_write(path, w, h, rows):
    def chunk(t, dat): return struct.pack('>I', len(dat)) + t + dat + struct.pack('>I', zlib.crc32(t + dat) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def find(folder, rel):
    """a file in the mod folder, its spelling matched case-insensitively"""
    path = folder
    for part in rel.replace('\\', '/').split('/'):
        try: names = os.listdir(path)
        except OSError: return None
        match = next((n for n in names if n.lower() == part.lower()), None)
        if not match: return None
        path = os.path.join(path, match)
    return path


def backdrop(mod, out):
    layout = find(mod, 'resource/BackgroundLayout.txt')
    if not layout: return False
    W, H, tiles = 0, 0, []
    for line in open(layout, encoding='latin1'):
        f = line.split()
        if len(f) == 3 and f[0] == 'resolution': W, H = int(f[1]), int(f[2])
        elif len(f) >= 4 and f[0].lower().endswith('.tga'): tiles.append((f[0], int(f[-2]), int(f[-1])))
    if not W or not tiles: return False
    canvas = [bytearray(W * 4) for _ in range(H)]
    for name, x0, y0 in tiles:
        path = find(mod, name)
        if not path: continue
        w, h, rows = tga_read(path)
        for y in range(min(h, H - y0)):
            n = min(w, W - x0)
            canvas[y0 + y][x0 * 4:(x0 + n) * 4] = rows[y][:n * 4]
    png_write(out, W, H, [bytes(r) for r in canvas])
    return True


def shot(mod, out, chapters=(1, 5, 9, 13)):
    """a strip of chapter thumbnails: a glimpse of the campaign in the banner"""
    images = []
    for n in chapters:
        path = find(mod, f'gfx/vgui/vanchap_{n}.tga')
        if path: images.append(tga_read(path))
    if not images: return False
    h = min(i[1] for i in images); gap = 4
    w = sum(i[0] for i in images) + gap * (len(images) - 1)
    rows = []
    for y in range(h):
        r = bytearray()
        for k, (iw, ih, irows) in enumerate(images):
            if k: r += b'\0\0\0\xff' * gap
            r += irows[y][:iw * 4]
        rows.append(bytes(r))
    png_write(out, w, h, rows)
    return True


def logo(mod, out):
    path = find(mod, 'game.ico')
    if not path: return False
    d = open(path, 'rb').read()
    _, kind, count = struct.unpack_from('<HHH', d, 0)
    if kind != 1: return False
    best = None
    for k in range(count):
        bw, bh, _, _, _, bpp, size, off = struct.unpack_from('<BBBBHHII', d, 6 + 16 * k)
        dim = bw or 256
        if not best or dim > best[0]: best = (dim, size, off)
    if not best or best[0] < 64: return False
    data = d[best[2]:best[2] + best[1]]
    if data.startswith(b'\x89PNG'):
        open(out, 'wb').write(data); return True
    hsize, w, h2, _, bpp = struct.unpack_from('<IiiHH', data, 0)
    if bpp != 32: return False
    h = h2 // 2; px = data[hsize:hsize + w * h * 4]
    rows = [bytes(b for x in range(w) for b in (px[(y * w + x) * 4 + 2], px[(y * w + x) * 4 + 1], px[(y * w + x) * 4], px[(y * w + x) * 4 + 3]))
            for y in range(h)]
    rows.reverse()                                  # DIB rows are bottom-up
    png_write(out, w, h, rows)
    return True


def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    mod, art = sys.argv[1], sys.argv[2]
    os.makedirs(art, exist_ok=True)
    made = [name for name, fn in (('backdrop.png', backdrop), ('shot.png', shot), ('logo.png', logo))
            if fn(mod, os.path.join(art, name))]
    if not made: sys.exit('no launch-window art found in the mod folder')
    print(', '.join(made))


if __name__ == '__main__':
    main()
