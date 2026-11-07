"""Champions of Norrath <LEVEL>.TEX textures, decoded for inspection and to check runtime/con_textures.cpp.

  python3 -I con_tex.py png <LEVEL.TEX> <cell id> <texture index> <out.png>
  python3 -I con_tex.py hash <LEVEL.TEX> [every]     every Nth texture, in the format of con-tex-check

Layout (read from SLUS_205.65: textureInitTexture 0x16d278, vqDecompress 0x1712f8, processHuffman 0x1711d0,
texDecompressThread 0x16f8b8):
- file: u32 0, then (s32 cell id, u32 offset, u32 size) records ended by id -1 or -2; cell: u32 count, 0x40-byte headers
  from 0x40 (u16 width, u16 height, u16, u16, u16 flags, u16, u32, u32 payload offset, ...).

Flags 0x10d, VQ:
- payload: u32 offset of the shared block, then the texture's bitstream.
- shared block: u16 colours, u16 vectors, colours x u32 palette in index order, two canonical Huffman tables (u32
  shortest length, then (u32 left-aligned limit, s32 first index) pairs ended by index -1), then a bitstream of vectors x
  4 symbols (table 1): the codebook of 2x2 texel vectors.
- texture bitstream: rectangles of 16x16 texel blocks, fields x0, y0, x1, y1 (bit lengths of the block column and row
  counts), ranges [x0, x1) x [y0, y1), ended by x1 < x0; per block 1 bit (1: a 24-bit reference, byte offset in the
  cell << 3 | bit offset, to an identical block coded with the same codebook: its own flag bit is skipped), then vectors
  (table 2) for each 2x2 texel square, rows of the block top down.
- bitstreams: 64-bit buffer filled MSB first one byte at a time.

Flags 0x10c, tiles (3 levels: HATEEX1, HATEIN1, ELFCUT):
- payload: u32 offset of the shared block, then rectangles: s8 x0, y0, x1, y1 (x0 == -1 ends the list), inclusive, in
  16x16 texel blocks, and u32 stream offset per block, rows outer.
- shared block: 0x400 palette, GS CLUT order (bits 3 and 4 of the index exchanged), 0x800 successor table (8 bytes per
  texel value), 0xc00 u32 n, u16 symbols[n], s32 S[18], s32 T[17]: canonical code of lengths 1 to 16, top k bits belong
  to length k when T[k] >= them, symbol = symbols[bits + S[k]].
- stream: 16-bit little-endian words, bits MSB first, 256 symbols, one per texel in raster order: below 0x100 the texel,
  0x100-0x104 the texel at -1, -16, -17, -15, -2, 0x105-0x10c successor[previous texel][symbol - 0x105].
"""
import struct
import sys
import zlib


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def s32(d, o):
    return struct.unpack_from('<i', d, o)[0]


class Bits:
    def __init__(self, d, o):
        self.d, self.o, self.buf, self.n = d, o, 0, 0

    def fill(self):
        while self.n < 32:
            byte = self.d[self.o] if self.o < len(self.d) else 0
            self.o += 1
            self.buf |= byte << (56 - self.n)
            self.n += 8

    def peek32(self):
        self.fill()
        return (self.buf >> 32) & 0xFFFFFFFF

    def skip(self, k):
        self.buf = (self.buf << k) & 0xFFFFFFFFFFFFFFFF
        self.n -= k

    def raw(self, k):
        if k == 0:
            return 0
        value = self.peek32() >> (32 - k)
        self.skip(k)
        return value


class Huffman:
    def __init__(self, d, o):
        self.shortest = s32(d, o)
        o += 4
        self.levels = []
        while True:
            limit, index = u32(d, o), s32(d, o + 4)
            o += 8
            if index == -1:
                break
            self.levels.append((limit, index))
        self.end = o

    def decode(self, bits):
        window = bits.peek32()
        level = 0
        for i, (limit, _) in enumerate(self.levels):
            if limit <= window:
                level = i
        limit, index = self.levels[level]
        length = self.shortest + level
        bits.skip(length)
        return index + ((window - limit) >> (32 - length))


def vq_shared(cell, o):
    colours, vectors = struct.unpack_from('<HH', cell, o)
    palette = struct.unpack_from('<%dI' % colours, cell, o + 4)
    table1 = Huffman(cell, o + 4 + 4 * colours)
    table2 = Huffman(cell, table1.end)
    bits = Bits(cell, table2.end)
    codebook = [tuple(table1.decode(bits) for _ in range(4)) for _ in range(vectors)]
    return palette, table2, codebook


def field_bits(count):
    return max(1, count.bit_length())


def decode_vq(cell, width, height, payload, shared):
    palette, table2, codebook = shared
    bx, by = field_bits((width + 15) // 16), field_bits((height + 15) // 16)
    bits = Bits(cell, payload + 4)
    texels = [0] * (width * height)
    while True:
        x0, y0, x1, y1 = bits.raw(bx), bits.raw(by), bits.raw(bx), bits.raw(by)
        if x1 < x0 or (x1 == x0 and y1 <= y0):
            return texels, palette
        for block_y in range(y0, y1):
            for block_x in range(x0, x1):
                left, top = block_x * 16, block_y * 16
                right, bottom = min(left + 16, width), min(top + 16, height)
                source = bits
                if bits.raw(1):
                    reference = bits.raw(24)
                    source = Bits(cell, reference >> 3)
                    source.raw(reference & 7)
                    source.raw(1)
                for y in range(top, bottom, 2):
                    for x in range(left, right, 2):
                        vector = codebook[table2.decode(source)]
                        for k, (dx, dy) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
                            if x + dx < right and y + dy < bottom:
                                texels[(y + dy) * width + x + dx] = vector[k]


def clut_order(i):
    return (i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1)


def tile_shared(cell, o):
    stored = struct.unpack_from('<256I', cell, o)
    palette = [stored[clut_order(i)] for i in range(256)]
    successors = cell[o + 0x400:o + 0xc00]
    n = u32(cell, o + 0xc00)
    symbols = struct.unpack_from('<%dH' % n, cell, o + 0xc04)
    first = struct.unpack_from('<18i', cell, o + 0xc04 + 2 * n)
    last = struct.unpack_from('<17i', cell, o + 0xc04 + 2 * n + 72)
    return palette, successors, symbols, first, last


def decode_tile(cell, o, shared):
    _, successors, symbols, first, last = shared
    out = bytearray(256)
    position, previous = 0, 0
    for i in range(256):
        at = o + (position >> 4) * 2
        high, low = struct.unpack_from('<HH', cell, at)
        top = ((((high << 16) | low) << (position & 15)) & 0xFFFFFFFF) >> 16
        k = 1
        while k <= 16 and last[k] < top >> (16 - k):
            k += 1
        symbol = symbols[(top >> (16 - k)) + first[k]]
        position += k
        if symbol < 0x100:
            value = symbol
        elif symbol < 0x105:
            value = out[i + (-1, -16, -17, -15, -2)[symbol - 0x100]]
        else:
            value = successors[previous * 8 + symbol - 0x105]
        out[i] = value
        previous = value
    return out


def decode_tiles(cell, width, height, payload, shared):
    texels = [0] * (width * height)
    o = payload + 4
    while cell[o] != 0xFF:
        x0, y0, x1, y1 = struct.unpack_from('<4b', cell, o)
        o += 4
        for block_y in range(y0, y1 + 1):
            for block_x in range(x0, x1 + 1):
                tile = decode_tile(cell, u32(cell, o), shared)
                o += 4
                for y in range(block_y * 16, min((block_y + 1) * 16, height)):
                    for x in range(block_x * 16, min((block_x + 1) * 16, width)):
                        texels[y * width + x] = tile[(y & 15) * 16 + (x & 15)]
    return texels, shared[0]


def decode_texture(cell, index, cache=None):
    header = 0x40 * (index + 1)
    width, height = struct.unpack_from('<2H', cell, header)
    flags = struct.unpack_from('<H', cell, header + 8)[0]
    payload = u32(cell, header + 0x10)
    offset = u32(cell, payload)
    cache = {} if cache is None else cache
    vq = flags & 1
    if offset not in cache:
        cache[offset] = vq_shared(cell, offset) if vq else tile_shared(cell, offset)
    texels, palette = (decode_vq if vq else decode_tiles)(cell, width, height, payload, cache[offset])
    return width, height, [palette[t] for t in texels]


def cells(tex):
    o = 4
    while True:
        cid, off, size = struct.unpack_from('<iII', tex, o)
        if cid in (-1, -2):
            return
        yield cid, tex[off:off + size]
        o += 12


def fnv(rgba):
    value = 0xCBF29CE484222325
    for byte in struct.pack('<%dI' % len(rgba), *rgba):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def write_png(path, width, height, rgba):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for c in rgba[y * width:(y + 1) * width]:
            raw += bytes((c & 255, (c >> 8) & 255, (c >> 16) & 255))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))


def main(argv):
    with open(argv[2], 'rb') as f:
        tex = f.read()
    if argv[1] == 'png':
        cell = dict(cells(tex))[int(argv[3], 0)]
        width, height, rgba = decode_texture(cell, int(argv[4], 0))
        write_png(argv[5], width, height, rgba)
        return
    every = int(argv[3]) if len(argv) > 3 else 1
    seen = 0
    for cid, cell in cells(tex):
        cache = {}
        for i in range(u32(cell, 0)):
            if seen % every == 0:
                width, height, rgba = decode_texture(cell, i, cache)
                print('%d %d %d %d %016x' % (cid, i, width, height, fnv(rgba)), flush=True)
            seen += 1


if __name__ == '__main__':
    main(sys.argv)
