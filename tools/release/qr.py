"""A QR code for the server's address: byte mode, error correction M,
versions 1-10 (up to 213 bytes), after ISO/IEC 18004 and Project Nayuki's
reference encoder. Drawn in the terminal with half blocks or as SVG for the
computer's setup page."""

# Error correction codewords per block and blocks, versions 1-10 (index 0
# unused), level M.
ECC_PER_BLOCK = [0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26]
BLOCKS = [0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5]
FORMAT_M = 0


def _raw_modules(ver):
    n = (16 * ver + 128) * ver + 64
    if ver >= 2:
        align = ver // 7 + 2
        n -= (25 * align - 10) * align - 55
        if ver >= 7:
            n -= 36
    return n


def _data_codewords(ver):
    return _raw_modules(ver) // 8 - ECC_PER_BLOCK[ver] * BLOCKS[ver]


def _gf_mul(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11d)
        z ^= ((y >> i) & 1) * x
    return z


def _rs_divisor(degree):
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = _gf_mul(result[j], root)
            if j + 1 < degree:
                result[j] ^= result[j + 1]
        root = _gf_mul(root, 2)
    return result


def _rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for b in data:
        factor = b ^ result.pop(0)
        result.append(0)
        for i, coef in enumerate(divisor):
            result[i] ^= _gf_mul(coef, factor)
    return result


def _codewords(text):
    data = text.encode('utf-8')
    for ver in range(1, 11):
        count_bits = 8 if ver < 10 else 16
        if 4 + count_bits + 8 * len(data) <= _data_codewords(ver) * 8:
            break
    else:
        raise ValueError('too long for a QR code here: %r' % text)
    bits = [0, 1, 0, 0] + [(len(data) >> i) & 1 for i in range(count_bits - 1, -1, -1)]
    for b in data:
        bits += [(b >> i) & 1 for i in range(7, -1, -1)]
    capacity = _data_codewords(ver) * 8
    bits += [0] * min(4, capacity - len(bits))
    bits += [0] * (-len(bits) % 8)
    pad = 0xec
    while len(bits) < capacity:
        bits += [(pad >> i) & 1 for i in range(7, -1, -1)]
        pad ^= 0xec ^ 0x11
    words = [int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    # Blocks with their error correction, then interleaved.
    nblocks, ecc_len = BLOCKS[ver], ECC_PER_BLOCK[ver]
    raw = _raw_modules(ver) // 8
    short = nblocks - raw % nblocks
    short_len = raw // nblocks
    divisor = _rs_divisor(ecc_len)
    blocks, k = [], 0
    for i in range(nblocks):
        n = short_len - ecc_len + (0 if i < short else 1)
        dat = words[k:k + n]
        k += n
        ecc = _rs_remainder(dat, divisor)
        if i < short:
            dat = dat + [None]
        blocks.append(dat + ecc)
    out = []
    for i in range(len(blocks[0])):
        for block in blocks:
            if block[i] is not None:
                out.append(block[i])
    return ver, out


class QR:
    def __init__(self, text):
        ver, data = _codewords(text)
        self.size = size = ver * 4 + 17
        self.m = [[False] * size for _ in range(size)]
        self.fn = [[False] * size for _ in range(size)]
        self._function_patterns(ver)
        self._draw(data)
        best = None
        for mask in range(8):
            self._mask(mask)
            self._format(mask)
            p = self._penalty()
            if best is None or p < best[0]:
                best = (p, mask)
            self._mask(mask)
        self._mask(best[1])
        self._format(best[1])

    def _set(self, x, y, dark):
        self.m[y][x] = dark
        self.fn[y][x] = True

    def _function_patterns(self, ver):
        size = self.size
        for i in range(size):
            self._set(6, i, i % 2 == 0)
            self._set(i, 6, i % 2 == 0)
        for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < size and 0 <= y < size:
                        d = max(abs(dx), abs(dy))
                        self._set(x, y, d not in (2, 4))
        if ver >= 2:
            align = ver // 7 + 2
            step = (ver * 8 + align * 3 + 5) // (align * 4 - 4) * 2
            pos = [6] + sorted(size - 7 - i * step for i in range(align - 1))
            last = len(pos) - 1
            for i, a in enumerate(pos):
                for j, b in enumerate(pos):
                    if (i, j) in ((0, 0), (0, last), (last, 0)):
                        continue
                    for dy in range(-2, 3):
                        for dx in range(-2, 3):
                            self._set(a + dx, b + dy, max(abs(dx), abs(dy)) != 1)
        self._format(0)  # reserves the format areas
        if ver >= 7:
            rem = ver
            for _ in range(12):
                rem = (rem << 1) ^ ((rem >> 11) * 0x1f25)
            word = ver << 12 | rem
            for i in range(18):
                bit = (word >> i) & 1 == 1
                a, b = size - 11 + i % 3, i // 3
                self._set(a, b, bit)
                self._set(b, a, bit)

    def _format(self, mask):
        data = FORMAT_M << 3 | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        word = (data << 10 | rem) ^ 0x5412
        bit = [(word >> i) & 1 == 1 for i in range(15)]
        size = self.size
        for i in range(6):
            self._set(8, i, bit[i])
        self._set(8, 7, bit[6])
        self._set(8, 8, bit[7])
        self._set(7, 8, bit[8])
        for i in range(9, 15):
            self._set(14 - i, 8, bit[i])
        for i in range(8):
            self._set(size - 1 - i, 8, bit[i])
        for i in range(8, 15):
            self._set(8, size - 15 + i, bit[i])
        self._set(8, size - 8, True)

    def _draw(self, data):
        size, i = self.size, 0
        right = size - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(size):
                for j in range(2):
                    x = right - j
                    upward = ((right + 1) & 2) == 0
                    y = size - 1 - vert if upward else vert
                    if not self.fn[y][x] and i < len(data) * 8:
                        self.m[y][x] = (data[i >> 3] >> (7 - (i & 7))) & 1 == 1
                        i += 1
            right -= 2

    MASKS = [
        lambda x, y: (x + y) % 2 == 0,
        lambda x, y: y % 2 == 0,
        lambda x, y: x % 3 == 0,
        lambda x, y: (x + y) % 3 == 0,
        lambda x, y: (x // 3 + y // 2) % 2 == 0,
        lambda x, y: x * y % 2 + x * y % 3 == 0,
        lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
        lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0,
    ]

    def _mask(self, mask):
        f = self.MASKS[mask]
        for y in range(self.size):
            for x in range(self.size):
                if not self.fn[y][x] and f(x, y):
                    self.m[y][x] = not self.m[y][x]

    def _penalty(self):
        size, m, p = self.size, self.m, 0
        lines = [row for row in m] + [[m[y][x] for y in range(size)] for x in range(size)]
        finder = ([True, False, True, True, True, False, True, False, False, False, False],
                  [False, False, False, False, True, False, True, True, True, False, True])
        for line in lines:
            run = 1
            for i in range(1, size + 1):
                if i < size and line[i] == line[i - 1]:
                    run += 1
                else:
                    if run >= 5:
                        p += run - 2
                    run = 1
            for i in range(size - 10):
                if line[i:i + 11] in [list(f) for f in finder]:
                    p += 40
        for y in range(size - 1):
            for x in range(size - 1):
                if m[y][x] == m[y][x + 1] == m[y + 1][x] == m[y + 1][x + 1]:
                    p += 3
        dark = sum(sum(row) for row in m)
        total = size * size
        p += ((abs(dark * 20 - total * 10) + total - 1) // total - 1) * 10
        return p

    def terminal(self, border=2):
        """Lines of text: two rows of modules per line, dark on light
        whatever the terminal's colours are (inverted codes do not scan
        everywhere)."""
        size = self.size
        get = lambda x, y: 0 <= x < size and 0 <= y < size and self.m[y][x]
        lines = []
        for y in range(-border, size + border, 2):
            row = ''
            for x in range(-border, size + border):
                top, bottom = get(x, y), get(x, y + 1)
                row += {(False, False): ' ', (True, False): '▀',
                        (False, True): '▄', (True, True): '█'}[(top, bottom)]
            lines.append('\x1b[30;107m' + row + '\x1b[0m')
        return lines

    def svg(self, border=4):
        size = self.size + 2 * border
        path = ''.join('M%d %dh1v1h-1z' % (x + border, y + border)
                       for y in range(self.size) for x in range(self.size) if self.m[y][x])
        return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" shape-rendering="crispEdges">'
                '<rect width="100%%" height="100%%" fill="#fff"/><path fill="#000" d="%s"/></svg>'
                % (size, size, path))
