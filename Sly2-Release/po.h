#pragma once

// The player objects (PO, cid 0x32, module 0x1C3D70..0x1C4B00; Sly 1's PO): the objects a pad can control (the
// characters 0x34.., the guards 0x38.., the vehicles, the safe house's menu object cid 0x46 ...), and the current
// one (SW +0x3EFC, its override +0x3F00; 0x1C4840 / 0x1C4A48 / 0x1C4940): the character the pad drives and the
// camera follows. Each owns, once controlled, a controller state (PADCTL,
// 0xE0 bytes, constructed by 0x1C4280, kept in the world's list SW +0x3EAC and updated each frame by 0x1C4468): the
// sticks turned camera-relative, the buttons and the times they were pressed. The game state keeps the controlled
// objects (+0x3EB8 count, +0x3EBC up to 16), the current one (+0x3EFC) and a forced one (+0x3F00).

#include "basic.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct LO;
struct SW;

// orig 0x1C4280 (0xE0 bytes; a splice object of class 0x89: options 900..909)
struct PADCTL : BASIC
{
    float x = 0, y = 0;          // +0x10 / +0x14 the left stick (the pad's, then camera-relative)
    float radJoy = 0;            // +0x18 the left stick's world heading
    float uJoy = 0;              // +0x1C its deflection
    float x2 = 0, y2 = 0;        // +0x20 / +0x24 the right stick
    float radJoy2 = 0;           // +0x28
    float uJoy2 = 0;             // +0x2C
    uint32_t grfbtn = 0;         // +0x30 the buttons down (the game's layout, joy.h JOYPS2), less the handled ones
    glm::mat3 matCam{1.0f};      // +0x40 the camera's pan / tilt (0x11B720) at the update
    float uJoyDecay = 0;         // +0x70 the left stick's held deflection (decays by 0.2 a frame)
    float uJoyDecay2 = 0;        // +0x74
    uint32_t grfbtnPrev = 0;     // +0x78
    LO* plo = nullptr;           // +0x7C the owner
    int nControl = -1;           // +0x80 -1 none, 0 off, 1 current
    float atPressed[16]{};       // +0x88 when each button went down (-FLT_MAX: forgotten)
    float atStick[4]{};          // +0xC8 the left stick's last time at rest / moved, +0xD0 the right one's
};

// The player-object fields of a PO-derived object (+0x150..+0x180).
struct PODATA
{
    PADCTL* ppadctl = nullptr;   // +0x150 (made by 0x1C3ED8 on first control)
    uint32_t grfbtnForced = 0;   // +0x154 buttons the object holds down by itself
    int n15C = 1;                // +0x15C (0x1C3D70)
};

PODATA* PpodataFromLo(LO* plo);   // null for objects outside cid 0x32
void ResetPlayerObjects();        // a new world

// orig 0x001C4280 / 0x001C42E8 / 0x001C4330 / 0x001C4468 / 0x001C45A8 / 0x001C4640 / 0x001C4660 / 0x001C46C0 /
// 0x001C4738
void ClearPadctl(PADCTL* ppc);
void StampPadctl(PADCTL* ppc);  // 0x1C45A8, including synthesized actor-job buttons
void UpdatePadctl(PADCTL* ppc);
void SetPadctlControl(PADCTL* ppc, int n);
uint32_t GrfPadctlPressedWithin(const PADCTL* ppc, uint32_t grf, float dt);
uint32_t GrfPadctlTakePressed(PADCTL* ppc, uint32_t grf, float dt);
void ForgetPadctlPresses(PADCTL* ppc, uint32_t grf);
// The world update's pass: each controller state (SW +0x3EAC), 0x1C4468
void UpdateAllPadctls();

// the current player object and the enable (the world code's interface)
LO* PloPoCurrent(SW* psw);                   // orig 0x001C4840 (0x1C4A48 with the override)
void SetPoCurrent(SW* psw, LO* plo);         // orig 0x001C4940
void SetPoForced(SW* psw, LO* plo);          // orig 0x001C4A60 (SW +0x3F00; TODO(splice): the world's EOPIDs 927 / 928)
void EnablePo(SW* psw, LO* plo, int n);      // vt slot 79 (0x1C3ED8; players 0x1406E8; the safe house 0x1D7418)
int NPoEnabled(const LO* plo);               // orig 0x001C4080 (-1: never)
void ResetPos(SW* psw);                      // a new world's
int CpoRegistered();                         // orig SW +0x3EB8 (the objects slot 79 ever enabled or disabled)
const std::vector<LO*>& AploRegistered();    // orig SW +0x3EBC (in the order enabled)
int UnlockSafeHouseCharacters(SW* psw);   // debug (F6): every character selectable at the safe house (po.cpp)

// (QoL, the walkable safe house) the prompt to draw (1: sit down, 2: stand up, 0: none); the safe house's own door
// not offered (it is walked through)
int NSafeHousePrompt();   // (3: the wardrobe)
// (the walkable safe house) its wardrobe (paint.h) while open: the character, the selected row (the slots, then
// "Done"), each slot's preset (-1 the original)
struct WARDROBEVIEW
{
    int ichar = 0, irow = 0, cslot = 0;
    int aipreset[4]{};
};
bool FWardrobeView(WARDROBEVIEW* pv);
struct ACTOR;
// (the walkable safe house) its door used: opened (or closed) where it stands, walked through (its own use is a
// fade into the menu); true: handled
bool FSafeHouseDoorUse(LO* puse, ACTOR* pa);
