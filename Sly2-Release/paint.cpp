// The character repainter (paint.h).

#include "paint.h"

#include "lo.h"
#include "../platform/platform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

bool FCoopRemoteRoot(const ALO* palo);   // coop.cpp: the other player's drawn copy

namespace
{
// The bands (hue 0..1 = 0..360 degrees) of the characters' own textures (their texture dumps' histograms).
const PAINTBAND s_aband[CPAINTCHAR][CPAINTSLOT] = {
    // Sly: his blue (cap, shirt, mask, gloves, shoes: 220..230), the gold of his cane and his eyes (30..40), the red
    // of his outfit's trim (0); his fur is grey (unsaturated: none of them)
    {{"Outfit", 225.0f / 360.0f, 28.0f / 360.0f, 0.40f}, {"Cane and eyes", 37.0f / 360.0f, 16.0f / 360.0f, 0.60f},
     {"Trim", 0.0f, 14.0f / 360.0f, 0.55f}, {}},
    // Bentley: his skin (80..110), his shell and gadgets' blue (180..210), his hat's and gear's tan (30..40)
    {{"Skin", 95.0f / 360.0f, 25.0f / 360.0f, 0.40f}, {"Shell", 195.0f / 360.0f, 22.0f / 360.0f, 0.40f},
     {"Hat and gear", 37.0f / 360.0f, 14.0f / 360.0f, 0.35f}, {}},
    // Murray: his skin (purple, 280..295), his shirt (blue, 200..210), his hat and gloves (red, 350..10)
    {{"Skin", 290.0f / 360.0f, 25.0f / 360.0f, 0.50f}, {"Shirt", 205.0f / 360.0f, 20.0f / 360.0f, 0.40f},
     {"Hat and gloves", 0.0f, 18.0f / 360.0f, 0.60f}, {}},
};
const char* s_aszChar[CPAINTCHAR] = {"Sly", "Bentley", "Murray"};

const PAINTPRESET s_apreset[] = {
    {"Red", 0.0f / 360.0f, 1.1f, 1.0f},      {"Orange", 25.0f / 360.0f, 1.1f, 1.05f}, {"Gold", 45.0f / 360.0f, 1.0f, 1.1f},
    {"Lime", 90.0f / 360.0f, 1.0f, 1.05f},   {"Green", 130.0f / 360.0f, 1.0f, 0.95f}, {"Teal", 175.0f / 360.0f, 1.0f, 1.0f},
    {"Sky", 200.0f / 360.0f, 0.9f, 1.15f},   {"Blue", 225.0f / 360.0f, 1.1f, 1.0f},   {"Purple", 270.0f / 360.0f, 1.0f, 1.0f},
    {"Pink", 320.0f / 360.0f, 0.9f, 1.15f},  {"White", 0.0f, 0.08f, 1.5f},           {"Black", 0.0f, 0.35f, 0.4f},
};

CHARPAINT s_acpLocal[CPAINTCHAR];
CHARPAINT s_acpRemote[CPAINTCHAR];
bool s_fLoaded = false, s_fChanged = false;

std::filesystem::path PathPaints() { return cane::userDirectory() / "paint.ini"; }

void LoadPaints()
{
    if (s_fLoaded) return;
    s_fLoaded = true;
    std::ifstream in(PathPaints());
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        int ichar = -1, islot = -1, fOn = 0;
        float hue = 0, sat = 1, val = 1;
        if (!(ss >> ichar >> islot >> hue >> sat >> val >> fOn)) continue;
        if (ichar < 0 || ichar >= CPAINTCHAR || islot < 0 || islot >= CPAINTSLOT) continue;
        s_acpLocal[ichar].aslot[islot] = {hue, sat, val, fOn != 0};
    }
}
}

const PAINTBAND& PaintBand(int ichar, int islot) { return s_aband[ichar][islot]; }
const char* SzPaintChar(int ichar) { return ichar >= 0 && ichar < CPAINTCHAR ? s_aszChar[ichar] : "?"; }
int IPaintCharOfOid(int oid) { return oid >= 7 && oid <= 9 ? oid - 7 : -1; }

CHARPAINT& LocalPaint(int ichar)
{
    LoadPaints();
    return s_acpLocal[ichar];
}

void SavePaints()
{
    LoadPaints();
    std::ofstream out(PathPaints());
    out << "# character islot hue sat val on (the safe house wardrobe)\n";
    for (int c = 0; c < CPAINTCHAR; ++c)
        for (int s = 0; s < CPAINTSLOT; ++s)
        {
            const PAINTSLOT& p = s_acpLocal[c].aslot[s];
            out << c << ' ' << s << ' ' << p.hue << ' ' << p.sat << ' ' << p.val << ' ' << (p.fOn ? 1 : 0) << '\n';
        }
}

void SetRemotePaint(int ichar, const CHARPAINT& cp)
{
    if (ichar >= 0 && ichar < CPAINTCHAR) s_acpRemote[ichar] = cp;
}
void ClearRemotePaints()
{
    for (CHARPAINT& cp : s_acpRemote) cp = CHARPAINT{};
}
bool FTakeLocalPaintChanged()
{
    const bool f = s_fChanged;
    s_fChanged = false;
    return f;
}
void MarkLocalPaintChanged() { s_fChanged = true; }

uint8_t IPaintProfileOfAlo(const ALO* palo)
{
    const ALO* proot = palo;
    while (proot && proot->paloParent) proot = proot->paloParent;
    if (!proot) return 0;
    const int ichar = IPaintCharOfOid(proot->oid);
    if (ichar < 0) return 0;
    return uint8_t((FCoopRemoteRoot(proot) ? 4 : 1) + ichar);
}

void FillPaintUniforms(float* pv)
{
    LoadPaints();
    static const char* s_szTest = std::getenv("SLY2N_PAINTTEST");   // (debug: "c s hue": that slot painted, for all)
    for (int p = 0; p < CPAINTPROFILE; ++p)
    {
        const int ichar = p % CPAINTCHAR;
        const CHARPAINT& cp = p < CPAINTCHAR ? s_acpLocal[ichar] : s_acpRemote[ichar];
        for (int s = 0; s < CPAINTSLOT; ++s)
        {
            float* v = pv + (size_t(p) * CPAINTSLOT + size_t(s)) * 8;
            const PAINTBAND& b = s_aband[ichar][s];
            PAINTSLOT ps = cp.aslot[s];
            if (s_szTest)
            {
                int c = -1, k = -1;
                float h = 0;
                if (std::sscanf(s_szTest, "%d %d %f", &c, &k, &h) == 3 && c == ichar && k == s) ps = {h, 1.2f, 1.0f, true};
            }
            const bool fOn = b.szName && ps.fOn;
            v[0] = b.hue, v[1] = b.halfWidth, v[2] = b.satMin, v[3] = fOn ? 1.0f : 0.0f;
            v[4] = ps.hue, v[5] = ps.sat, v[6] = ps.val, v[7] = 0.0f;
        }
    }
}

int CPaintPresets() { return int(sizeof(s_apreset) / sizeof(s_apreset[0])); }
const PAINTPRESET& PaintPreset(int i) { return s_apreset[i]; }
