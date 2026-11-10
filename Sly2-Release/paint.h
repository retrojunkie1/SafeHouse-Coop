#pragma once

// The character repainter (QoL, SafeHouse-Coop): Sly, Bentley and Murray recoloured by their paint slots (a slot is a
// band of hues of the character's own textures: Sly's blue, Murray's pink, ...), each slot taking a new hue,
// saturation and brightness. The renderer marks each draw record with its character's paint profile (SCENEGLOBSET
// ipaint) and glob.frag recolours the fragments in a slot's band. The local player's paints are its own characters'
// (profiles 1..3) and saved in the user folder (paint.ini); co-op sends them, and the other player's copy is drawn
// with theirs (profiles 4..6). The wardrobe in the walkable safe house (po.cpp) edits them.

#include <array>
#include <cstdint>
#include <string>

struct ALO;

constexpr int CPAINTSLOT = 4;          // slots per character
constexpr int CPAINTCHAR = 3;          // Sly, Bentley, Murray (oids 7, 8, 9)
constexpr int CPAINTPROFILE = 6;       // local x 3, remote x 3
constexpr int CPAINTVEC = CPAINTPROFILE * CPAINTSLOT * 2;   // glob.frag's uniform vec4 paint[]

struct PAINTSLOT
{
    float hue = 0.0f;   // the new hue (0..1); the band's own variation kept around it
    float sat = 1.0f;   // saturation scale (0..2)
    float val = 1.0f;   // brightness scale (0.3..1.6)
    bool fOn = false;   // repainted (else the original)
};
struct CHARPAINT
{
    std::array<PAINTSLOT, CPAINTSLOT> aslot{};
};

// A character's slot: its name for the wardrobe and its band (hue centre / half width 0..1, the least saturation).
struct PAINTBAND
{
    const char* szName = nullptr;   // null: no slot
    float hue = 0.0f, halfWidth = 0.0f, satMin = 0.0f;
};
const PAINTBAND& PaintBand(int ichar, int islot);
const char* SzPaintChar(int ichar);
// The character index (0..2) of a gang member's oid (7, 8, 9), else -1.
int IPaintCharOfOid(int oid);

// The local paints (loaded from paint.ini at the first use), changed by the wardrobe; FSavePaints writes them.
CHARPAINT& LocalPaint(int ichar);
void SavePaints();
// The other player's paints (co-op), and forgetting them (disconnected).
void SetRemotePaint(int ichar, const CHARPAINT& cp);
void ClearRemotePaints();
// A change of the local paints since the last call (co-op sends them then).
bool FTakeLocalPaintChanged();
void MarkLocalPaintChanged();

// The draw record's profile for an object (its tree's root: a gang member, or the co-op copy of the other player):
// 1..6, 0 none.
uint8_t IPaintProfileOfAlo(const ALO* palo);
// glob.frag's paint[] (CPAINTVEC vec4s, x y z w): per profile, per slot: {band hue, half width, least saturation, on},
// {new hue, saturation scale, brightness scale, 0}.
void FillPaintUniforms(float* pv);

// The wardrobe's preset hues (name, hue 0..1, saturation, brightness).
struct PAINTPRESET { const char* szName; float hue, sat, val; };
int CPaintPresets();
const PAINTPRESET& PaintPreset(int i);
