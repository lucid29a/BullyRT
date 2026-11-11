# Every light effect (2dfx) of the game's object types, read from Objects\ide.img as the game reads it

import os, struct, sys, json

GAME = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = sys.argv[2] if len(sys.argv) > 2 else None


def read_image(game):
    d = open(os.path.join(game, 'Objects', 'ide.dir'), 'rb').read()
    img = open(os.path.join(game, 'Objects', 'ide.img'), 'rb').read()
    return img


class R:
    def __init__(self, b, o): self.b, self.o = b, o
    def i(self): v, = struct.unpack_from('<i', self.b, self.o); self.o += 4; return v
    def f(self): v, = struct.unpack_from('<f', self.b, self.o); self.o += 4; return v
    def s(self):
        out = b''
        while True:
            c = self.b[self.o:self.o + 4]; self.o += 4
            if len(c) < 4: raise ValueError('eof')
            out += c
            if c[3] == 0: break
        return out.split(b'\0')[0].decode('latin1')


def tag(t): return t[::-1].encode()


def parse_objs(b, o, timed):
    r = R(b, o); n = r.i(); out = {}
    if not (0 < n < 20000): raise ValueError('count')
    for _ in range(n):
        kind = r.i()
        if not (0 <= kind <= 5): raise ValueError('kind')
        mid = r.i(); name = r.s(); r.s(); r.i(); r.f()
        r.o += 4 * (kind // 2); r.i(); r.o += 12; r.i()
        if kind % 2 == 0: r.o += 12
        if timed: r.i(); r.i()
        if not (0 <= mid < 30000) or not name: raise ValueError('id')
        out[mid] = name
    return out, r.o


def parse_2dfx(b, o):
    r = R(b, o); n = r.i(); out = []
    if not (0 < n < 20000): raise ValueError('count')
    for _ in range(n):
        mid = r.i(); pos = (r.f(), r.f(), r.f()); rgba = (r.i(), r.i(), r.i(), r.i()); r.o += 4
        tex1 = r.s(); tex2 = r.s()
        far, rng, size, f4 = r.f(), r.f(), r.f(), r.f()
        u3b = r.i(); kind = r.i(); c0, c1, c2 = r.f(), r.f(), r.f(); r.o += 4
        flare = r.i(); flags = r.i(); off = r.i()
        if not (0 <= mid < 30000) or not all(0 <= c <= 255 for c in rgba): raise ValueError('rec')
        out.append(dict(id=mid, pos=pos, rgb=rgba[:3], a=rgba[3], tex=tex1, tex2=tex2, far=far, range=rng, size=size, f4=f4,
                        u3b=u3b, kind=kind, cycle=(c0, c1, c2), flare=flare, flags=flags & 0xFF, off=off))
    return out, r.o


def main_quiet(game):
    return main(game, quiet=True)


def main(game=None, quiet=False):
    b = read_image(game or GAME)
    names, fx = {}, []
    for t, timed in (('objs', False), ('tobj', True)):
        at = b.find(tag(t))
        while at >= 0:
            try:
                got, _ = parse_objs(b, at + 4, timed); names.update(got)
            except (ValueError, struct.error, UnicodeDecodeError):
                pass
            at = b.find(tag(t), at + 1)
    at = b.find(tag('2dfx'))
    while at >= 0:
        try:
            got, _ = parse_2dfx(b, at + 4); fx += got
        except (ValueError, struct.error, UnicodeDecodeError):
            pass
        at = b.find(tag('2dfx'), at + 1)
    for e in fx: e['model'] = names.get(e['id'], '#%d' % e['id'])
    if not quiet: print('%d model names, %d light effects on %d models' % (len(names), len(fx), len(set(e['id'] for e in fx))))
    if OUT and not quiet:
        json.dump(fx, open(OUT, 'w'), indent=0)
    return fx


if __name__ == '__main__':
    fx = main()
    for e in fx[:0]: print(e)
