# Every kind of light, alone, in four places: a small closed room, a corridor (two walls), a big hall, and outside in Bullworth
# (a night frame dump of the game). Every lamp model of the catalog that stands alone, and for every light texture that only lives
# inside a building or a room, a piece of that model cut 1.5 m around one of its fixtures. Each test is a frame dump for the replay
# with a lamps.dat of that lamp only, mounted as the game mounts it: ceiling lamps under the ceiling, wall lamps on a wall, the rest
# on the floor.

import math, os, struct, sys
import numpy as np

GAME, VOUT, OUTDOOR = sys.argv[1], sys.argv[2], sys.argv[3]
sys.argv = [sys.argv[0], GAME]
here = os.path.dirname(os.path.abspath(__file__))
exec(open(os.path.join(here, 'lampcatalog.py'), encoding='utf-8').read().replace("if __name__ == '__main__':\n    main()", ''))
exec(open(os.path.join(here, 'lampgallery.py'), encoding='utf-8').read().split('def main():')[0].split('PER_ROW')[1].split('\n', 1)[1])


def shape_emitters(nif):
    per_tex, alltris = {}, []
    for name, tex, emis, data, R, T, S in nif.shapes():
        v, t3 = nif.geometry(data)
        W = [[sum(R[i * 3 + k] * p[k] for k in range(3)) * S + T[i] for i in range(3)] for p in v]
        tris = [(W[a], W[b], W[c]) for a, b, c in t3]
        alltris += tris
        tb = os.path.basename(tex).lower()
        if tex_kind(tb): per_tex.setdefault(tb, []).extend(tris)
    return per_tex, alltris


def bounds(tris):
    a = np.array([p for t in tris for p in t])
    return a.min(0), a.max(0)


def rotz(p, ang):
    c, s = math.cos(ang), math.sin(ang)
    return [p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2]]


def box(lo, hi, walls=(1, 1, 1, 1), floor=True, ceiling=True):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    q = []
    if floor: q.append(((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)))
    if ceiling: q.append(((x0, y0, z1), (x0, y1, z1), (x1, y1, z1), (x1, y0, z1)))
    if walls[0]: q.append(((x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)))
    if walls[1]: q.append(((x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)))
    if walls[2]: q.append(((x0, y0, z0), (x0, y0, z1), (x1, y0, z1), (x1, y0, z0)))
    if walls[3]: q.append(((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)))
    return q


PLACES = {
    'room':     (box((-2, -2, 0), (2, 2, 3)),                       (-1.7, -1.7, 1.7), (0.6, 0.8, 1.2), 3.0, 2.0, (0.0, 0.3)),
    'corridor': (box((-1.5, -12, 0), (1.5, 12, 3), walls=(1, 1, 0, 0), ceiling=False), (0.0, -9.0, 1.7), (0.0, 2.0, 1.0), 3.0, None, (0.0, 0.0)),
    'hall':     (box((-10, -10, 0), (10, 10, 6)),                   (-8.5, -8.5, 3.0), (2.0, 2.0, 1.5), 6.0, 10.0, (0.0, 0.0)),
}


def outdoor_spot():
    txt = open(os.path.join(OUTDOOR, 'frame.txt')).read().split('\n')
    cam = vp = None
    for l in txt:
        if l.startswith('camera '): cam = np.array([float(x) for x in l.split()[1:4]])
        if l.startswith('viewproj '): vp = np.array([float(x) for x in l.split()[1:17]]).reshape(4, 4)
    fwd = vp[3, :3] / np.linalg.norm(vp[3, :3])
    pos, tris = [], []
    for l in open(os.path.join(OUTDOOR, 'frame.obj')):
        if l.startswith('v '): pos.append([float(x) for x in l.split()[1:4]])
        elif l.startswith('f '): tris.append([int(x.split('/')[0]) - 1 for x in l.split()[1:4]])
    P = np.array(pos); T = np.array(tris)
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    e1, e2 = b - a, c - a
    def first_hit(o, dvec, tmin_ok):
        h = np.cross(dvec, e2); det = (e1 * h).sum(1)
        ok = np.abs(det) > 1e-9
        f = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
        s = o - a; u = f * (s * h).sum(1)
        q = np.cross(s, e1); v = f * (q * dvec).sum(1); t = f * (e2 * q).sum(1)
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > tmin_ok)
        return t[hit].min() if hit.any() else None
    flat = np.array([fwd[0], fwd[1], 0.0]); flat /= np.linalg.norm(flat)
    above = cam + flat * 8.0 + np.array([0.0, 0.0, 3.0])
    td = first_hit(above, np.array([0.0, 0.0, -1.0]), 0.01)
    ground = above + np.array([0.0, 0.0, -(td if td is not None else 3.0 + 1.5)])
    return cam, vp, ground, open(os.path.join(OUTDOOR, 'frame.obj')).read(), [l for l in txt if not l.startswith('light ')]


def main():
    dirmap = read_dir(GAME)
    places = placements(GAME, dirmap)
    first = {}
    for p in places: first.setdefault(p[0].lower(), p)
    img = open(os.path.join(GAME, 'Stream', 'World.img'), 'rb')
    tests = []
    covered = set()
    big = []
    for key, plc in sorted(first.items()):
        if key + '.nif' not in dirmap: continue
        nm, off, sz = dirmap[key + '.nif']
        name = nm[:-4]
        if re.search(r'broke|_off\d*$|day\d*(_[a-z])?$|sgnday|strsnsday', name, re.I): continue
        img.seek(off * 2048)
        try:
            nif = Nif(img.read(sz * 2048)); em = model_emitters(nif, name)
            per_tex, alltris = shape_emitters(nif)
        except Exception:
            continue
        if not em or not alltris: continue
        lo, hi = bounds(alltris)
        inplace = any(sum((T[i] - plc[2][i]) ** 2 for i in range(3)) < 1.0 for T in model_emitters.placed_at)
        if inplace or max(hi[0] - lo[0], hi[1] - lo[1]) > 5.0 or hi[2] - lo[2] > 12.0:
            big.append((name, per_tex, alltris)); continue
        tests.append((name, [(c, n, ar, kind, min(max(0.5 * math.dist(l_, h_), 0.05), 0.9)) for (c, n, ar, l_, h_, kind) in em], alltris))
        covered.update(per_tex.keys())
    for name, per_tex, alltris in big:
        for tex, tris in per_tex.items():
            if tex in covered: continue
            cl = clusters(tris, 0.75)
            if not cl: continue
            c0 = max(cl, key=lambda e: e[2])[0]
            near = lambda t: min(sum((p[i] - c0[i]) ** 2 for i in range(3)) for p in t) < 1.5 ** 2
            piece = [t for t in alltris if near(t)]
            em = [(c, n, ar, tex_kind(tex), min(max(0.5 * math.dist(l_, h_), 0.05), 0.9)) for (c, n, ar, l_, h_) in clusters([t for t in tris if near(t)], 0.75)]
            if not em: continue
            tests.append(('%s~%s' % (name, tex[:-4] if tex.endswith('.tga') else tex), em, piece))
            covered.add(tex)
    print('%d lights to validate, %d light textures covered of %d' % (len(tests), len(covered), len(TEX_KIND)))
    cam_o, vp_o, ground, outdoor_obj, outdoor_txt = outdoor_spot()
    print('outdoor: the lamp stands at %.1f %.1f %.1f' % tuple(ground))
    os.makedirs(VOUT, exist_ok=True)
    index = open(os.path.join(VOUT, 'tests.txt'), 'w')
    for ti, (name, em, tris) in enumerate(tests):
        lo, hi = bounds(tris)
        nz = sum(e[1][2] for e in em) / len(em)
        hor = [sum(e[1][0] for e in em), sum(e[1][1] for e in em)]
        height = hi[2] - lo[2]
        if height > 2.2 or re.search(r'street|pole|strlamp|lamppost|standlamp|flood|field|bridgelamp|cityhall|xbeam', name, re.I):
            mount = 'floor'
        elif re.search(r'wall|sconce|stwall|candle|walllight|exit|sign', name, re.I) or (math.hypot(*hor) > 0.5 * len(em) and abs(nz) < 0.5):
            mount = 'wall'
        elif re.search(r'chand|ceil|hang|cloth_?light|flo|flour|adm_lamp|ohlight|fixture|gym|elev|bath|sew|motor|ware|inflamp|light_?nox|tennemnt|_lightaa', name, re.I) or nz < -0.3:
            mount = 'ceiling'
        else:
            mount = 'floor'
        ang = (math.atan2(-1.0, 0.0) - math.atan2(hor[1], hor[0])) if mount == 'wall' else 0.0
        R = lambda p: rotz(p, ang)
        rt = [tuple(R(p) for p in t) for t in tris]
        rlo, rhi = bounds(rt)
        rem = [(R(c), R(n), ar, kind, rad) for (c, n, ar, kind, rad) in em]
        for place in ('room', 'corridor', 'hall', 'outdoor'):
            if place == 'outdoor':
                base = list(ground); ceil_h = None; wall_y = None
            else:
                quads, eye, target, ceil_h, wall_y, spot = PLACES[place]
                base = [spot[0], spot[1], 0.0]
            off = [base[0] - (rlo[0] + rhi[0]) / 2, base[1] - (rlo[1] + rhi[1]) / 2, base[2] - rlo[2]]
            if mount == 'ceiling':
                top = (ceil_h if ceil_h else 3.5)
                off[2] = base[2] + top - rhi[2] - (0.0 if ceil_h else 0.0)
            elif mount == 'wall':
                if wall_y is not None: off[1] = wall_y - rhi[1]
                off[2] = base[2] + 2.0 - (rlo[2] + rhi[2]) / 2
            d = os.path.join(VOUT, '%03d' % ti, place)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, 'frame.obj'), 'w') as obj:
                vc = 0
                if place == 'outdoor':
                    obj.write(outdoor_obj)
                    vc = outdoor_obj.count('\nv ') + (1 if outdoor_obj.startswith('v ') else 0)
                    obj.write('\n')
                else:
                    obj.write('mtllib frame.mtl\ng draw0_flags0\nusemtl plain\n')
                    for q in quads:
                        for p in q: obj.write('v %.4f %.4f %.4f\n' % p)
                        obj.write('f %d %d %d\nf %d %d %d\nf %d %d %d\nf %d %d %d\n' % (
                            vc + 1, vc + 2, vc + 3, vc + 1, vc + 3, vc + 4, vc + 3, vc + 2, vc + 1, vc + 4, vc + 3, vc + 1))
                        vc += 4
                obj.write('g draw9000_flags2\nusemtl plain\n')
                for t in rt:
                    for p in t: obj.write('v %.4f %.4f %.4f\n' % (p[0] + off[0], p[1] + off[1], p[2] + off[2]))
                    obj.write('f %d %d %d\n' % (vc + 1, vc + 2, vc + 3))
                    vc += 3
            if place != 'outdoor':
                open(os.path.join(d, 'frame.mtl'), 'w').write('newmtl plain\n')
                vp = lookat_viewproj(eye, target, fov_y=70.0)
                with open(os.path.join(d, 'frame.txt'), 'w') as t:
                    t.write('frame 1\ncamera %.3f %.3f %.3f\nviewproj %s\nclock 23.000\narea -1 hour 23 cutscene 0\n' % (
                        *eye, ' '.join('%.7g' % vp[r][k] for r in range(4) for k in range(4))))
            else:
                for f_ in os.listdir(OUTDOOR):
                    if f_.endswith('.mtl'): open(os.path.join(d, f_), 'w').write(open(os.path.join(OUTDOOR, f_)).read())
                open(os.path.join(d, 'frame.txt'), 'w').write('\n'.join(outdoor_txt))
                with open(os.path.join(d, 'texdir.txt'), 'w') as t: t.write(OUTDOOR)
            with open(os.path.join(d, 'lamps.dat'), 'wb') as f:
                f.write(b'BLMP' + struct.pack('<II', 1, len(rem)))
                shares = emitter_shares([(c, n, ar, kind) for (c, n, ar, kind, rad) in rem], '~' in name)
                for ei, (c, n, ar, kind, rad) in enumerate(rem):
                    col, reach = emitter_light(kind, shares[ei])
                    f.write(struct.pack('<3f3ff3fiII', *[c[i] + off[i] for i in range(3)], *n, ar, *col, -1, reach << 8, int(rad * 100)))
        index.write('%03d %-40s %-7s %3d emitters %s\n' % (ti, name, mount, len(em), ' '.join(sorted(set(e[3] for e in em)))))
    index.close()


if __name__ == '__main__':
    main()
