# A test of every light fixture of the catalog: the game's lamp models (not the buildings that carry lamps in their own mesh),
# twelve to a page, standing on a floor at night, each lit by its emitters from lamps.dat as the bridge places them.
# Every page is a frame dump the replay reads (frame.obj + frame.txt) with its own lamps.dat.

import math, os, struct, sys

GAME = sys.argv[1]
GOUT = sys.argv[2]
sys.argv = [sys.argv[0], GAME]
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'lampcatalog.py'), encoding='utf-8').read().replace("if __name__ == '__main__':\n    main()", '')
exec(src)

PER_ROW, ROWS, SPACING = 4, 3, 6.0


def lookat_viewproj(eye, target, fov_y=50.0, aspect=16 / 10, near=0.1, far=500.0):
    f = [target[i] - eye[i] for i in range(3)]; fl = math.sqrt(sum(x * x for x in f)); f = [x / fl for x in f]
    up = (0.0, 0.0, 1.0)
    r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]]
    rl = math.sqrt(sum(x * x for x in r)); r = [x / rl for x in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    V = [r + [-sum(r[i] * eye[i] for i in range(3))], u + [-sum(u[i] * eye[i] for i in range(3))],
         f + [-sum(f[i] * eye[i] for i in range(3))], [0.0, 0.0, 0.0, 1.0]]
    ys = 1.0 / math.tan(math.radians(fov_y) / 2); xs = ys / aspect
    P = [[xs, 0, 0, 0], [0, ys, 0, 0], [0, 0, far / (far - near), -near * far / (far - near)], [0, 0, 1, 0]]
    return [[sum(P[i][k] * V[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def main():
    dirmap = read_dir(GAME)
    places = placements(GAME, dirmap)
    first_place = {}
    for p in places: first_place.setdefault(p[0].lower(), p)
    img = open(os.path.join(GAME, 'Stream', 'World.img'), 'rb')
    models = []
    bigs = []
    for key, plc in sorted(first_place.items()):
        if key + '.nif' not in dirmap: continue
        nm, off, sz = dirmap[key + '.nif']
        img.seek(off * 2048)
        try:
            nif = Nif(img.read(sz * 2048))
            em = model_emitters(nif, nm[:-4])
        except Exception:
            continue
        if not em: continue
        pos = plc[2]
        inplace = any(sum((T[i] - pos[i]) ** 2 for i in range(3)) < 1.0 for T in model_emitters.placed_at)
        tris = []
        for name, tex, emis, data, R, T, S in nif.shapes():
            v, t3 = nif.geometry(data)
            W = [[sum(R[i * 3 + k] * p[k] for k in range(3)) * S + T[i] for i in range(3)] for p in v]
            tris += [(W[a], W[b], W[c]) for a, b, c in t3]
        if not tris: continue
        lo = [min(min(t[i] for t in tri) for tri in tris) for i in range(3)]
        hi = [max(max(t[i] for t in tri) for tri in tris) for i in range(3)]
        size = [hi[i] - lo[i] for i in range(3)]
        big = inplace or max(size[0], size[1]) > SPACING - 1.0 or size[2] > 12.0
        (bigs if big else models).append((nm[:-4], tris, em, lo, hi))
    print('%d lamp models in the gallery' % len(models))
    per_page = PER_ROW * ROWS
    os.makedirs(GOUT, exist_ok=True)
    index = open(os.path.join(GOUT, 'pages.txt'), 'w')
    for page in range(0, len(models), per_page):
        batch = models[page:page + per_page]
        d = os.path.join(GOUT, 'page%02d' % (page // per_page))
        os.makedirs(d, exist_ok=True)
        obj = open(os.path.join(d, 'frame.obj'), 'w')
        obj.write('mtllib frame.mtl\n')
        vcount = 0
        lamps = []
        names = []
        W_ = PER_ROW * SPACING; D_ = ROWS * SPACING
        quads = [((-2, -2, 0), (W_ + 2, -2, 0), (W_ + 2, D_ + 2, 0), (-2, D_ + 2, 0)),
                 ((-2, D_ + 2, 0), (W_ + 2, D_ + 2, 0), (W_ + 2, D_ + 2, 8), (-2, D_ + 2, 8))]
        obj.write('g draw0_flags0\nusemtl plain\n')
        for q in quads:
            for p in q: obj.write('v %.4f %.4f %.4f\n' % p)
            obj.write('f %d %d %d\nf %d %d %d\n' % (vcount + 1, vcount + 2, vcount + 3, vcount + 1, vcount + 3, vcount + 4))
            obj.write('f %d %d %d\nf %d %d %d\n' % (vcount + 3, vcount + 2, vcount + 1, vcount + 4, vcount + 3, vcount + 1))
            vcount += 4
        for k, (name, tris, em, lo, hi) in enumerate(batch):
            cx = (k % PER_ROW) * SPACING + SPACING / 2; cy = (k // PER_ROW) * SPACING + SPACING / 2
            off = [cx - (lo[0] + hi[0]) / 2, cy - (lo[1] + hi[1]) / 2, -lo[2]]
            down = sum(e[1][2] for e in em) < -0.5 * len(em)
            if down and hi[2] - lo[2] < 2.0: off[2] = 3.0 - hi[2]
            obj.write('g draw%d_flags2\nusemtl plain\n' % (k + 1))
            for tri in tris:
                for p in tri: obj.write('v %.4f %.4f %.4f\n' % (p[0] + off[0], p[1] + off[1], p[2] + off[2]))
                obj.write('f %d %d %d\n' % (vcount + 1, vcount + 2, vcount + 3))
                vcount += 3
            for ei, (c, n, ar, elo, ehi, kind) in enumerate(em):
                col, reach = emitter_light(kind, emitter_shares(em, False)[ei])
                lamps.append(([c[i] + off[i] for i in range(3)], n, ar, col, -1, reach << 8, 0))
            names.append('%2d %-24s %3d emitters (%s) at %.0f %.0f' % (k, name, len(em), ' '.join(sorted(set(e[5] for e in em))), cx, cy))
        obj.close()
        open(os.path.join(d, 'frame.mtl'), 'w').write('newmtl plain\n')
        eye = (W_ / 2, -D_ * 0.55, D_ * 0.75); target = (W_ / 2, D_ * 0.45, 0.8)
        vp = lookat_viewproj(eye, target)
        with open(os.path.join(d, 'frame.txt'), 'w') as t:
            t.write('frame 1\ncamera %.3f %.3f %.3f\nviewproj %s\nclock 23.000\narea -1 hour 23 cutscene 0\n' % (
                *eye, ' '.join('%.7g' % vp[r][c] for r in range(4) for c in range(4))))
        with open(os.path.join(d, 'lamps.dat'), 'wb') as f:
            f.write(b'BLMP' + struct.pack('<II', 1, len(lamps)))
            for (p, n, ar, col, area, fl, h) in lamps:
                f.write(struct.pack('<3f3ff3fiII', *p, *n, ar, *col, area, fl, h))
        open(os.path.join(d, 'names.txt'), 'w').write('\n'.join(names) + '\n')
        index.write('page%02d: %s\n' % (page // per_page, ', '.join(m[0] for m in batch)))
    for j, (name, tris, em, lo, hi) in enumerate(bigs):
        d = os.path.join(GOUT, 'big%03d' % j)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'frame.obj'), 'w') as obj:
            obj.write('mtllib frame.mtl\ng draw1_flags2\nusemtl plain\n')
            vc = 0
            for tri in tris:
                for p in tri: obj.write('v %.4f %.4f %.4f\n' % tuple(p))
                obj.write('f %d %d %d\nf %d %d %d\n' % (vc + 1, vc + 2, vc + 3, vc + 3, vc + 2, vc + 1))
                vc += 3
        open(os.path.join(d, 'frame.mtl'), 'w').write('newmtl plain\n')
        c = [(lo[i] + hi[i]) / 2 for i in range(3)]
        ext = max(hi[0] - lo[0], hi[1] - lo[1], 4.0)
        eye = (c[0], lo[1] - ext * 0.15, lo[2] + min(hi[2] - lo[2], 12.0) * 0.55)
        target = (c[0], c[1] + ext * 0.2, lo[2] + 0.5)
        vp = lookat_viewproj(eye, target, fov_y=70.0)
        with open(os.path.join(d, 'frame.txt'), 'w') as t:
            t.write('frame 1\ncamera %.3f %.3f %.3f\nviewproj %s\nclock 23.000\narea -1 hour 23 cutscene 0\n' % (
                *eye, ' '.join('%.7g' % vp[r][k] for r in range(4) for k in range(4))))
        with open(os.path.join(d, 'lamps.dat'), 'wb') as f:
            f.write(b'BLMP' + struct.pack('<II', 1, len(em)))
            for ei, (cc, n, ar, elo, ehi, kind) in enumerate(em):
                col, reach = emitter_light(kind, emitter_shares(em, True)[ei])
                f.write(struct.pack('<3f3ff3fiII', *cc, *n, ar, *col, -1, reach << 8, 0))
        open(os.path.join(d, 'names.txt'), 'w').write('%s %d emitters (%s) size %.0f x %.0f x %.0f\n' % (
            name, len(em), ' '.join(sorted(set(e[5] for e in em))), hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
        index.write('big%03d: %s\n' % (j, name))
    print('%d big models on their own pages' % len(bigs))
    index.close()


if __name__ == '__main__':
    main()
