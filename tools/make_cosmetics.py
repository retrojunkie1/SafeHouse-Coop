#!/usr/bin/env python3
"""The starter cosmetics (Sly2-Release/cosmetic.h): original low-poly items written as Mods/Cosmetics/<item>/
(item.ini, a Wavefront .obj and its .mtl). Usage: make_cosmetics.py OUT_DIR

Frames (cosmetic.cpp): a head item's origin is the top of the head, a back item's the middle of the back, X forward,
Z up; a hand item (Sly's cane) is in the cane's own frame (its shaft along X, the grip at the origin). Units: the
game's (about a centimetre)."""
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
    """Your sheet's first cane: a turned wooden shaft, brass collars and a long golden hook."""
    m = Mesh()
    m.material("wood", (0.62, 0.34, 0.14))
    m.material("gold", (0.92, 0.76, 0.18))
    L = 95.0
    m.cylinder("wood", (-40, 0, 0), (L - 40, 0, 0), 1.6, n=10)
    m.cylinder("gold", (-42, 0, 0), (-40, 0, 0), 1.9, n=10)
    for x in (L - 44, L - 40.5):
        m.cylinder("gold", (x, 0, 0), (x + 2.5, 0, 0), 2.2, n=10)
    x0 = L - 38
    # the hook: a blade sweeping up and back, its point turned down
    pts = [(x0, 0, 0), (x0 + 8, 0, 1.5), (x0 + 16, 0, 4.5), (x0 + 22, 0, 9), (x0 + 24, 0, 14), (x0 + 21, 0, 19),
           (x0 + 15, 0, 21), (x0 + 9, 0, 19.5)]
    for i in range(len(pts) - 1):
        r0 = 2.2 - 1.6 * i / (len(pts) - 1)
        r1 = 2.2 - 1.6 * (i + 1) / (len(pts) - 1)
        m.cylinder("gold", pts[i], pts[i + 1], r0, r1, n=8)
    m.cylinder("gold", pts[-1], (x0 + 5, 0, 15.5), 0.6, 0.1, n=8)
    item(out, "cane_golden_hook", """
name=Golden hook cane
slot=hand
characters=sly
hides=cane
""", m)


def cane_zigzag(out):
    """Your sheet's fourth cane: a grey zigzag hook, a dark grip, a wooden butt with a brass tip."""
    m = Mesh()
    m.material("steel", (0.42, 0.43, 0.46))
    m.material("grip", (0.22, 0.22, 0.24))
    m.material("wood", (0.66, 0.36, 0.14))
    m.material("brass", (0.86, 0.72, 0.30))
    m.cylinder("wood", (-42, 0, 0), (-22, 0, 0), 1.3, n=8)
    m.cylinder("brass", (-46, 0, 0), (-42, 0, 0), 0.3, 1.3, n=8)
    m.cylinder("grip", (-22, 0, 0), (-12, 0, 0), 3.2, 3.2, n=10)
    m.cylinder("steel", (-12, 0, 0), (36, 0, 0), 1.6, n=8)
    # the zigzag: up, back down, then the long point
    zz = [(36, 0, 0), (44, 0, 10), (36, 0, 14), (52, 0, 24)]
    for a, b in zip(zz, zz[1:]):
        m.cylinder("steel", a, b, 1.5, 1.3, n=8)
    m.cylinder("steel", zz[-1], (40, 0, 26), 1.2, 0.1, n=8)
    item(out, "cane_zigzag", """
name=Zigzag cane
slot=hand
characters=sly
hides=cane
""", m)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "Cosmetics"
    for f in (aviator_cap, thief_bandana, backpack, cane_golden_hook, cane_zigzag):
        f(out)
    print("written to", out)
