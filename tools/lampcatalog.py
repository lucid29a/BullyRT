# Builds BullyDX12\bridge\lamps.dat: every light fixture of Bully's world, where it really is.

import math, os, re, struct, sys, zlib

GAME = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(GAME, 'BullyDX12', 'bridge', 'lamps.dat')
REPORT = sys.argv[3] if len(sys.argv) > 3 else None

LAMP_TEX = re.compile(r'light_on|f_light|_flight|lght_on|lamp|bulb|globe|chand|neon|candle|lantern|lantrn|torch|sconce|flour|fluor|lightfix|lite_on|light0|_lights?_|lightshade|lampshade|poollite', re.I)
NOT_LAMP_TEX = re.compile(r'_off|broke|flashlight|beam|cone|volume|bloom|spotlight_proj|shadow|glow|additive|sunspot|screen|reflect', re.I)
KINDS = {
    'ceiling': ((1.00, 0.94, 0.86), 1500),
    'fixture': ((1.00, 0.78, 0.52), 2000),
    'lamp':    ((1.00, 0.78, 0.52), 600),
    'street':  ((1.00, 0.66, 0.33), 10000),
    'flood':   ((1.00, 0.95, 0.88), 20000),
    'neon_w':  ((1.00, 0.95, 0.90), 300), 'neon_g': ((0.25, 1.00, 0.35), 300), 'neon_o': ((1.00, 0.45, 0.12), 300),
    'neon_r':  ((1.00, 0.12, 0.08), 300), 'neon_uv': ((0.55, 0.25, 1.00), 150),
    'sign':    ((0.40, 1.00, 0.45), 30),
    'red':     ((1.00, 0.08, 0.04), 80),
    'flame':   ((1.00, 0.58, 0.22), 15),
    'fire':    ((1.00, 0.55, 0.20), 600),
    'xmas':    ((1.00, 0.60, 0.35), 8),
    'window':  ((1.00, 0.82, 0.55), 250),
    'model':   ((1.00, 0.78, 0.52), 1500),
    'outwall': ((1.00, 0.72, 0.42), 2500),
    'industrial': ((1.00, 0.92, 0.80), 4000),
}
STREETLIKE = re.compile(r'street|strlamp|tri_lamp|lamppost|polelight|cityhalllmp|bridgelamp', re.I)
NAME_KIND = [(re.compile(r'pathlt', re.I), 'lamp'),
             (re.compile(r'stwalllamp|alleylamp|ext_walllamps|in_walllight|tennemntlght', re.I), 'outwall'),
             (re.compile(r'whlamp|warelight|warelamp|gymlight|gymlght|bx_ohlights|ware_flolights', re.I), 'industrial')]
LUX_EDGE = 0.05
FX_EDGE = 2.0
FX_NIGHT = {1, 5, 7, 9, 11}
FX_FLICKER = {2, 3, 10, 11}


def emitter_light(kind, share):
    kc, lm = KINDS[kind]
    flux = lm * share
    reach = min(max(math.sqrt(flux / (4 * math.pi) / LUX_EDGE), 1.5), 40.0)
    return tuple(x * flux for x in kc), int(round(reach))


def emitter_shares(em, whole):
    if whole:
        owner = list(range(len(em)))
        for i in range(len(em)):
            for j in range(i):
                if owner[j] == j and em[j][-1] == em[i][-1] and sum((em[i][0][k] - em[j][0][k]) ** 2 for k in range(3)) < 1.5 ** 2:
                    owner[i] = j; break
        area = {}
        for i, e in enumerate(em): area[owner[i]] = area.get(owner[i], 0.0) + e[2]
        return [em[i][2] / area[owner[i]] if area[owner[i]] > 0 else 1.0 for i in range(len(em))]
    tot = {}
    for e in em: tot[e[-1]] = tot.get(e[-1], 0.0) + e[2]
    return [e[2] / tot[e[-1]] if tot[e[-1]] > 0 else 1.0 for e in em]


_T = {
    'ceiling': 'sc28_f_light_on_d sc29_flight_d sc09_ceilinglght_d dorm_ceilinglight_d offi_ceilinglight_d sc17_ceiling_light_d sc06_flour_d '
               'sc12_lightwin_d sc27_lightwin_d ch00_hlight1_d ch00_hlight2_d ch00_hlightg_d sil4_hlight_d ap_gymlight_d',
    'fixture': 'sc28_lightfix_d sc28_chandalier_01_d sc28_chandalier_02_d lib_chandlr_d gdrm_chandelier_d re01_chandelier_d ps00_chandelier_d '
               'ps00_chandelier2_d ps00_chandelier3_d ps00_chandelier4_d ps00_chandelier5_d whchandalier_01_d whchandalier_02_d whchandalier_03_d ap_aniglobe_d',
    'lamp': 'lib_desklight_d lib_greenlamp_d lib_shelflamp_d re01_decolamp_d re01_lamp_d sc32_lamp_d sc32_poollite_d sc32_telescope_d psc_bio_inflamp_d '
            'sc31_inflamp_d fun_lampz_d fun_walllight1_d p_fun_lightbulb_d p_fun_minelight2_d p_fun_walllight2_d p_fun_walllight_d p_ipoor_hanglight_d '
            'p_ipoor_walllight_d p_cloth_light01_d p_cloth_wireslight_d sc11_wireslight_d psc_boil_light_d psc_boil_wirelight_d ppoor_light_d '
            'ppoor_light_covering_d ppoor_lightbulb_d ppoor_wall_light_d sil4_lightbulb_d sil4_lamp_d sil4_pipelight_d gb_rooflight_swap_d v_cclights_d',
    'street': 'sc00_ltpostbulb_d sc00_ltpostbulba_d polelight_01_d polelight_02_d pr_drlight_d ppoor_light_post_d',
    'flood': 'ppoor_floodlight_d af00_floodlght_d floodlt_d',
    'neon_w': 'ba00_neon_d gs00_neon01_d neon_closed_d neon_open_d neon_porn_d neon_test_d neon_tube_d newneon_d neonflicker_d neon_tube_phase_d '
              'cn_tinylite_d cn_lighta_swap_d cn_marquelt_001_d aquasign2_d cs01_aquasign_d gk_startlights_swap_d',
    'neon_g': 'neon_tubegreen_d', 'neon_o': 'neon_tubeorng_d', 'neon_r': 'neon_tubered_d', 'neon_uv': 'sil4_neonuv_d',
    'sign': 'sc09_exitsign_d as00_exit_d', 'red': 'sc05_redlamp_d redlight_d to00_drlight_d gk_stglowa_d',
    'flame': 'chr_candle_d sc36_candle_d fun_lanternlight_d jackolantern_d jackolantern2_d jackolantern3_d',
    'fire': 'fun_fakefire_d fun_fireplace_d',
    'xmas': 'xmas_light xmas_light3_d xmas_lightgreen_d xmas_lightred_d rc_xm_light1_swap_d rc_xm_light2_swap_d rc_xm_light3_swap_d '
            'sil4_xmasdecor01_d sil4_xmasdecor02_d sil4_xmasdecor03_d sil4_xmasdecor04_d rc_star_swap_d hal_lights1_d hal_lights2_d gk_stglowb_d',
    'window': 'glow_window_d windowcorona_02_d sc05_window_01n_d',
}
TEX_KIND = {t: k for k, names in _T.items() for t in names.split()}
def tex_kind(texbase):
    t = texbase.lower()
    for ext in ('.tga', '.bmp', '.dds'):
        if t.endswith(ext): t = t[:-len(ext)]
    return TEX_KIND.get(t)
LAMP_MODEL = re.compile(r'lamp|light|lght|lite|chand|bulb|sconce|flour|fluor|neon|lantern|lantrn|torch|candle|lmp|lgt|flood|globe', re.I)
NOT_LAMP_MODEL = re.compile(r'lightbeam|beams?$|flight|delight|highlight|slight|lightning|_off$|volume|cone|bloom|additive|addtve|lghtadd|vollight|flashlight|broke|glow|sunspot|day$|screen', re.I)
NIGHT_MODEL = re.compile(r'street|pole|alley|strlamp|stwalllamp|tennemnt|cityhall|bridge|patio|ext_|field|gate|xbeam|nlights|neon|wheellights|ballLight|lhouse|wall_lamp|in_walllight|af_|po_', re.I)
FLAME = re.compile(r'candle|torch|lantern|fire|flame', re.I)
FLICKER = re.compile(r'flick|flicker|trouble', re.I)


def read_dir(game):
    d = open(os.path.join(game, 'Stream', 'World.dir'), 'rb').read()
    out = {}
    for i in range(len(d) // 32):
        off, sz = struct.unpack_from('<II', d, i * 32)
        nm = d[i * 32 + 8:i * 32 + 32].split(b'\0')[0].decode('latin1')
        out[nm.lower()] = (nm, off, sz)
    return out


class Nif:
    def __init__(self, b):
        self.b = b
        o = b.index(b'\n') + 1
        self.ver, = struct.unpack_from('<I', b, o); o += 9
        nb, = struct.unpack_from('<I', b, o); o += 4
        nt, = struct.unpack_from('<H', b, o); o += 2
        types = []
        for _ in range(nt):
            l, = struct.unpack_from('<I', b, o); o += 4
            types.append(b[o:o + l].decode('latin1')); o += l
        idx = struct.unpack_from('<%dH' % nb, b, o); o += 2 * nb
        sizes = struct.unpack_from('<%dI' % nb, b, o); o += 4 * nb
        ns, = struct.unpack_from('<I', b, o); o += 8
        self.strings = []
        for _ in range(ns):
            l, = struct.unpack_from('<I', b, o); o += 4
            self.strings.append(b[o:o + l].decode('latin1')); o += l
        ng, = struct.unpack_from('<I', b, o); o += 4 + 4 * ng
        self.blocks = []
        for i in range(nb):
            self.blocks.append((types[idx[i] & 0x7fff], o, sizes[i]))
            o += sizes[i]

    def s(self, i):
        return self.strings[i] if 0 <= i < len(self.strings) else ''

    def i32(self, o): return struct.unpack_from('<i', self.b, o)[0]

    def net(self, o):
        name = self.s(self.i32(o)); o += 4
        ne = self.i32(o); o += 4 + 4 * ne
        return name, o + 4

    def av(self, o):
        name, o = self.net(o)
        o += 2
        t = struct.unpack_from('<3f', self.b, o); o += 12
        r = struct.unpack_from('<9f', self.b, o); o += 36
        sc, = struct.unpack_from('<f', self.b, o); o += 4
        npr = self.i32(o); o += 4
        props = list(struct.unpack_from('<%di' % npr, self.b, o)); o += 4 * npr
        o += 4
        return name, t, r, sc, props, o

    def texture_of(self, props):
        for p in props:
            if not (0 <= p < len(self.blocks)): continue
            t, o, _ = self.blocks[p]
            if t != 'NiTexturingProperty': continue
            _, o = self.net(o)
            o += 2
            o += 4
            if not self.b[o]: return ''
            src = self.i32(o + 1)
            if 0 <= src < len(self.blocks) and self.blocks[src][0] == 'NiSourceTexture':
                _, so = self.net(self.blocks[src][1])
                if self.b[so]:
                    return self.s(self.i32(so + 1))
        return ''

    def emissive_of(self, props):
        for p in props:
            if 0 <= p < len(self.blocks) and self.blocks[p][0] == 'NiMaterialProperty':
                return struct.unpack_from('<3f', self.b, self.blocks[p][1] + 48)
        return (0.0, 0.0, 0.0)

    def geometry(self, ref):
        if not (0 <= ref < len(self.blocks)): return [], []
        t, o, _ = self.blocks[ref]
        if t not in ('NiTriShapeData', 'NiTriStripsData'): return [], []
        b = self.b
        o += 4
        nv, = struct.unpack_from('<H', b, o); o += 4
        hv = b[o]; o += 1
        if not hv: return [], []
        v = list(struct.iter_unpack('<3f', b[o:o + 12 * nv])); o += 12 * nv
        vf, = struct.unpack_from('<H', b, o); o += 2
        hn = b[o]; o += 1
        if hn: o += 12 * nv
        if hn and (vf & 0x1000): o += 24 * nv
        o += 16
        hc = b[o]; o += 1
        if hc: o += 16 * nv
        o += 8 * nv * (vf & 63)
        o += 6
        nt, = struct.unpack_from('<H', b, o); o += 2
        tris = []
        if t == 'NiTriShapeData':
            o += 4
            if b[o]:
                o += 1
                tris = list(struct.iter_unpack('<3H', b[o:o + 6 * nt]))
        else:
            ns, = struct.unpack_from('<H', b, o); o += 2
            lens = struct.unpack_from('<%dH' % ns, b, o); o += 2 * ns
            if b[o]:
                o += 1
                for L in lens:
                    pts = struct.unpack_from('<%dH' % L, b, o); o += 2 * L
                    for k in range(L - 2):
                        a, c, e = pts[k], pts[k + 1], pts[k + 2]
                        if a == c or c == e or a == e: continue
                        tris.append((a, c, e) if k % 2 == 0 else (a, e, c))
        return v, [t3 for t3 in tris if max(t3) < len(v)]

    def shapes(self):
        out = []
        def mul(pr, pt, ps, r, t, s):
            R = [sum(pr[i * 3 + k] * r[k * 3 + j] for k in range(3)) for i in range(3) for j in range(3)]
            T = [sum(pr[i * 3 + k] * t[k] for k in range(3)) * ps + pt[i] for i in range(3)]
            return R, T, ps * s
        def walk(ref, R, T, S, depth):
            if depth > 32 or not (0 <= ref < len(self.blocks)): return
            t, o, _ = self.blocks[ref]
            if t in ('NiNode', 'NiBillboardNode', 'NiLODNode', 'NiSwitchNode', 'NiSortAdjustNode', 'NiBSPNode') or t.endswith('Node'):
                name, tr, r, sc, props, o2 = self.av(o)
                R2, T2, S2 = mul(R, T, S, r, tr, sc)
                n = self.i32(o2)
                kids = struct.unpack_from('<%di' % n, self.b, o2 + 4) if 0 <= n < 100000 else ()
                for k in kids: walk(k, R2, T2, S2, depth + 1)
            elif t in ('NiTriShape', 'NiTriStrips'):
                name, tr, r, sc, props, o2 = self.av(o)
                R2, T2, S2 = mul(R, T, S, r, tr, sc)
                data = self.i32(o2)
                out.append((name, self.texture_of(props), self.emissive_of(props), data, R2, T2, S2))
        walk(0, [1, 0, 0, 0, 1, 0, 0, 0, 1], [0, 0, 0], 1.0, 0)
        return out


def clusters(points_tris, radius=0.75):
    groups = []
    for (a, b, c) in points_tris:
        e1 = [b[i] - a[i] for i in range(3)]; e2 = [c[i] - a[i] for i in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        ar = 0.5 * math.sqrt(sum(x * x for x in n))
        if ar < 1e-6: continue
        m = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
        g = None
        for gg in groups:
            if sum((gg['c'][i] / gg['a'] - m[i]) ** 2 for i in range(3)) < radius * radius: g = gg; break
        if g is None:
            g = {'c': [0.0, 0.0, 0.0], 'a': 0.0, 'n': [0.0, 0.0, 0.0], 'lo': [1e9] * 3, 'hi': [-1e9] * 3}
            groups.append(g)
        for i in range(3):
            g['c'][i] += m[i] * ar
            g['lo'][i] = min(g['lo'][i], a[i], b[i], c[i]); g['hi'][i] = max(g['hi'][i], a[i], b[i], c[i])
        g['n'] = [g['n'][i] + n[i] for i in range(3)]
        g['a'] += ar
    out = []
    for g in groups:
        c = [g['c'][i] / g['a'] for i in range(3)]
        nl = math.sqrt(sum(x * x for x in g['n']))
        n = [x / nl for x in g['n']] if nl > 1e-6 * g['a'] else [0.0, 0.0, -1.0]
        out.append((c, n, g['a'], g['lo'], g['hi']))
    return out


def model_emitters(nif, model_name, nodes=True):
    lampish_model = LAMP_MODEL.search(model_name) and not NOT_LAMP_MODEL.search(model_name)
    by_kind = {}
    model_emitters.placed_at = []
    shapes = nif.shapes()
    any_lit_texture = any(tex_kind(os.path.basename(t)) for _, t, _, _, _, _, _ in shapes)
    for name, tex, emis, data, R, T, S in shapes:
        model_emitters.placed_at.append(T)
        kind = tex_kind(os.path.basename(tex))
        if kind in ('lamp', 'fixture') and STREETLIKE.search(model_name): kind = 'street'
        if kind in ('lamp', 'fixture', 'street', 'ceiling'):
            for rx, k in NAME_KIND:
                if rx.search(model_name): kind = k; break
        if kind is None:
            if not lampish_model or any_lit_texture: continue
            kind = 'street' if STREETLIKE.search(model_name) else 'model'
            for rx, k in NAME_KIND:
                if rx.search(model_name): kind = k; break
        v, tris = nif.geometry(data)
        if not tris: continue
        W = [[sum(R[i * 3 + k] * p[k] for k in range(3)) * S + T[i] for i in range(3)] for p in v] if nodes else [list(p) for p in v]
        by_kind.setdefault(kind, []).extend((W[a], W[b], W[c]) for a, b, c in tris)
    out = []
    for kind, tris in by_kind.items():
        if kind == 'model' or (kind == 'street' and not any_lit_texture):
            top = max(max(p[2] for p in t) for t in tris)
            bot = min(min(p[2] for p in t) for t in tris)
            cut = top - max(0.35 * (top - bot), 0.25)
            pts = [p for t in tris for p in t if p[2] > cut]
            groups = []
            for p in pts:
                for g in groups:
                    if (g[0][0] - p[0]) ** 2 + (g[0][1] - p[1]) ** 2 < 1.0: g[1].append(p); break
                else: groups.append([p, [p]])
            for _, g in groups:
                lo = [min(q[i] for q in g) for i in range(3)]; hi = [max(q[i] for q in g) for i in range(3)]
                out.append(([(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2] + 0.02], [0.0, 0.0, -1.0], 0.1, lo, hi, kind))
            continue
        if kind in HEAD_KINDS and len(tris) < 4000:
            heads = lamp_heads(tris)
            if heads and kind != 'street':
                heads = [max(heads, key=lambda h: h[1])]
            if heads:
                out += [(c, [0.0, 0.0, -1.0], ar, lo, hi, kind) for (c, ar, lo, hi) in heads]
                continue
        cl = clusters(tris, 0.75)
        if kind == 'ceiling' and cl:
            top = max(h[2] for (c, n, ar, lo, h) in cl)
            cl = [e for e in cl if e[0][2] > top - 1.0]
        for (c, n, ar, lo, hi) in cl:
            out.append((c, n, ar, lo, hi, kind))
    return out


HEAD_KINDS = ('lamp', 'outwall', 'street', 'industrial', 'model')


def lamp_heads(tris):
    down = []
    for (a, b, c) in tris:
        e1 = [b[i] - a[i] for i in range(3)]; e2 = [c[i] - a[i] for i in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        l = math.sqrt(sum(x * x for x in n))
        if l < 1e-9: continue
        if abs(n[2]) / l > 0.5: down.append((a, b, c, 0.5 * l))
    if not down: return []
    zs = [p[2] for t in tris for p in t]
    top, bot = max(zs), min(zs)
    groups = []
    for (a, b, c, ar) in down:
        m = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
        for g in groups:
            if (g['m'][0] / g['a'] - m[0]) ** 2 + (g['m'][1] / g['a'] - m[1]) ** 2 < 0.6 ** 2: break
        else:
            g = {'m': [0.0, 0.0, 0.0], 'a': 0.0, 'lo': [1e9] * 3, 'hi': [-1e9] * 3}; groups.append(g)
        for i in range(3):
            g['m'][i] += m[i] * ar; g['lo'][i] = min(g['lo'][i], a[i], b[i], c[i]); g['hi'][i] = max(g['hi'][i], a[i], b[i], c[i])
        g['a'] += ar
    best = max(g['a'] for g in groups)
    lowest = min(g['lo'][2] for g in groups if g['a'] >= 0.1 * best)
    out = []
    for g in groups:
        if g['a'] < 0.1 * best or g['lo'][2] > lowest + 0.3: continue
        c = [g['m'][i] / g['a'] for i in range(3)]
        out.append(([c[0], c[1], g['lo'][2] + 0.03], g['a'], g['lo'], g['hi']))
    return out


def placements(game, dirmap):
    out = []
    for key, (nm, off, sz) in dirmap.items():
        if not key.endswith('.ipb'): continue
        with open(os.path.join(game, 'Stream', 'World.img'), 'rb') as f:
            f.seek(off * 2048); b = f.read(sz * 2048)
        i = b.find(b'inst')
        if i < 0: continue
        cnt, = struct.unpack_from('<I', b, i + 4); o = i + 8
        for _ in range(cnt):
            r = b[o:o + 120]; o += 120
            if len(r) < 120: break
            name = r[4:68].split(b'\0')[0].decode('latin1')
            area, = struct.unpack_from('<f', r, 68)
            pos = struct.unpack_from('<3f', r, 72); sc = struct.unpack_from('<3f', r, 84); q = struct.unpack_from('<4f', r, 96)
            out.append((name, int(round(area)), pos, sc, q, nm))
    return out


def qrot(q, v):
    x, y, z, w = q
    x, y, z = -x, -y, -z
    ix = w * v[0] + y * v[2] - z * v[1]; iy = w * v[1] + z * v[0] - x * v[2]
    iz = w * v[2] + x * v[1] - y * v[0]; iw = -x * v[0] - y * v[1] - z * v[2]
    return [ix * w + iw * -x + iy * -z - iz * -y, iy * w + iw * -y + iz * -x - ix * -z, iz * w + iw * -z + ix * -y - iy * -x]


def main():
    dirmap = read_dir(GAME)
    import ide2dfx
    fxs = {}
    for e in ide2dfx.main_quiet(GAME):
        if e['range'] > 0.0: fxs.setdefault(e['model'].lower(), []).append(e)
    places = placements(GAME, dirmap)
    used = {}
    for p in places: used.setdefault(p[0].lower(), []).append(p)
    img = open(os.path.join(GAME, 'Stream', 'World.img'), 'rb')
    cache = {}
    rep = []
    lamps = []
    for key, plist in sorted(used.items()):
        nifkey = key + '.nif'
        if nifkey not in dirmap: continue
        nm, off, sz = dirmap[nifkey]
        img.seek(off * 2048); b = img.read(sz * 2048)
        try:
            nif = Nif(b)
            em_world = model_emitters(nif, nm[:-4], True)
            baked = list(model_emitters.placed_at)
            em_local = model_emitters(nif, nm[:-4], False)
        except Exception as e:
            continue
        em = em_local or em_world
        if not em: continue
        name = nm[:-4]
        if re.search(r'broke|_off\d*$|day\d*(_[a-z])?$|sgnday|strsnsday', name, re.I): continue
        flags = (2 if FLICKER.search(name) else 0) | (4 if FLAME.search(name) else 0)
        rep.append('%-28s %3d placements x %3d emitters  %s' % (name, len(plist), len(em), ' '.join(sorted(set(e[5] for e in em)))))
        h = zlib.crc32(name.lower().encode()) & 0xffffffff
        for (pname, area, pos, sc, q, src) in plist:
            inplace = any(sum((T[i] - pos[i]) ** 2 for i in range(3)) < 1.0 for T in baked)
            ems = em_world if inplace else em_local
            shares = emitter_shares(ems, True if inplace else False)
            for ei, (c, n, ar, lo, hi, kind) in enumerate(ems):
                if inplace:
                    wp, wn = list(c), list(n)
                else:
                    lc = [c[i] * sc[i] for i in range(3)]
                    wp = qrot(q, lc); wn = qrot(q, n)
                    wp = [wp[i] + pos[i] for i in range(3)]
                col, reach = emitter_light(kind, shares[ei])
                night = 1 if area == 0 else 0
                fx = fxs.get(name.lower())
                if fx:
                    if inplace:
                        rng = sum(e['range'] for e in fx) / len(fx); rgb = [sum(e['rgb'][i] for e in fx) / len(fx) for i in range(3)]
                        kinds = set(e['kind'] for e in fx); per = max(1, len(ems) / max(len(fx), 1))
                    else:
                        e = min(fx, key=lambda e: sum((e['pos'][i] - c[i]) ** 2 for i in range(3)))
                        rng, rgb, kinds = e['range'], e['rgb'], {e['kind']}
                        per = max(1, sum(1 for (c2, *_r) in ems if min(fx, key=lambda e2: sum((e2['pos'][i] - c2[i]) ** 2 for i in range(3))) is e))
                    peak = max(max(rgb), 1)
                    flux = 4 * math.pi * FX_EDGE * rng * rng / per
                    col = tuple(rgb[i] / peak * flux for i in range(3))
                    reach = int(round(min(max(math.sqrt(flux / (4 * math.pi) / LUX_EDGE), 1.5), 40.0)))
                    night = 1 if kinds & FX_NIGHT else 0
                    if kinds & FX_FLICKER: flags |= 2
                f = flags | (4 if kind == 'flame' else 0) | night | (min(reach, 255) << 8)
                rad = int(min(max(0.5 * math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3))) * max(sc), 0.05), 0.9) * 100)
                lamps.append((wp, wn, ar * sc[0] * sc[1], col, area, f, rad, name))
    with open(OUT, 'wb') as f:
        f.write(b'BLMP' + struct.pack('<II', 1, len(lamps)))
        for (p, n, ar, col, area, fl, h, _) in lamps:
            f.write(struct.pack('<3f3ff3fiII', *p, *n, ar, *col, area, fl, h))
    if REPORT:
        with open(REPORT, 'w') as f:
            f.write('%d emitters from %d models\n' % (len(lamps), len(rep)))
            f.write('\n'.join(rep) + '\n')
            for (p, n, ar, col, area, fl, h, name) in lamps:
                f.write('%-24s area %2d pos %8.2f %8.2f %7.2f n %5.2f %5.2f %5.2f area %.3f flags %d\n' % (name, area, *p, *n, ar, fl))
    print('%d emitters from %d models -> %s' % (len(lamps), len(rep), OUT))


if __name__ == '__main__':
    main()
