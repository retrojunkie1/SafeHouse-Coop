#!/usr/bin/env python3
"""The starter cosmetics (Sly2-Release/cosmetic.h): original low-poly items written as Mods/Cosmetics/<item>/
(item.ini, a Wavefront .obj and its .mtl). Usage: make_cosmetics.py OUT_DIR

Frames (cosmetic.cpp): a head item's origin is the top of the head, a back item's the middle of the back, X forward,
Z up; a hand item (Sly's cane) is in the cane's own frame (its shaft along X, the grip at the origin); a fists item
(Murray's) is one right fist, mirrored for the left: its centre at the origin, X the punch's direction, Z the back of
the hand, +Y the thumb's side. Units: the game's (about a centimetre)."""
import math
import os
import sys


class Mesh:
    def __init__(self):
        self.v = []      # positions
        self.f = []      # (material, [indices])
        self.mtl = {}    # name -> rgb

    def material(self, name, rgb):
        self.mtl[name] = rgb

    def quad(self, m, a, b, c, d):
        base = len(self.v) + 1
        self.v += [a, b, c, d]
        self.f.append((m, [base, base + 1, base + 2, base + 3]))

    def tri(self, m, a, b, c):
        base = len(self.v) + 1
        self.v += [a, b, c]
        self.f.append((m, [base, base + 1, base + 2]))

    def box(self, m, center, size, rot=None):
        cx, cy, cz = center
        sx, sy, sz = (s / 2 for s in size)
        p = [(x, y, z) for z in (-sz, sz) for y in (-sy, sy) for x in (-sx, sx)]
        if rot:
            p = [rot(q) for q in p]
        p = [(q[0] + cx, q[1] + cy, q[2] + cz) for q in p]
        for a, b, c, d in [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]:
            self.quad(m, p[a], p[b], p[c], p[d])

    def cylinder(self, m, p0, p1, r0, r1=None, n=12, caps=True):
        """A (truncated) cone between points p0 and p1 with radii r0, r1."""
        r1 = r0 if r1 is None else r1
        ax = [p1[i] - p0[i] for i in range(3)]
        ln = math.sqrt(sum(a * a for a in ax)) or 1.0
        ax = [a / ln for a in ax]
        tmp = (0, 0, 1) if abs(ax[2]) < 0.9 else (1, 0, 0)
        u = cross(ax, tmp)
        u = norm(u)
        w = cross(ax, u)
        ring = lambda p, r: [tuple(p[i] + r * (math.cos(2 * math.pi * k / n) * u[i] + math.sin(2 * math.pi * k / n) * w[i])
                                   for i in range(3)) for k in range(n)]
        a, b = ring(p0, r0), ring(p1, r1)
        for k in range(n):
            k2 = (k + 1) % n
            self.quad(m, a[k], a[k2], b[k2], b[k])
        if caps:
            for k in range(n):
                k2 = (k + 1) % n
                self.tri(m, p0, a[k2], a[k])
                self.tri(m, p1, b[k], b[k2])

    def ellipsoid(self, m, c, r, n=12, zmin=-1.0):
        """An ellipsoid (radii r), cut below zmin (-1: whole, 0: a dome)."""
        rings = n // 2
        pts = []
        for i in range(rings + 1):
            t = -math.pi / 2 + math.pi * i / rings
            z = math.sin(t)
            if z < zmin - 1e-6:
                z = zmin
            rr = math.sqrt(max(0.0, 1 - z * z))
            pts.append([(c[0] + r[0] * rr * math.cos(2 * math.pi * k / n), c[1] + r[1] * rr * math.sin(2 * math.pi * k / n),
                         c[2] + r[2] * z) for k in range(n)])
        for i in range(rings):
            for k in range(n):
                k2 = (k + 1) % n
                self.quad(m, pts[i][k], pts[i][k2], pts[i + 1][k2], pts[i + 1][k])

    def torus(self, m, c, axis, R, r, n=14, nr=6):
        """A ring around axis (unit) through c: radius R, tube radius r."""
        tmp = (0, 0, 1) if abs(axis[2]) < 0.9 else (1, 0, 0)
        u = norm(cross(axis, tmp))
        w = cross(axis, u)
        def pt(i, j):
            a, b = 2 * math.pi * i / n, 2 * math.pi * j / nr
            d = [math.cos(a) * u[k] + math.sin(a) * w[k] for k in range(3)]
            return tuple(c[k] + (R + r * math.cos(b)) * d[k] + r * math.sin(b) * axis[k] for k in range(3))
        for i in range(n):
            for j in range(nr):
                self.quad(m, pt(i, j), pt(i + 1, j), pt(i + 1, j + 1), pt(i, j + 1))

    def tube(self, m, pts, r0, r1=None, n=8):
        """A tube through points (x, y, z), its radius r0 tapering to r1."""
        r1 = r0 if r1 is None else r1
        for k in range(len(pts) - 1):
            a = r0 + (r1 - r0) * k / (len(pts) - 1)
            b = r0 + (r1 - r0) * (k + 1) / (len(pts) - 1)
            self.cylinder(m, pts[k], pts[k + 1], a, b, n=n)

    def write(self, d, name):
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name + ".mtl"), "w") as f:
            for k, rgb in self.mtl.items():
                f.write("newmtl %s\nKd %.3f %.3f %.3f\n" % (k, *rgb))
        with open(os.path.join(d, name + ".obj"), "w") as f:
            f.write("mtllib %s.mtl\n" % name)
            for p in self.v:
                f.write("v %.3f %.3f %.3f\n" % p)
            cur = None
            for m, idx in self.f:
                if m != cur:
                    f.write("usemtl %s\n" % m)
                    cur = m
                f.write("f %s\n" % " ".join(map(str, idx)))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    ln = math.sqrt(sum(x * x for x in a)) or 1.0
    return tuple(x / ln for x in a)


def item(out, ident, ini, mesh):
    d = os.path.join(out, ident)
    mesh.write(d, ident)
    with open(os.path.join(d, "item.ini"), "w") as f:
        f.write(ini.strip() + "\nmodel=%s.obj\n" % ident)


def aviator_cap(out):
    m = Mesh()
    m.material("leather", (0.42, 0.26, 0.14))
    m.material("fleece", (0.85, 0.80, 0.68))
    m.material("brass", (0.80, 0.62, 0.22))
    m.material("lens", (0.35, 0.62, 0.78))
    m.material("strap", (0.18, 0.12, 0.08))
    m.ellipsoid("leather", (0, 0, -9), (15, 14, 13), n=16, zmin=-0.05)          # the cap
    m.cylinder("fleece", (0, 0, -10), (0, 0, -8), 15.5, 15.0, n=16, caps=False)  # its fleece rim
    for s in (-1, 1):                                                            # the ear flaps
        m.box("leather", (-1, s * 14.5, -15), (10, 2.5, 13))
        m.box("fleece", (-1, s * 13.2, -15), (8, 1, 11))
    m.cylinder("strap", (13.4, -14, -2), (13.4, 14, -2), 1.2, n=8)            # the goggles' strap, its lenses
    for s in (-1, 1):
        m.cylinder("brass", (13.5, s * 5.5, -2), (16.5, s * 5.5, -2), 4.4, n=12)
        m.cylinder("lens", (16.4, s * 5.5, -2), (16.9, s * 5.5, -2), 3.4, n=12)
    item(out, "aviator_cap", """
name=Aviator cap
slot=head
characters=all
hides=hat
""", m)


def thief_bandana(out):
    m = Mesh()
    m.material("cloth", (0.09, 0.09, 0.10))   # (a black burglar's kerchief over the crown, tied at the back)
    m.material("dots", (0.88, 0.88, 0.84))
    m.ellipsoid("cloth", (0, 0, -9), (15.5, 14.5, 12), n=16, zmin=-0.12)       # over the top of the head
    for (x, y, z) in [(6, 7, 1.8), (-4, -8, 1.8), (2, -3, 3.2), (-8, 4, 1.4), (9, -4, 0.2)]:
        m.ellipsoid("dots", (x, y, z), (1.4, 1.4, 0.6), n=8)
    m.ellipsoid("cloth", (-15, 0, -9), (3, 3.2, 3), n=10)                      # the knot at the back
    m.box("cloth", (-18, -2.5, -15), (2, 4.5, 11), rot=lambda q: (q[0], q[1] - 0.25 * q[2], q[2]))   # its two ends
    m.box("cloth", (-18, 2.5, -15), (2, 4.5, 11), rot=lambda q: (q[0], q[1] + 0.25 * q[2], q[2]))
    item(out, "thief_bandana", """
name=Thief's bandana
slot=head
characters=all
hides=hat
""", m)


def backpack(out):
    m = Mesh()
    m.material("canvas", (0.34, 0.37, 0.20))
    m.material("flap", (0.28, 0.30, 0.16))
    m.material("strap", (0.30, 0.19, 0.10))
    m.material("buckle", (0.75, 0.70, 0.55))
    m.box("canvas", (-9, 0, 0), (13, 24, 27))                                   # the pack
    m.box("flap", (-9.5, 0, 11), (14, 25, 6))
    m.box("canvas", (-16.5, 0, -4), (3, 16, 12))                                 # its pocket
    m.box("buckle", (-18.2, 0, 1), (0.8, 4, 3))
    for s in (-1, 1):                                                           # the shoulder straps
        m.box("strap", (2, s * 7, 12), (14, 3, 1.5))
        m.box("strap", (9, s * 7, 2), (1.5, 3, 20))
    item(out, "backpack", """
name=Backpack
slot=back
characters=all
""", m)


def cane_golden_hook(out):
    """Your sheet's first cane: a turned wooden shaft, brass collars and a long golden hook. In the cane's own frame,
    as Sly's: the shaft along X from the butt (-40) to the hook's root (54), the hook curling in +Y out to x 124."""
    m = Mesh()
    m.material("wood", (0.62, 0.34, 0.14))
    m.material("gold", (0.92, 0.76, 0.18))
    m.cylinder("wood", (-40, 0, 0), (54, 0, 0), 1.7, n=10)
    m.cylinder("gold", (-43, 0, 0), (-40, 0, 0), 2.0, n=10)
    for x in (46, 50.5):
        m.cylinder("gold", (x, 0, 0), (x + 2.8, 0, 0), 2.4, n=10)
    # the hook: a blade sweeping forward and up, curling back to its point
    pts = [(54, 0), (70, 1.5), (88, 5), (104, 11), (116, 19), (121, 27), (115, 33), (104, 32)]
    for k in range(len(pts) - 1):
        r0 = 2.6 - 1.8 * k / (len(pts) - 1)
        r1 = 2.6 - 1.8 * (k + 1) / (len(pts) - 1)
        m.cylinder("gold", (pts[k][0], pts[k][1], 0), (pts[k + 1][0], pts[k + 1][1], 0), r0, r1, n=8)
    m.cylinder("gold", (104, 32, 0), (97, 26, 0), 0.8, 0.1, n=8)
    item(out, "cane_golden_hook", """
name=Golden hook cane
slot=hand
characters=sly
hides=cane
""", m)


def cane_zigzag(out):
    """Your sheet's fourth cane: a grey zigzag hook, a dark grip, a wooden butt with a brass tip (the cane's frame:
    the shaft along X, the hook in +Y)."""
    m = Mesh()
    m.material("steel", (0.42, 0.43, 0.46))
    m.material("grip", (0.22, 0.22, 0.24))
    m.material("wood", (0.66, 0.36, 0.14))
    m.material("brass", (0.86, 0.72, 0.30))
    m.cylinder("brass", (-46, 0, 0), (-42, 0, 0), 0.3, 1.4, n=8)
    m.cylinder("wood", (-42, 0, 0), (-22, 0, 0), 1.4, n=8)
    m.cylinder("grip", (-22, 0, 0), (-10, 0, 0), 3.4, 3.4, n=10)
    m.cylinder("steel", (-10, 0, 0), (54, 0, 0), 2.1, n=8)
    # the zigzag: up, back, then the long point
    zz = [(54, 0), (70, 18), (60, 23), (112, 31)]
    for a, b in zip(zz, zz[1:]):
        m.cylinder("steel", (a[0], a[1], 0), (b[0], b[1], 0), 2.2, 1.9, n=8)
    m.cylinder("steel", (112, 31, 0), (121, 28, 0), 1.9, 0.1, n=8)
    item(out, "cane_zigzag", """
name=Zigzag cane
slot=hand
characters=sly
hides=cane
""", m)


CANE = """
slot=hand
characters=sly
hides=cane
"""


def hook_curl(cx, cy, r, a0, a1, k=10):
    """Points on an arc (the hooks' curls), angles in degrees."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / k)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / k)), 0)
            for i in range(k + 1)]


def cane_black_barrel(out):
    """Your sheet's second cane: a black barrelled shaft, a chunky grip body at the hook's root and a curled gold hook."""
    m = Mesh()
    m.material("black", (0.10, 0.10, 0.12))
    m.material("steel", (0.38, 0.39, 0.42))
    m.material("gold", (0.93, 0.75, 0.20))
    m.material("wood", (0.45, 0.24, 0.10))
    m.cylinder("gold", (-44, 0, 0), (-40, 0, 0), 0.6, 2.0, n=10)
    m.cylinder("black", (-40, 0, 0), (40, 0, 0), 2.0, n=10)
    for x in (-30, -15, 0, 15, 30):                               # the barrel's rings
        m.cylinder("steel", (x, 0, 0), (x + 1.5, 0, 0), 2.4, n=10)
    m.box("black", (47, 1.5, 0), (14, 7, 5.5))                   # the grip body
    m.box("wood", (45, -3.0, 0), (9, 3, 4.6))
    m.cylinder("gold", (40, 0, 0), (41.5, 0, 0), 2.8, n=10)
    # the hook: up out of the body, over, and curling back down into a tight spiral
    pts = [(54, 2, 0), (66, 5, 0), (80, 11, 0), (94, 20, 0)] + hook_curl(96, 32, 12, -100, 120)[1:]
    m.tube("gold", pts, 2.6, 1.2)
    m.tube("gold", hook_curl(92, 31, 5, 120, 380, 8), 1.2, 0.5)
    item(out, "cane_black_barrel", "name=Black barrel cane" + CANE, m)


def cane_sail_blade(out):
    """Your sheet's third cane: an orange wooden shaft wound with a spiral, and a big pale sail-shaped hook striped dark."""
    m = Mesh()
    m.material("wood", (0.86, 0.46, 0.14))
    m.material("spiral", (0.42, 0.20, 0.07))
    m.material("blade", (0.90, 0.88, 0.80))
    m.material("stripe", (0.20, 0.20, 0.24))
    m.cylinder("spiral", (-44, 0, 0), (-40, 0, 0), 0.8, 1.9, n=10)
    m.cylinder("wood", (-40, 0, 0), (54, 0, 0), 1.9, n=10)
    sp = [(x, 2.0 * math.cos(x * 0.45), 2.0 * math.sin(x * 0.45)) for x in [-40 + 1.0 * i for i in range(95)]]
    m.tube("spiral", sp, 0.55, n=5)
    # the blade: a thick plate in the XY plane, its outline a sail curving up and back
    outline = [(52, -3), (64, -2), (80, 2), (96, 9), (110, 18), (120, 28), (122, 37), (114, 40), (104, 35), (92, 28),
               (78, 19), (64, 10), (54, 6)]
    th = 1.2
    cx, cy = 90.0, 18.0
    for k in range(len(outline)):
        a, b = outline[k], outline[(k + 1) % len(outline)]
        m.tri("blade", (cx, cy, th), (a[0], a[1], th), (b[0], b[1], th))
        m.tri("blade", (cx, cy, -th), (b[0], b[1], -th), (a[0], a[1], -th))
        m.quad("blade", (a[0], a[1], th), (a[0], a[1], -th), (b[0], b[1], -th), (b[0], b[1], th))
    for (x0, y0, x1, y1) in [(70, 1, 64, 11), (84, 5, 76, 18), (98, 12, 90, 27), (111, 20, 104, 35)]:   # the stripes
        for z in (th + 0.15, -th - 0.15):
            m.quad("stripe", (x0, y0, z), (x0 + 3, y0 + 0.5, z), (x1 + 3, y1 + 0.5, z), (x1, y1, z))
    item(out, "cane_sail_blade", "name=Sail blade cane" + CANE, m)


def cane_gold_bolt(out):
    """Your sheet's fifth cane: a red-brown shaft, gold end caps, a golden zigzag hook."""
    m = Mesh()
    m.material("shaft", (0.48, 0.16, 0.10))
    m.material("gold", (0.95, 0.77, 0.22))
    m.cylinder("gold", (-46, 0, 0), (-40, 0, 0), 0.8, 2.3, n=10)
    m.cylinder("shaft", (-40, 0, 0), (48, 0, 0), 1.9, n=10)
    m.cylinder("gold", (48, 0, 0), (55, 0, 0), 2.4, 2.4, n=10)
    zz = [(55, 0, 0), (76, 14, 0), (68, 19, 0), (96, 30, 0), (88, 35, 0), (118, 40, 0)]
    m.tube("gold", zz, 2.5, 1.6)
    m.cylinder("gold", (118, 40, 0), (126, 37, 0), 1.6, 0.1, n=8)
    item(out, "cane_gold_bolt", "name=Gold bolt cane" + CANE, m)


def cane_wrapped_zigzag(out):
    """Your sheet's sixth cane: shaft and zigzag hook wrapped in olive and white bands."""
    m = Mesh()
    m.material("olive", (0.42, 0.45, 0.20))
    m.material("white", (0.90, 0.89, 0.84))
    m.material("tip", (0.25, 0.25, 0.22))
    m.cylinder("tip", (-45, 0, 0), (-41, 0, 0), 0.8, 2.0, n=8)
    path = [(-41, 0, 0), (54, 0, 0), (72, 16, 0), (63, 22, 0), (110, 32, 0)]
    # bands every 6 units along the path, alternating colours
    seg = []
    for a, b in zip(path, path[1:]):
        ln = math.dist(a, b)
        nb = max(1, int(ln // 6))
        for i in range(nb):
            seg.append((tuple(a[k] + (b[k] - a[k]) * i / nb for k in range(3)), tuple(a[k] + (b[k] - a[k]) * (i + 1) / nb for k in range(3))))
    for i, (a, b) in enumerate(seg):
        m.cylinder("olive" if i % 2 == 0 else "white", a, b, 2.2 if i % 2 == 0 else 2.0, n=8)
    m.cylinder("olive", (110, 32, 0), (120, 29, 0), 2.0, 0.1, n=8)
    item(out, "cane_wrapped_zigzag", "name=Wrapped zigzag cane" + CANE, m)


FISTS = """
slot=fists
characters=murray
"""


def boxing_gloves(out):
    m = Mesh()
    m.material("glove", (0.80, 0.10, 0.10))
    m.material("cuff", (0.93, 0.92, 0.88))
    m.material("lace", (0.20, 0.20, 0.22))
    m.ellipsoid("glove", (2, 0, 0.5), (14, 11.5, 11), n=16)               # the padded fist
    m.ellipsoid("glove", (-1, 9.5, -3), (8, 5, 5.5), n=10)                # the thumb along its side
    m.cylinder("cuff", (-15, 0, 0), (-7, 0, 0), 8.5, 9.5, n=16)
    m.cylinder("lace", (-7.5, 0, 0), (-6.5, 0, 0), 9.8, n=16, caps=False)
    for x in (-13, -10.5):
        m.box("lace", (x, 0, 9.4), (1.0, 5, 0.8))
    item(out, "boxing_gloves", "name=Boxing gloves" + FISTS, m)


def brass_knuckles(out):
    m = Mesh()
    m.material("brass", (0.86, 0.68, 0.24))
    for y in (-6.6, -2.2, 2.2, 6.6):                                     # a ring on each finger, the knuckles forward
        m.torus("brass", (6, y, 1), (0, 1, 0), 3.4, 1.1, n=12, nr=5)
        m.box("brass", (9.8, y, 1), (1.6, 3.8, 6))                        # their striking face
    m.box("brass", (10, 0, 1), (1.4, 17, 3))
    m.tube("brass", [(2, -8, -5), (0, -3, -7), (0, 3, -7), (2, 8, -5)], 1.6)   # the grip bar in the palm
    item(out, "brass_knuckles", "name=Brass knuckles" + FISTS, m)


def oven_mitts(out):
    m = Mesh()
    m.material("quilt", (0.96, 0.62, 0.70))
    m.material("check", (0.98, 0.95, 0.90))
    m.material("trim", (0.65, 0.18, 0.30))
    m.ellipsoid("quilt", (3, 0, 0), (15, 12, 11.5), n=16)
    m.ellipsoid("quilt", (0, 11, -2), (9, 5.5, 6), n=10)
    for (x, y) in [(-4, -6), (-4, 6), (6, 0), (6, -10), (6, 10), (14, -5), (14, 5)]:   # the quilting's checks
        z = 11.5 * math.sqrt(max(0.0, 1 - ((x - 3) / 15) ** 2 - (y / 12) ** 2))
        m.ellipsoid("check", (x, y, z - 0.6), (2.6, 2.6, 0.9), n=8)
    m.cylinder("trim", (-17, 0, 0), (-9, 0, 0), 10, 11, n=16)
    m.torus("trim", (-19, 0, 6), (0, 1, 0), 3, 0.7, n=10, nr=4)              # the hanging loop
    item(out, "oven_mitts", "name=Oven mitts" + FISTS, m)


def foam_fingers(out):
    m = Mesh()
    m.material("foam", (0.98, 0.80, 0.10))
    m.material("cuff", (0.12, 0.30, 0.75))
    m.box("foam", (2, 0, 0), (20, 22, 20))                                # the big foam hand
    m.box("foam", (0, 12.5, -3), (12, 5, 7))                              # its thumb
    m.box("foam", (8, -5, 25), (7, 8, 32))                                 # the pointing finger, up
    m.ellipsoid("foam", (8, -5, 41), (3.5, 4, 3), n=10, zmin=0)
    m.box("cuff", (-12, 0, 0), (8, 24, 22))
    item(out, "foam_fingers", "name=Foam fingers" + FISTS, m)


def crab_claws(out):
    m = Mesh()
    m.material("shell", (0.92, 0.32, 0.14))
    m.material("tip", (0.98, 0.86, 0.70))
    m.ellipsoid("shell", (0, 0, 0), (13, 10, 10), n=14)                  # the claw's palm
    m.ellipsoid("shell", (16, 0, 5), (12, 5, 4.5), n=12)                  # the upper pincer
    m.ellipsoid("shell", (14, 0, -5), (10, 4.5, 3.8), n=12)               # the lower one
    m.ellipsoid("tip", (27, 0, 3.5), (3, 3, 2.5), n=8)
    m.ellipsoid("tip", (23.5, 0, -4), (2.5, 2.5, 2), n=8)
    for (x, z) in [(-4, 9), (4, 9.5), (0, -9)]:                           # its bumps
        m.ellipsoid("shell", (x, 0, z), (2, 2, 1.6), n=8)
    m.cylinder("shell", (-16, 0, 0), (-9, 0, 0), 7, 8.5, n=12)
    item(out, "crab_claws", "name=Crab claws" + FISTS, m)


ALL = (aviator_cap, thief_bandana, backpack, cane_golden_hook, cane_zigzag, cane_black_barrel, cane_sail_blade,
       cane_gold_bolt, cane_wrapped_zigzag, boxing_gloves, brass_knuckles, oven_mitts, foam_fingers, crab_claws)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "Cosmetics"
    for f in ALL:
        f(out)
    print("written to", out)
