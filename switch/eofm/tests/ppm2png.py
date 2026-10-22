"""Converts the preview_*.ppm frames to PNG (stdlib only)."""
import glob, struct, sys, zlib


def ppm_to_png(src, dst):
    data = open(src, 'rb').read()
    head, px = data.split(b'\n', 1)  # "P6 <w> <h> 255" on one line
    w, h = int(head.split()[1]), int(head.split()[2])
    raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    open(dst, 'wb').write(png)


for f in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'preview_*.ppm')):
    ppm_to_png(f, f[:-4] + '.png')
    print(f[:-4] + '.png')
