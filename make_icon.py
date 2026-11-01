"""Generate mdview.ico: a page with a folded corner, an "M" and a down arrow.

16-colour (4bpp) images at 32x32 and 16x16 so any Win95 display depth shows them.
"""
import struct

# standard VGA palette indices
BLACK, SILVER, GRAY, NAVY, WHITE = 0, 7, 8, 4, 15
PALETTE = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128),
           (128, 0, 128), (0, 128, 128), (192, 192, 192), (128, 128, 128),
           (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255),
           (255, 0, 255), (0, 255, 255), (255, 255, 255)]


def page(size):
    """Return (pixels, transparent) grids for a page with the corner folded."""
    px = [[WHITE] * size for _ in range(size)]
    tr = [[True] * size for _ in range(size)]
    if size == 32:
        x0, x1, y0, y1, fold = 3, 27, 1, 30, 7
    else:
        x0, x1, y0, y1, fold = 1, 13, 0, 15, 4
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            d = x - (x1 - fold)                  # distance past the fold's left edge
            fy = y - y0
            if d > fy:                           # cut-off corner
                continue
            tr[y][x] = False
            edge = x in (x0, x1) or y in (y0, y1) or d == fy
            px[y][x] = BLACK if edge else WHITE
            if d > 0 and fy > d and not edge:     # inside the folded flap
                px[y][x] = SILVER
            if (fy == fold and d >= 0) or (d == 0 and fy <= fold):
                px[y][x] = BLACK
    return px, tr


def draw_mark(px, size):
    def dot(x, y, c=NAVY):
        px[y][x] = c

    if size == 32:
        for r in range(11):                      # stems of the M
            for x in (7, 8, 20, 21):
                dot(x, 9 + r)
        for r in range(6):                       # diagonals
            for x in (9 + r, 10 + r, 19 - r, 20 - r):
                dot(x, 9 + r)
        for y in range(21, 25):                  # arrow shaft
            dot(14, y); dot(15, y)
        for i, (a, b) in enumerate([(11, 18), (12, 17), (13, 16), (14, 15)]):
            for x in range(a, b + 1):
                dot(x, 25 + i)
    else:
        for r in range(6):
            dot(3, 4 + r); dot(11, 4 + r)
        for k in range(4):
            dot(4 + k, 4 + k); dot(10 - k, 4 + k)
        dot(7, 10); dot(7, 11)
        for x in (6, 7, 8):
            dot(x, 12)
        dot(7, 13)


def image_bytes(size):
    px, tr = page(size)
    draw_mark(px, size)
    stride4 = ((size * 4 + 31) // 32) * 4
    stride1 = ((size + 31) // 32) * 4
    xor, andm = bytearray(), bytearray()
    for y in range(size - 1, -1, -1):            # bottom-up
        row = bytearray(stride4)
        for x in range(size):
            c = 0 if tr[y][x] else px[y][x]
            row[x // 2] |= (c << 4) if x % 2 == 0 else c
        xor += row
        mrow = bytearray(stride1)
        for x in range(size):
            if tr[y][x]:
                mrow[x // 8] |= 0x80 >> (x % 8)
        andm += mrow
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 4, 0,
                         len(xor) + len(andm), 0, 0, 16, 16)
    pal = b"".join(struct.pack("BBBB", b, g, r, 0) for r, g, b in PALETTE)
    return header + pal + bytes(xor) + bytes(andm)


def main():
    images = [image_bytes(32), image_bytes(16)]
    sizes = [32, 16]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, s in zip(images, sizes):
        out += struct.pack("<BBBBHHII", s, s, 16, 0, 1, 4, len(img), offset)
        offset += len(img)
    out += b"".join(images)
    with open("mdview.ico", "wb") as f:
        f.write(out)
    print("wrote mdview.ico,", len(out), "bytes")


if __name__ == "__main__":
    main()
