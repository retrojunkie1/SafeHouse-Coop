#include "coop.h"
#include "pickup.h"
#include "actor.h"
#include "damage.h"
#include "breakable.h"
#include "step.h"
#include "murraycarry.h"
#include "trail.h"
#include "activate.h"
#include "display.h"
#include "nav.h"
#include <execinfo.h>
#include "carry.h"
#include "booty.h"
#include "guard.h"
#include "ainoise.h"
#include "projectile.h"
#include "so.h"
#include "sound.h"
#include "vtnative.h"

#include "bis.h"
#include "disc.h"
#include "frame.h"
#include "glob.h"
#include "alo.h"
#include "proc.h"
#include "pair.h"
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
#include "physworld.h"
#include "objmethod.h"
#include "brx.h"
#include "cmworld.h"
#include "cls.h"
#include "sw.h"
#include "transition.h"
#include "update.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include "drawdist.h"
#include <cmath>
#include "clock.h"

#include <cstdio>
#include <cstdlib>

ACTVIEW g_actview;

void OnRemoveGuard(LO* plo);  // guard.cpp (orig 0x2064D8)
void OnRemoveSpawner(LO* plo);  // spawner.cpp (orig 0x1746B8, after the base notification)

void OnRemoveHackSo(LO* plo);  // (hack.cpp: the hack objects' 0x17F110, the ship's 0x17F740, the enemies' 0x180598)
void OnRemoveTrigger(LO* plo);  // trigger.cpp (orig 0x149880, after the base 0x1A5848)

namespace
{
constexpr float S_STOP = 500.0f;  // orig D_002BD450: the distance the viewpoints move before every root is rechecked

bool FAlo(const LO* plo) { return FCidDerivedFrom(plo->Cid(), 0x30); }

// The parent's list keeps its added children first, its removed ones after (orig 0x1A5448: the list +0x64 and
// the first removed +0x70)
void UnlinkChild(LO* plo)
{
    if (!plo->paloParent) return;
    auto& a = plo->paloParent->aploChild;
    a.erase(std::remove(a.begin(), a.end(), plo), a.end());
}
void LinkChild(LO* plo)
{
    if (!plo->paloParent) return;
    auto& a = plo->paloParent->aploChild;
    if (plo->fRemoved) a.push_back(plo);
    else a.insert(std::find_if(a.begin(), a.end(), [](const LO* p) { return p->fRemoved; }), plo);
}

// vt slot 11 (0x1A5800, ALO 0x1F2C30): in the world, and so are its added children (TODO: the ALO's +0x40 |= 0x14,
// the BASIC notification slot 4, the ALO's 0x22FAA0 / 0x1F2B20 / 0x1F37C8 parts)
void OnAdd(LO* plo)
{
    plo->fInWorld = true;
    OnLoAddPhys(plo);  // the ALO's 0x1F2C30 physics part (physworld.cpp)
    // the camera objects (cid 0x2C, 0x12E6A0): their view at priority 10 when +0x124 (cmworld.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(11) == 0x12E6A0) EnableCmObject(plo, true);
    if (FAlo(plo))
        for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild))
            if (!pc->fRemoved) OnAdd(pc);
    OnAddTrail(plo);   // 0x21BBD0: refill the existing ring after the complete base OnAdd
    if (plo->pcls && plo->pcls->PfnOrig(11) == 0x224028) OnCarryAdd(plo);
    // the blocking polygons (class 0x0E's slot 11 0x1BC160, also 0x62 / 0xAE): queued for the nav's pass 0x1BE1E8
    if (plo->pcls && plo->pcls->PfnOrig(11) == 0x1BC160) MarkNavPolyDirty(plo);
    if (plo->pcls && plo->pcls->PfnOrig(11) == 0x1C2078) BumpNavLinkSerial();
}

// vt slot 12 (0x1A5848, ALO 0x1F2D48): out of the world (the ALO's fade reset), and so are its added children
void OnRemove(LO* plo)
{
    plo->fInWorld = false;
    if (plo->grf & (1ull << 59)) RemoveSafeGroundReferences(plo, nullptr);  // 0x1A5848 -> 0x1A00D0
    OnLoRemovePhys(plo);  // the ALO's 0x1F2D48 physics part (physworld.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x12E6E8) EnableCmObject(plo, false);  // 0x12E6E8
    if (FAlo(plo))
    {
        ALO* palo = static_cast<ALO*>(plo);
        palo->dgFade = 0.0f;
        palo->gFade = 1.0f;
        for (LO* pc : std::vector<LO*>(palo->aploChild))
            if (!pc->fRemoved) OnRemove(pc);
    }
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x224068) OnCarryRemove(plo);
    // 0x1BC198: the polygon out of its region and the nav's queue (its merge owner's members requeued: not modelled)
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x1BC198) RemoveNavPoly(plo);
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x1C20B0) BumpNavLinkSerial();
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x2064D8) OnRemoveGuard(plo);  // guard.cpp
    if (plo->pcls && (plo->pcls->PfnOrig(12) == 0x17F110 || plo->pcls->PfnOrig(12) == 0x17F740 ||
                      plo->pcls->PfnOrig(12) == 0x180598))
    {
        OnRemoveHackSo(plo);
    }
    // 0x1A5848's end: the notification slot 4 (0x133508) with message 1 (the object's own slot 5, then its listeners)
    if (FLoHasListeners(plo)) NotifyLoListeners(plo, 1, plo);
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x1746B8) OnRemoveSpawner(plo);
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x149880) OnRemoveTrigger(plo);
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x156D58) OnRemoveNoiseListener(plo);
    // 0x1B1E38 calls its ALO base first, then releases the flight clip/controller.
    if (plo->pcls && plo->pcls->PfnOrig(12) == 0x1B1E38) OnRemoveMissile(plo);
}

// orig 0x00147E20
void DequeueLoRoot(SW* psw, LO* plo)
{
    if (!plo->fQueued) return;
    if (psw->ploStop == plo)
    {
        auto it = std::next(plo->itQueued);
        psw->ploStop = it == psw->lsplo20.end() ? nullptr : *it;
    }
    psw->lsplo20.erase(plo->itQueued);
    plo->fQueued = false;
}

void SetNodeActive(SW* psw, LO* ploRoot, bool fIn)
{
    if (fIn == ploRoot->fNodeActive) return;
    if (fIn)
    {
        ploRoot->fNodeActive = true;
        RegisterLoTree(psw, ploRoot);  // vt slot 23
    }
    else
    {
        UnregisterLoTree(psw, ploRoot);  // vt slot 22
        ploRoot->fNodeActive = false;
    }
}

glm::vec3 PosPlayer(const glm::vec3& posDefault)
{
    // orig 0x1C4840: the current player
    LO* plo = g_transition.ploPlayer;
    return plo ? PosLoWorld(plo) : posDefault;
}

// orig 0x00148300: a root checked at once (AddLo, SetLoParent)
void CheckLoRoot(SW* psw, LO* plo)
{
    const glm::vec3 posA = g_actview.posCm;
    const uint32_t grf = g_actview.grfCm;
    const glm::vec3 posB = PosPlayer(posA);
    glm::vec3 posR;
    const bool fR = FCoopActivationPos(posR);   // (co-op: the other player's character activates its surroundings too)
    bool fIn = false;
    switch (plo->st)
    {
    case 1: fIn = true; break;
    case 2: fIn = false; break;
    case 0:
    case 3:
        if (grf & plo->bMask)
        {
            const glm::vec3 d = PosLoWorld(plo) - (plo->st == 0 ? posA : posB);
            fIn = glm::dot(d, d) < plo->sRadius * plo->sRadius;
            if (!fIn && fR)
            {
                const glm::vec3 dR = PosLoWorld(plo) - posR;
                fIn = glm::dot(dR, dR) < plo->sRadius * plo->sRadius;
            }
        }
        break;
    default: break;
    }
    SetNodeActive(psw, plo, fIn);
}

// The classes' InitLo (vt slot 7) bodies' activation part: the kind they set (0x1A6CA8; InitLo 0x1A5270: 2)
int StInit(CID cid)
{
    switch (cid)
    {
    case 0x04: case 0x0F: case 0x10: case 0x1C: case 0x57: case 0x68: case 0x73: case 0x74: case 0x79: case 0x7D:
        return 1;  // 0x1A3360, 0x220ED0, 0x153D58, 0x1391C8, 0x1CDDC8, 0x1471C0, 0x22B720, 0x15DC80, 0x1526D8, 0x20B4A8
    case 0x0B: case 0x0C: case 0x0D: case 0x12: case 0x13: case 0x1B: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x58:
        return 3;  // 0x182C70, 0x21D278
    case 0x06: case 0x07: case 0x18: case 0x19: case 0x1A: case 0x1D: case 0x2B: case 0x2F:
        return 0;  // 0x1CA788, 0x1492F0, 0x2281E8, 0x1591B0, 0x12EB48, 0x156CC8
    default: break;
    }
    if (cid >= 0x30 && cid < 0x6D) return 0;  // ALO 0x1F2108
    return 2;
}

// ... and their +0x40 flags (0x40: never in the update table; 2: updated; TODO: the other bits)
uint32_t GrfUpdInit(CID cid)
{
    switch (cid)
    {
    case 0x6D: case 0x6E: case 0x6F: case 0x75: return 0x21 | 0x40;  // 0x12F248, 0x1EFA90, 0x1564E8
    case 0x04: return 0x21 & ~0x20u;                                  // 0x1A3360
    case 0x55: return 0x21 | 8;                                       // 0x1DE400
    case 0x57: return 0x21 | 0xE;                                     // 0x1CDDC8: (v & 1) | 0xE
    case 0x68: return 0x21 | 0x102;                                   // 0x1471C0
    case 0x64: case 0x65: case 0x66: return 0x21 | 6;                 // 0x223F70 (the carryables)
    case 0x74: case 0x79: return 0x21 | 0xA;                          // 0x15DC80, 0x1526D8
    case 0x67:  // 0x13BDF0 (the booty)
    case 0x0F: case 0x10: case 0x18: case 0x19: case 0x1A: case 0x1D: case 0x2F: case 0x59: case 0x60: case 0x6C: case 0x73:
    case 0x7D:
        return 0x21 | 2;  // 0x220ED0, 0x153D58, 0x1492F0, 0x2281E8, 0x1591B0, 0x156CC8, 0x228B48 (the water), 0x1B0B78, 0x1D4E68, ...
    default: break;
    }
    if ((cid >= 0x32 && cid <= 0x46 && cid != 0x34) || cid == 0x4D || cid == 0x4E) return 0x21 | 2;  // 0x1C3D70
    // the hack objects (0x47..0x4C): their base init 0x17F090 sets the update bit too (the pieces 0x4A have no native
    // post-load that adds it: their time out / homing 0x1801B8 never ran; audit 2026-11 H1)
    if (cid >= 0x47 && cid <= 0x4C) return 0x21 | 2;
    return 0x21;
}
}

// orig: 0x00148300 (public entry for recovery; AddLo uses the same check internally)
void CheckLoRootNow(SW* psw, LO* plo) { if (psw && plo) CheckLoRoot(psw, plo); }

LO* PloRoot(LO* plo)
{
    while (plo->paloParent) plo = plo->paloParent;
    return plo;
}

bool FLoActive(const LO* plo)
{
    if (!plo->fInWorld) return false;
    const LO* p = plo;
    while (p->paloParent) p = p->paloParent;
    return p->fNodeActive;
}

glm::vec3 PosLoWorld(const LO* plo)
{
    if (FAlo(plo))
    {
        // (ALO vt slot 31: the object record's matrix (+0x60), not the first record's)
        ALO* palo = const_cast<ALO*>(static_cast<const ALO*>(plo));
        if (const XF* pxf = PxfObj(palo)) return glm::vec3(pxf->matWorld[3]);
        return palo->axf.empty() ? glm::vec3(0.0f) : glm::vec3(palo->axf[0]->matWorld[3]);
    }
    if (FCidDerivedFrom(plo->Cid(), 3))
    {
        // orig 0x231440 (0x231368: the reference's world x the local, +0xA0)
        const LOX* plox = static_cast<const LOX*>(plo);
        return glm::vec3((plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat)[3]);
    }
    return glm::vec3(0.0f);  // orig 0x1A62D8: the identity
}

void InitLoActivation(SW* psw, LO* plo)
{
    plo->st = 2;
    plo->bMask = 0xFF;
    plo->sRadius = 1e10f;
    plo->fInWorld = plo->paloParent ? plo->paloParent->fInWorld : true;
    plo->grfUpd = GrfUpdInit(plo->Cid());
    SetLoSt(psw, plo, StInit(plo->Cid()));  // (a root is queued when its class sets a kind)
    // orig 0x13E3C8: OnAdd when in the world, registered when its root is active
    if (plo->fInWorld) OnAdd(plo);
    if (FLoActive(plo)) RegisterLoTree(psw, plo);
}

void RegisterLoTree(SW* psw, LO* plo)
{
    AddLoToUpdateTable(psw, plo);
    OnLoRegisterPhys(plo);  // the ALO's 0x1F3EA8 physics part (physworld.cpp)
    // PROXY's slot 23 (0x1CA8E0): a streaming proxy (+0xE4 0) asks for an instance (0x149AB8: once, 0x1CA8C0)
    if (plo->pcls && plo->pcls->PfnOrig(23) == 0x1CA8E0 && psw->mpproxy.count(plo) && plo->U32Field(0, 0xE4) == 0)
        PostDeferredCall(plo, 0x1CA8C0, [](void* object, int) { PloAttachProxy(g_psw, static_cast<LO*>(object), 2); });
    if (plo->pcls && plo->pcls->PfnOrig(23) == 0x12ED20) OnAmbientRegistered(plo);  // (sound.h)
    if (plo->pcls && plo->pcls->PfnOrig(23) == 0x13CFD8) OnBootyStandRegistered(plo);  // (booty.h: cid 0x25)
    if (FAlo(plo))
        for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild))
            if (!pc->fRemoved) RegisterLoTree(psw, pc);
    if (plo->pcls && plo->pcls->PfnOrig(23) == 0x2079E0) OnGuardRegistered(plo);
}

void UnregisterLoTree(SW* psw, LO* plo)
{
    OnLoUnregisterPhys(plo);  // the ALO's 0x1F3E00 physics part (physworld.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(22) == 0x12EDE8) OnAmbientUnregistered(plo);  // (sound.h)
    RemoveLoFromUpdateTable(psw, plo);
    if (FAlo(plo))
        for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild))
            if (!pc->fRemoved) UnregisterLoTree(psw, pc);
    if (plo->pcls && plo->pcls->PfnOrig(22) == 0x2079A0) OnGuardUnregistered(plo);
    if (plo->pcls && plo->pcls->PfnOrig(22) == 0x2213D8) OnZoneUnregistered(plo);  // (cmworld.h)
}

void QueueLoRoot(SW* psw, LO* plo)
{
    DequeueLoRoot(psw, plo);
    auto itStop = psw->ploStop ? psw->ploStop->itQueued : psw->lsplo20.end();
    plo->itQueued = psw->lsplo20.insert(itStop, plo);
    plo->fQueued = true;
}

void AddLo(SW* psw, LO* plo)
{
    if (!plo->fRemoved) return;
    // (debug: SLY2N_ADDPRINT=oid prints the native call stack of that object's additions)
    if (static const char* s_sz = std::getenv("SLY2N_ADDPRINT"); s_sz && std::atoi(s_sz) == plo->oid && g_clock.t > 0.0f)
    {
        void* apv[24];
        std::fprintf(stderr, "add %d (alias %d, clone %d) t %.3f:\n", plo->oid, plo->oidInstance, plo->iclone, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 24), 2);
    }
    plo->fRemoved = false;
    UnlinkChild(plo);
    LinkChild(plo);
    if (!plo->paloParent || plo->paloParent->fInWorld) OnAdd(plo);
    if (!plo->paloParent && plo->st != 2) QueueLoRoot(psw, plo);
    if (FLoActive(plo)) RegisterLoTree(psw, plo);
    else if (!plo->paloParent) CheckLoRoot(psw, plo);
}

void RemoveLo(SW* psw, LO* plo)
{
    if (plo->fRemoved) return;
    // (debug: SLY2N_REMOVEPRINT=oid prints the native call stack of that object's removals)
    if (static const char* s_sz = std::getenv("SLY2N_REMOVEPRINT"); s_sz && std::atoi(s_sz) == plo->oid && g_clock.t > 0.0f)
    {
        void* apv[24];
        std::fprintf(stderr, "remove %d (alias %d) t %.3f:\n", plo->oid, plo->oidInstance, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 24), 2);
    }
    if (FLoActive(plo))
    {
        UnregisterLoTree(psw, plo);
        if (!plo->paloParent) plo->fNodeActive = false;
    }
    if (!plo->paloParent) DequeueLoRoot(psw, plo);
    const bool fInWorld = plo->fInWorld;
    plo->fRemoved = true;
    UnlinkChild(plo);
    LinkChild(plo);
    if (fInWorld) OnRemove(plo);
}

uint32_t g_cLoParentChanges = 0;   // activate.h

void SetLoParentActivation(SW* psw, LO* plo, ALO* paloNew, bool fMovePhys)
{
    const bool fOld = plo->fInWorld;
    const bool fNew = (!paloNew || paloNew->fInWorld) && !plo->fRemoved;
    if (FLoActive(plo))
    {
        UnregisterLoTree(psw, plo);
        if (!plo->paloParent) plo->fNodeActive = false;
    }
    ALO* paloOld = plo->paloParent;
    UnlinkChild(plo);
    plo->paloParent = paloNew;
    ++g_cLoParentChanges;
    LinkChild(plo);
    // orig vt slot 13 (0x1A5B80): a root that becomes a child gives up its node; a child that becomes a root
    // gets a new one
    if (paloNew && !paloOld)
    {
        DequeueLoRoot(psw, plo);
        plo->fNodeActive = false;
    }
    else if (!paloNew && paloOld)
    {
        plo->fNodeActive = false;
        plo->fQueued = false;
    }
    // Runtime slot 10 runs the original slot 13 physics move. Loader placements retain their existing path.
    if (fMovePhys && FAlo(plo)) MoveAloPhys(static_cast<ALO*>(plo), static_cast<ALO*>(plo), paloOld);
    else OnLoMovePhys(plo, paloNew, paloOld);
    if (fNew != fOld)
    {
        if (fNew) OnAdd(plo);
        else OnRemove(plo);
    }
    if (FLoActive(plo)) RegisterLoTree(psw, plo);
    else if (!plo->paloParent && fNew) CheckLoRoot(psw, plo);
    // 0x1A5900's end: the objects vt slot 28 lists follow (the grouper's (cid 0x1F, 0x15C310) kept explosions)
    if (FIsLoDerivedFrom(plo, 0x1F))
        if (LOXCHILD* pc = dynamic_cast<LOXCHILD*>(plo))
            for (LO* pch : std::vector<LO*>(pc->aploChild))
                if (FIsLoDerivedFrom(pch, 0x1E) && pch->paloParent != paloNew) SetLoParentActivation(psw, pch, paloNew);
}

void SetLoSt(SW* psw, LO* plo, int st)
{
    if (st == plo->st) return;
    plo->st = st;
    if (!plo->paloParent && plo->fInWorld) QueueLoRoot(psw, plo);
}

void SetLoRadius(SW* psw, LO* plo, float s)
{
    plo->sRadius = s;
    if (!plo->paloParent && plo->fInWorld) QueueLoRoot(psw, plo);
    // ALO (0x1F3118): a smaller radius goes down to the children (TODO: the +0x140 part, 0x1D6310 / 0x1D6378)
    if (FAlo(plo))
        for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild))
            if (s < pc->sRadius) SetLoRadius(psw, pc, s);
}

void SetLoMask(SW* psw, LO* plo, uint32_t b)
{
    LO* ploRoot = PloRoot(plo);
    if (ploRoot->fInWorld) QueueLoRoot(psw, ploRoot);
    // vt slot 46 (0x1A6DC0; ALO 0x1A6DC8: its children too, but those with +0x28 bit 58 (TODO))
    plo->bMask = static_cast<uint8_t>(b);
    if (FAlo(plo))
        for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild))
            if (!pc->fMaskOwn) SetLoMask(psw, pc, b);
    for (LO* p = plo->paloParent; p; p = p->paloParent) p->bMask |= static_cast<uint8_t>(b);
}

void UpdateSwActivation(SW* psw)
{
    if (static int s_c = 0; std::getenv("SLY2N_ACTLIST") && s_c++ == std::atoi(std::getenv("SLY2N_ACTLIST")))   // (debug: the radius-activated roots)
        for (LO* plo : psw->lsplo20)
            std::printf("actroot %d cid 0x%X st %d r %.0f mask 0x%X active %d dist %.0f\n", plo->oid, int(plo->Cid()), plo->st, plo->sRadius, plo->bMask,
                        int(plo->fNodeActive), glm::distance(PosLoWorld(plo), plo->st == 0 ? g_actview.posCm : PosPlayer(g_actview.posCm)));
    const uint32_t n = g_actview.grfCm;
    const glm::vec3 pos = g_actview.posCm;
    const glm::vec3 posB = PosPlayer(pos);
    const float gA = glm::distance(pos, psw->posCmLast);
    const float gB = glm::distance(posB, psw->posPoLast);
    if (S_STOP < gA || S_STOP < gB || n != psw->grfCmLast)
    {
        psw->ploStop = nullptr;
        psw->grfCmLast = n;
        psw->posCmLast = pos;
        psw->posPoLast = posB;
    }
    // (co-op: the other player's character activates its surroundings too; every root is checked each step then)
    glm::vec3 posR;
    const bool fR = FCoopActivationPos(posR);
    if (fR) psw->ploStop = nullptr;
    auto moveToEnd = [&](LO* plo) {
        // orig 0x147EE8
        psw->lsplo20.erase(plo->itQueued);
        if (!psw->ploStop) psw->ploStop = plo;
        plo->itQueued = psw->lsplo20.insert(psw->lsplo20.end(), plo);
    };
    for (auto it = psw->lsplo20.begin(); it != psw->lsplo20.end() && *it != psw->ploStop;)
    {
        LO* plo = *it++;
        bool fIn = false;
        switch (plo->st)
        {
        case 2:
        case 1:
            fIn = plo->st == 1;
            psw->lsplo20.erase(plo->itQueued);  // orig 0x147EC8
            plo->fQueued = false;
            break;
        case 0:
        case 3:
            if (!(n & plo->bMask))
            {
                fIn = false;
                moveToEnd(plo);
            }
            else
            {
                const float d = glm::distance(PosLoWorld(plo), plo->st == 0 ? pos : posB) - plo->sRadius;
                fIn = d < 0.0f;
                if (fR) fIn = fIn || glm::distance(PosLoWorld(plo), posR) < plo->sRadius;
                else if (S_STOP < std::fabs(d) - (plo->st == 0 ? gA : gB)) moveToEnd(plo);
            }
            break;
        default: break;
        }
        SetNodeActive(psw, plo, fIn);
    }
}

uint32_t GrfUpdLo(const SW* psw, const LO* plo)
{
    return plo->iupd >= 0 ? psw->aupd[plo->iupd].grf : plo->grfUpd;
}

void SetLoVisible(SW* psw, LO* plo, bool f)
{
    uint32_t& grf = plo->iupd >= 0 ? psw->aupd[plo->iupd].grf : plo->grfUpd;
    grf = f ? grf | 8u : grf & ~8u;
}

// (QoL draw distance, display.h) the activation radius (+0x38, option 13) of the root of palo's tree also limits what
// is drawn: an object outside it is inactive, out of the update table and so not drawn. Below 1x an active tree
// further than radius x drawDistance is not drawn (it still updates); above 1x an inactive one nearer than that is
// drawn as it was left (it does not update: the game's logic is the original's). 1x: the original. Returns 1: draw
// regardless of the update table, -1: do not draw, 0: the original's test.
int NDrawDistanceRoot(const ALO* palo)
{
    const float rScale = g_display.drawDistance;
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_DDACT"); return sz && sz[0] == '0'; }();   // (debug)
    if (rScale == 1.0f || s_fOff) return 0;
    const LO* proot = palo;
    while (proot->paloParent) proot = proot->paloParent;
    if ((proot->st != 0 && proot->st != 3) || !(proot->sRadius < 1e9f) || !(g_actview.grfCm & proot->bMask)) return 0;
    if (rScale < 1.0f ? !proot->fNodeActive : (proot->fNodeActive || !palo->fInWorld || !proot->fInWorld)) return 0;
    // (above 1x, drawdist-2: not the trees with a character (PO 0x33 and below: guards, NPCs, the gang), which would
    // stand frozen; they appear at the original's radius, faded in as the original (render.cpp's whole fade). Nor the
    // clue bottles of a streaming proxy: drawdist.cpp draws those animated, in place of the frozen instance.)
    if (rScale > 1.0f && (FDrawDistanceActorTree(proot) || (proot->Cid() == 0x5F && FDrawDistanceGhostedInstance(proot))))
        return 0;
    const glm::vec3 pos = proot->st == 0 ? g_actview.posCm : PosPlayer(g_actview.posCm);
    const float d = glm::distance(PosLoWorld(const_cast<LO*>(proot)), pos);
    const float r = proot->sRadius * rScale;
    if (rScale < 1.0f) return d - r < 0.0f ? 0 : -1;
    return d - r < 0.0f ? 1 : 0;
}

// (drawdist-2) a tree with a character in it: the root or any object under it derived from PO (0x33). Cached per
// root and game time (the trees change rarely; NDrawDistanceRoot asks for every drawn object).
bool FDrawDistanceActorTree(const LO* proot)
{
    struct CACHE { float t = -1.0f; bool f = false; };
    static std::unordered_map<const LO*, CACHE> s_mp;
    static const SW* s_psw = nullptr;
    if (s_psw != g_psw) { s_psw = g_psw; s_mp.clear(); }
    CACHE& c = s_mp[proot];
    if (c.t == g_clock.t) return c.f;
    std::function<bool(const LO*)> any = [&](const LO* p) {
        if (FCidDerivedFrom(p->Cid(), 0x33)) return true;
        if (FAlo(p))
            for (const LO* pc : static_cast<const ALO*>(p)->aploChild)
                if (pc && any(pc)) return true;
        return false;
    };
    c.t = g_clock.t;
    c.f = any(proot);
    return c.f;
}

// (drawdist-2) the tree of an inactive root that the draw distance option draws, emits and poses above 1x (never
// updated: its logic stays the original's): within its activation radius x the option, in the world, in the
// camera's mask, no character in it
bool FDrawDistanceVisualTree(const LO* plo)
{
    const float rScale = g_display.drawDistance;
    if (!(rScale > 1.0f) || !plo->fInWorld) return false;
    const LO* proot = plo;
    while (proot->paloParent) proot = proot->paloParent;
    if (proot->fNodeActive || !proot->fInWorld || proot->fRemoved) return false;
    if ((proot->st != 0 && proot->st != 3) || !(proot->sRadius < 1e9f) || !(g_actview.grfCm & proot->bMask)) return false;
    if (FDrawDistanceActorTree(proot)) return false;
    const glm::vec3 pos = proot->st == 0 ? g_actview.posCm : PosPlayer(g_actview.posCm);
    return glm::distance(PosLoWorld(const_cast<LO*>(proot)), pos) < proot->sRadius * rScale;
}

bool FAloDrawn(const SW* psw, const ALO* palo)
{
    const int nDd = NDrawDistanceRoot(palo);
    if (nDd < 0) return false;
    if (palo->iupd < 0 && (nDd == 0 || (palo->grfUpd & GRFUPD_Never))) return false;   // (never in the table: never drawn)
    const uint32_t grf = palo->iupd >= 0 ? psw->aupd[palo->iupd].grf : palo->grfUpd;
    if (!(grf & 8)) return false;
    const uint32_t pfnDraw = palo->pcls ? palo->pcls->PfnOrig(19) : 0;
    if (pfnDraw == 0 || pfnDraw == 0x1A5ED8) return false;
    if (!(palo->bMask & g_actview.grfCm)) return false;
    // orig: 0x001425B0 (the actors' draw, also under Sly's 0x18A238): an actor using its binocucom (+0xF04 1, on)
    // is not drawn, but in the binocucom view 2 (U19: Sly / Bentley / Murray hidden while looking through it)
    if (pfnDraw == 0x1425B0 || pfnDraw == 0x18A238)
        if (const ACTOR* pa = dynamic_cast<const ACTOR*>(PstepFromLo(const_cast<ALO*>(palo))))
            if (pa->nF04 == 1 && pa->nF20 != 2) return false;
    return palo->Cid() == 0x69 || palo->gFade != 0.0f;  // 0x21C340 draws surviving history at whole alpha 1
}

namespace
{
// orig 0x001CA978: how free an instance is for a proxy: 4 unattached, 3 attached but out of the world, 2 in the
// world but inactive, 1 active (taken)
int NScoreRow(const WE& we, int irow)
{
    if (irow < static_cast<int>(we.afRowLocked.size()) && we.afRowLocked[irow]) return 0;
    if (!we.aploRowOwner[irow]) return 4;
    int r = 5;
    for (int j = 0; j < we.cloLoaded; ++j)
    {
        const LO* plo = we.aplo[static_cast<std::size_t>(irow) * we.cloLoaded + j];
        int v;
        if (!plo->fInWorld) v = 3;
        else if (FLoActive(plo)) return 1;
        else v = 2;
        r = std::min(r, v);
    }
    return r;
}

LO* PloFindInInstance(const WE& we, int irow, int32_t oid)
{
    std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
        if (p->oid == oid || p->oidInstance == oid) return p;
        for (LO* pc : p->aploOwned)
            if (LO* q = find(pc)) return q;
        return nullptr;
    };
    for (int j = 0; j < we.cloLoaded; ++j)
        if (LO* p = find(we.aplo[static_cast<std::size_t>(irow) * we.cloLoaded + j])) return p;
    return nullptr;
}

// orig 0x001CAA60: an instance leaves its proxy (removed, its placement undone, back to the world's root)
void DetachProxy(SW* psw, LO* pproxy, int irow)
{
    PROXYSTREAM& ps = psw->mpproxy[pproxy];
    WE& we = psw->awe[ps.iwe];
    ps.airow.erase(std::remove(ps.airow.begin(), ps.airow.end(), irow), ps.airow.end());
    we.aploRowOwner[irow] = nullptr;
    const glm::mat4 matInv = glm::inverse(we.amatRow[irow]);
    we.amatRow[irow] = glm::mat4(1.0f);
    const XF* pxfProxy = static_cast<LOX*>(pproxy)->pxf;
    for (int j = 0; j < we.cloLoaded; ++j)
    {
        LO* plo = we.aplo[static_cast<std::size_t>(irow) * we.cloLoaded + j];
        // orig: 0x001CAA60 (0x1CABB0..0x1CAC14): undo each installed bind frame before removing the row's
        // objects. The first parent is the object's former frame; the bind remembers its target at +0x14.
        for (PROXYBIND& bind : ps.abind)
        {
            if (bind.pframe && bind.ploBound && pproxy->U32Field(0, 0xE4) == 0)
            {
                if (bind.pframe->m_cpframeParent > 0)
                {
                    bind.ploBound->pframe = bind.pframe->m_apframeParentOwner[0];  // orig: 0x00256EE0
                    bind.pframe->RemoveParent(bind.ploBound->pframe.get());       // orig: 0x00127B70
                }
                else bind.ploBound->pframe.reset();  // orig: 0x00256EE0(object, null)
            }
            bind.ploBound = nullptr;
        }
        // orig: 0x001CAC2C..0x001CAC44: notify the pooled object before
        // removing it and undoing its placement (clue bottles reset to -1).
        using PFN27 = void (*)(LO*, LO*);
        if (PFN27 pfn = reinterpret_cast<PFN27>(PvNativeVt(plo, 27))) pfn(plo, pproxy);
        // then the splice event 0xF to the proxy with the object and to the object with the proxy (the release, the
        // pair of the attach's 14 events); natively never sent, so scripts' release handlers did not run on pooled
        // objects (audit 2026-11 L2)
        {
            CRef refObj;
            refObj.SetBasic(plo);
            HandleLoSpliceEvent(pproxy, 15, false, 1, &refObj);
            CRef refProxy;
            refProxy.SetBasic(pproxy);
            HandleLoSpliceEvent(plo, 15, false, 1, &refProxy);
        }
        RemoveLo(psw, plo);  // vt slot 9 (TODO: the physics pose 0x1CA930)
        SetLoMatrix(plo, matInv);
        SetLoParentActivation(psw, plo, nullptr);  // 0x1A58D0
        if (FAlo(plo))
        {
            ALO* palo = static_cast<ALO*>(plo);
            if (std::getenv("SLY2N_ACTDEBUG") && !palo->axf.empty())
                std::fprintf(stderr, "  detach proxy %d -> %d: ref %p proxy's %p parent %d\n", pproxy->oid, plo->oid, (void*)palo->axf[0]->pxfRef,
                             (void*)pxfProxy, palo->paloParent ? palo->paloParent->oid : -1);
            // (0x1CAA60's SetLoParent(plo, 0, 0): no parent and no channel. The first record may by now hang from
            // another parent's record than the proxy's (world 32's hack chasers 44806 are re-parented to the level
            // group while chasing): left there, the next attach's channel (SetLoChannel keeps a set reference) never
            // took and the reused chasers stayed at the world origin)
            if (!palo->axf.empty() && palo->axf[0]->pxfRef)
            {
                bool fOwn = false;
                for (const auto& pxf : palo->axf) fOwn |= pxf.get() == palo->axf[0]->pxfRef;
                if (palo->axf[0]->pxfRef == pxfProxy || !fOwn) palo->axf[0]->pxfRef = nullptr;
            }
        }
        else if (FCidDerivedFrom(plo->Cid(), 3) && static_cast<LOX*>(plo)->pxf == pxfProxy)
            static_cast<LOX*>(plo)->pxf = nullptr;
        // orig: 0x001CAA60's end: an SO goes back to its source's rest pose (PROXYSRC +0x64), still: its
        // position record (+0x58) to the rest position with no velocity, its rotation record (+0x5C) to the
        // rest rotation with no spin. Without it a reused instance (e.g. Bentley's bombs) is placed relative to
        // wherever its last use left it.
        if (FAlo(plo) && static_cast<std::size_t>(j) < we.aposRest.size())
        {
            ALO* palo = static_cast<ALO*>(plo);
            // (native: the first record keeps the placement a carrier or channel left in it (carry.cpp's
            // DetachFromChannel); the original's position/rotation records are absolute after this, so the root
            // goes back to its load-time local first and the rest pose below is the whole placement again)
            if (!palo->axf.empty() && !palo->axf[0]->pxfRef && static_cast<std::size_t>(j) < we.amatRootRest.size())
            {
                palo->axf[0]->mat = we.amatRootRest[j];
                UpdateAloHierarchyWorlds(palo);
            }
            if (PxfPos(palo))
            {
                SetAloPos(palo, we.aposRest[j]);              // vt slot 29 (TranslateToPos)
                SetAloVelocityTo(palo, glm::vec3(0.0f));      // vt slot 35
            }
            if (PxfRot(palo))
            {
                SetAloRotation(palo, we.amatRest[j]);         // vt slot 30 (RotateToMat)
                SetAloAngVelocityTo(palo, glm::vec3(0.0f));   // vt slot 36
            }
            if (std::getenv("SLY2N_ACTDEBUG"))
            {
                const glm::vec3 v = PosAloCenter(palo);
                std::fprintf(stderr, "  detach proxy %d -> %d: rest (%.1f %.1f %.1f) now (%.1f %.1f %.1f) base t (%.1f %.1f %.1f) ref %p\n", pproxy->oid, plo->oid,
                             we.aposRest[j].x, we.aposRest[j].y, we.aposRest[j].z, v.x, v.y, v.z, palo->axf[0]->mat[3][0], palo->axf[0]->mat[3][1],
                             palo->axf[0]->mat[3][2], (void*)palo->axf[0]->pxfRef);
            }
        }
    }
}

// orig 0x001CAEC8 (from 0x1CA8C0 with 2): the freest instance of the proxy's group placed at the proxy
void EnsureRows(WE& we)
{
    const int crow = we.cloLoaded > 0 ? std::min(we.cinst, static_cast<int>(we.aplo.size()) / we.cloLoaded) : 0;
    we.aploRowOwner.resize(static_cast<std::size_t>(std::max(crow, 0)), nullptr);
    we.amatRow.resize(static_cast<std::size_t>(std::max(crow, 0)), glm::mat4(1.0f));
    we.afRowLocked.resize(static_cast<std::size_t>(std::max(crow, 0)), 0);
}

// orig 0x001CADE0 (the proxy's row to use: its own while it keeps one (score >= 3), else the best free one >= nMin)
int IrowProxyChoose(SW* psw, LO* pproxy, int nMin)
{
    auto it = psw->mpproxy.find(pproxy);
    if (it == psw->mpproxy.end() || it->second.iwe < 0) return -1;
    PROXYSTREAM& ps = it->second;
    WE& we = psw->awe[ps.iwe];
    if (we.cloLoaded <= 0) return -1;
    EnsureRows(we);
    const int crow = static_cast<int>(we.aploRowOwner.size());
    if (pproxy->U32Field(0, 0xE4) == 0 && !ps.airow.empty())
        return NScoreRow(we, ps.airow.front()) < 3 ? -1 : ps.airow.front();
    int irow = -1, nBest = nMin;
    for (int i = we.cUsed; i < crow; ++i)
    {
        const int n = NScoreRow(we, i);
        if (n >= nBest) { irow = i; nBest = n; }
    }
    return irow;
}
LO* PloRowFirst(SW* psw, LO* pproxy, int irow);
LO* PloProxyFreeRowImpl(SW* psw, LO* pproxy, int nMin)
{
    const int irow = IrowProxyChoose(psw, pproxy, nMin);
    return irow < 0 ? nullptr : PloRowFirst(psw, pproxy, irow);
}

// orig: 0x001CB5D0 (the ALO's vt slot 27, the pooled object's detach notification: the base's (0x1CB5C8: nothing),
// the physics pose part (0x1F37C8: natively RemoveLo's), then every child's slot 27 (LO +0x64 / +0x24). Natively the
// children were never told: e.g. a guard's carried booty kept its state across the pool (0x13BF30); audit 2026-11 L1)
}  // namespace
void DetachAloBase27(LO* plo, LO* pproxy)
{
    using PFN27 = void (*)(LO*, LO*);
    if (!FAlo(plo)) return;
    const std::vector<LO*> achild = static_cast<ALO*>(plo)->aploChild;  // (a child's notification may change it)
    for (LO* pc : achild)
        if (pc)
            if (PFN27 pfn = reinterpret_cast<PFN27>(PvNativeVt(pc, 27))) pfn(pc, pproxy);
}
NATIVEFN(0x1CB5D0, DetachAloBase27);
namespace
{

int AttachProxy(SW* psw, LO* pproxy, int nMin)
{
    auto it = psw->mpproxy.find(pproxy);
    if (it == psw->mpproxy.end() || it->second.iwe < 0) return -1;
    PROXYSTREAM& ps = it->second;
    WE& we = psw->awe[ps.iwe];
    if (we.cloLoaded <= 0) return -1;
    EnsureRows(we);
    const int crow = static_cast<int>(we.aploRowOwner.size());
    // orig 0x1CADE0
    int irow = -1;
    if (pproxy->U32Field(0, 0xE4) == 0 && !ps.airow.empty())  // (+0xE4: option 1178, no streaming)
    {
        if (NScoreRow(we, ps.airow.front()) < 3) return -1;
        irow = ps.airow.front();
    }
    else
    {
        int nBest = nMin;
        for (int i = we.cUsed; i < crow; ++i)
        {
            const int n = NScoreRow(we, i);
            if (n >= nBest)
            {
                irow = i;
                nBest = n;
            }
        }
    }
    if (irow < 0) return -1;
    if (std::getenv("SLY2N_ACTDEBUG"))
        std::fprintf(stderr, "attach proxy %d group %d row %d (owner %d) objects %d\n", pproxy->oid, ps.iwe, irow,
                     we.aploRowOwner[irow] ? we.aploRowOwner[irow]->oid : -1, we.cloLoaded);
    if (LO* pOwner = we.aploRowOwner[irow]) DetachProxy(psw, pOwner, irow);
    we.aploRowOwner[irow] = pproxy;
    ps.airow.insert(ps.airow.begin(), irow);
    const LOX* pproxyx = static_cast<const LOX*>(pproxy);
    we.amatRow[irow] = pproxyx->mat;
    // the binds: options for the instance's objects (the attach's grf mask 0x2031: 1 methods (the scripts' thunk +0x10,
    // brx.cpp), 0x10 fields, 0x20 setters, as the load's 0x80 / 0x100)
    for (PROXYBIND& bind : ps.abind)
    {
        LO* ploFound = bind.kind == 6 ? we.aplo[static_cast<std::size_t>(irow) * we.cloLoaded]
                                      : PloFindInInstance(we, irow, bind.kind);
        if (!ploFound) continue;
        bind.ploBound = ploFound;  // orig: 0x001CAEC8, bind +0x14
        CBinaryInputStream bis;
        bis.OpenBuffer(std::vector<uint8_t>(bind.ab));
        for (int n; bis.m_ib + 2 <= bind.ab.size() && (n = bis.S16Read()) >= 0;)
        {
            const uint32_t g = g_disc.U32Elf(0x2AC880 + static_cast<uint32_t>(n) * 0x28 + 4) & 0x2031;
            const int grf = (g & 1) | ((g & 0x10) ? 0x80 : 0) | ((g & 0x20) ? 0x100 : 0) | (g & 0x2000);
            OPT opt;
            LoadOptionFromBrx(opt, ploFound, n, grf, &bis);
            if (GUARD* pg = PguardFromLo(ploFound)) SyncGuardOption(pg, ploFound, n);  // (guardstart.cpp)
            ploFound->aopt.push_back(std::move(opt));
        }
        // orig: 0x001CAEC8 / 0x00127B40, 0x00256EE0: streaming binds install their retained frame after
        // the options, when PROXY +0xE4 is zero; keep the object's previous frame as a parent.
        if (bind.pframe && pproxy->U32Field(0, 0xE4) == 0)
        {
            if (ploFound->pframe) bind.pframe->AddParent(ploFound->pframe.get());
            ploFound->pframe = bind.pframe;
        }
    }
    for (int j = 0; j < we.cloLoaded; ++j)
    {
        LO* plo = we.aplo[static_cast<std::size_t>(irow) * we.cloLoaded + j];
        SetLoRadius(psw, plo, pproxy->sRadius);         // vt slot 42
        SetLoParentActivation(psw, plo, pproxy->paloParent);  // vt slot 10 (the proxy's parent and channel)
        SetLoChannel(plo, pproxyx->pxf);
        SetLoMatrix(plo, pproxyx->mat);                 // vt slot 37
        if (std::getenv("SLY2N_ACTDEBUG"))
            std::fprintf(stderr, "  attach proxy %d -> %d (alias %d): mat t (%.1f %.1f %.1f) x (%.2f %.2f %.2f) pxf %p parent %d\n", pproxy->oid,
                         plo->oid, plo->oidInstance, pproxyx->mat[3][0], pproxyx->mat[3][1], pproxyx->mat[3][2], pproxyx->mat[0][0],
                         pproxyx->mat[0][1], pproxyx->mat[0][2], (void*)pproxyx->pxf, pproxy->paloParent ? pproxy->paloParent->oid : -1);
        AddLo(psw, plo);                                // vt slot 8
        if (!plo->fMaskOwn) SetLoMask(psw, plo, pproxy->bMask);  // vt slot 26 (0x1CB4E0)
        {
            // a class's own slot 26 after that base (the guards' reset 0x206518: guardstart.cpp). The ALO's base
            // 0x1CB518 ends with every child's slot 26 (LO +0x64 / +0x24), so the children's own parts run first: e.g.
            // the Ep6 heist's flak shells (50539, cid 0x4F, children of the barrier 50529) need the projectile's
            // 0x13BB80 (out of the parent, in the world) or the script's detonation (method 390) at t 7.26 finds them
            // out of the world and the chopper is never hit. A child that leaves its parent keeps its row's node
            // (LO::ploNsRow), so its row's scripts still find it by name (the Ep8 TNT barrel's charge 18407). L3.
            using PFN26 = void (*)(LO*, LO*);
            std::function<void(LO*)> slot26 = [&](LO* p) {
                if (FAlo(p))
                    for (LO* pc : std::vector<LO*>(static_cast<ALO*>(p)->aploChild))
                        if (pc) slot26(pc);
                if (PFN26 pfn = reinterpret_cast<PFN26>(PvNativeVt(p, 26))) pfn(p, pproxy);
                ResetBreakable(p);  // (cid 0x62's slot 26 0x1BBEA8)
            };
            slot26(plo);
        }
        // CID24 references after the instance's binding options and placement, including owned child uses.
        std::function<void(LO*)> postCarryUse = [&](LO* p) {
            PostLoadCarryUse(p);
            for (LO* child : p->aploOwned) postCarryUse(child);
        };
        postCarryUse(plo);
        // orig: 0x001CAEC8's end: vt slot 3 (14, immediate, one argument) to the proxy with the instance's object, then
        // to the object with the proxy (the scripts keep each other: e.g. Paris' newspapers 138378 remember their
        // dispenser's proxy as 1309 and call its 1311 when broken; a guard's script start, as 0x206170 / 0x206820)
        {
            CRef refObj;
            refObj.SetBasic(plo);
            HandleLoSpliceEvent(pproxy, 14, false, 1, &refObj);
            CRef refProxy;
            refProxy.SetBasic(pproxy);
            HandleLoSpliceEvent(plo, 14, false, 1, &refProxy);
        }
        if (std::getenv("SLY2N_ACTDEBUG") && FAlo(plo))
        {
            const glm::vec3 v = PosAloCenter(static_cast<ALO*>(plo));
            std::fprintf(stderr, "  attached at (%.1f %.1f %.1f)\n", v.x, v.y, v.z);
        }
        if (std::getenv("SLY2N_ACTDEBUG"))
            std::fprintf(stderr, "  object cid 0x%X oid %d: removed %d inworld %d root %d active %d registered %d\n",
                         plo->Cid(), plo->oid, plo->fRemoved, plo->fInWorld, plo->paloParent == nullptr,
                         PloRoot(plo)->fNodeActive, plo->iupd >= 0);
    }
    return irow;
}

LO* PloRowFirst(SW* psw, LO* pproxy, int irow)
{
    auto it = psw->mpproxy.find(pproxy);
    if (irow < 0 || it == psw->mpproxy.end()) return nullptr;
    const WE& we = psw->awe[it->second.iwe];
    return we.cloLoaded == 1 ? we.aplo[static_cast<std::size_t>(irow)] : nullptr;
}

// PROXY's methods and getters (1174..1181)
SW* PswOf(LO*) { return g_psw; }
// The splice wrappers use 0x129A10 even for a null result: null-obj? requires the BASIC tag.
CRef RefLoOrNull(LO* plo) { CRef r; r.SetBasic(plo); return r; }
// orig 0x001CB410: an instance row by its object (one object per instance)
int IrowOfObject(SW* psw, LO* pproxy, const LO* plo)
{
    auto it = psw->mpproxy.find(pproxy);
    if (it == psw->mpproxy.end() || it->second.iwe < 0) return -1;
    const WE& we = psw->awe[it->second.iwe];
    if (we.cloLoaded != 1) return -1;
    for (int i = we.cUsed; i < std::min(we.cinst, static_cast<int>(we.aplo.size())); ++i)
        if (we.aplo[static_cast<std::size_t>(i)] == plo) return i;
    return -1;
}
struct INITPROXYMETHODS
{
    INITPROXYMETHODS()
    {
        RegisterObjectMethod(0x10E248, [](LO* plo, const std::vector<CRef>&) {  // 1174: 0x1CB2C0 (one is free)
            SW* psw = PswOf(plo);
            CRef r;
            auto it = psw->mpproxy.find(plo);
            bool f = false;
            if (it != psw->mpproxy.end() && it->second.iwe >= 0)
            {
                WE& we = psw->awe[it->second.iwe];
                EnsureRows(we);
                for (int i = we.cUsed; i < static_cast<int>(we.aploRowOwner.size()) && !f; ++i) f = NScoreRow(we, i) >= 2;
            }
            r.SetBool(f);
            return r;
        });
        RegisterObjectMethod(0x10E2B8, [](LO* plo, const std::vector<CRef>&) {  // 1175: 0x1CB250(p, 0)
            SW* psw = PswOf(plo);
            LO* po = PloRowFirst(psw, plo, AttachProxy(psw, plo, 2));
            if (std::getenv("SLY2N_HITDEBUG"))  // (debug: the proxies attached by scripts, e.g. a guard's strike)
                std::printf("proxy 1175: t %.3f proxy %d -> %d (cid 0x%X) %p\n", g_clock.t, plo->oid, po ? po->oid : -1, po ? po->Cid() : 0, (void*)po);
            return RefLoOrNull(po);
        });
        RegisterObjectMethod(0x10E328, [](LO* plo, const std::vector<CRef>&) {  // 1177: 0x1CB328
            SW* psw = PswOf(plo);
            auto it = psw->mpproxy.find(plo);
            if (it != psw->mpproxy.end() && !it->second.airow.empty())
                return RefLoOrNull(PloRowFirst(psw, plo, it->second.airow.front()));
            return RefLoOrNull(PloRowFirst(psw, plo, AttachProxy(psw, plo, 1)));
        });
        RegisterObjectMethod(0x10E398, [](LO* plo, const std::vector<CRef>& a) {  // 1179: 0x1CB488 (the row kept)
            SW* psw = PswOf(plo);
            const int i = IrowOfObject(psw, plo, PloArg(a, 0));
            if (i >= 0) { WE& we = psw->awe[psw->mpproxy[plo].iwe]; EnsureRows(we); we.afRowLocked[i] = 1; }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10E420, [](LO* plo, const std::vector<CRef>& a) {  // 1180: 0x1CB4B8
            SW* psw = PswOf(plo);
            const int i = IrowOfObject(psw, plo, PloArg(a, 0));
            if (i >= 0) { WE& we = psw->awe[psw->mpproxy[plo].iwe]; EnsureRows(we); we.afRowLocked[i] = 0; }
            return CRef(TAGK_Void);
        });
        // EOPID 18 (e.g. a guard's option 31; grf 4, getter 0x1CB658): the proxy whose instance the object is (null when
        // none). Ep2's boardroom battle (task 1591, s17 cases 23 / 24) counts a knocked-out guard only when its proxy is
        // one of the wave proxies 106722..106725; unanswered, no wave after the first was ever spawned.
        RegisterObjectGetter(18, [](LO* plo) {
            if (std::getenv("SLY2N_HITDEBUG"))
                std::printf("proxy of %d %p: %d (t %.3f)\n", plo->oid, (void*)plo, PproxyOfInstance(plo) ? PproxyOfInstance(plo)->oid : -1, g_clock.t);
            return RefLoOrNull(PproxyOfInstance(plo));
        });
        RegisterObjectGetter(1176, [](LO* plo) {  // 0x1CB2E0: the latest attached instance's object
            SW* psw = PswOf(plo);
            auto it = psw->mpproxy.find(plo);
            if (it == psw->mpproxy.end() || it->second.airow.empty()) return RefLoOrNull(nullptr);
            return RefLoOrNull(PloRowFirst(psw, plo, it->second.airow.front()));
        });
        // EOPID 1181 (getter 0x1CB360): every attached instance's object, latest first, at most 0x20, as a list;
        // scripts of 5 job starts read it (natively unbound: nil; audit 2026-11 P11)
        RegisterObjectGetter(1181, [](LO* plo) {
            SW* psw = PswOf(plo);
            CRef r(TAGK_None);
            auto it = psw ? psw->mpproxy.find(plo) : decltype(psw->mpproxy.end()){};
            if (!psw || it == psw->mpproxy.end()) return r;
            std::shared_ptr<CPair> head, tail;
            int c = 0;
            for (int irow : it->second.airow)
            {
                if (c++ >= 0x20) break;
                auto pp = PpairNew();
                pp->m_ref = RefLoOrNull(PloRowFirst(psw, plo, irow));
                if (tail) tail->m_ppairNext = pp;
                else head = pp;
                tail = pp;
            }
            if (head) r.SetPair(head);
            return r;
        });
    }
} s_initproxymethods;
}

void PrintActivationDebug(SW* psw)
{
    // debug: SLY2N_PROXYOID=oid: the streaming proxies of the groups with that object, once at frame 60
    if (const char* psz = std::getenv("SLY2N_PROXYOID"))
    {
        static int s_c = 0;
        if (++s_c == 60)
        {
            const OID oid = std::atoi(psz);
            for (auto& [pproxy, ps] : psw->mpproxy)
            {
                if (ps.iwe < 0) continue;
                const WE& we = psw->awe[ps.iwe];
                bool f = false;
                for (int j = 0; j < we.cloLoaded && j < static_cast<int>(we.aplo.size()); ++j) f = f || we.aplo[j]->oid == oid;
                if (!f) continue;
                const glm::vec3 p = PosLoWorld(pproxy);
                std::printf("proxy %d (group %d %s, inst %d used %d, e4 %u): at %.0f %.0f %.0f r %.0f st %d mask 0x%X active %d inworld %d removed %d rows %zu parent %d cm %.0f %.0f %.0f\n",
                            pproxy->oid, ps.iwe, we.StrName().c_str(), we.cinst, we.cUsed, pproxy->U32Field(0, 0xE4), p.x, p.y, p.z, pproxy->sRadius, pproxy->st, pproxy->bMask,
                            PloRoot(pproxy)->fNodeActive, pproxy->fInWorld, pproxy->fRemoved, ps.airow.size(),
                            pproxy->paloParent ? pproxy->paloParent->oid : -1, g_actview.posCm.x, g_actview.posCm.y, g_actview.posCm.z);
            }
        }
    }
    if (std::getenv("SLY2N_TASKREFS"))  // (debug: the volume triggers once: their script and place)
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (const auto& plo : psw->aploAll)
                if (plo->pcls && plo->pcls->PfnOrig(16) == 0x149440)
                {
                    const glm::vec3 p = PosLoWorld(plo.get());
                    std::fprintf(stderr, "trigvol %d cid 0x%X kind %u script %d at %.0f %.0f %.0f inworld %d\n", plo->oid, plo->Cid(),
                                 plo->U32Field(0, 0x1C0, 0), int(plo->U32Field(0, 0x1DC, uint32_t(-1))), p.x, p.y, p.z, plo->fInWorld);
                }
        }
    }
    if (const char* psz = std::getenv("SLY2N_FRAMEPRINT"))  // (debug: "oid@frame": an object's splice frame bindings once)
    {
        static int s_c = 0;
        int oid, n;
        if (std::sscanf(psz, "%d@%d", &oid, &n) == 2 && s_c++ == n)
            for (const auto& plo : psw->aploAll)
                if (plo->oid == oid && (plo->pframe || (std::fprintf(stderr, "frame of %d: none\n", oid), false)))
                    for (CFrame* pf = plo->pframe.get(); pf; pf = pf->m_cpframeParent ? pf->m_apframeParent[0] : nullptr)
                    {
                        std::fprintf(stderr, "frame of %d %p (module %d, %zu bindings):\n", oid, static_cast<void*>(pf), pf->m_fModule, pf->m_avarb.size());
                        for (const VARB& v : pf->m_avarb)
                        {
                            std::string str;
                            if (v.ref.m_tagk == TAGK_Proc && v.ref.m_pproc)
                            {
                                CRef body;
                                body.SetPair(v.ref.m_pproc->m_ppairCodeExpr);
                                PrintRef(body, &str);
                            }
                            else PrintRef(v.ref, &str);
                            std::fprintf(stderr, "  s%d = %s\n", static_cast<int>(v.symid), str.substr(0, 3000).c_str());
                        }
                        if (pf->m_fModule) break;
                    }
    }
    if (const char* psz = std::getenv("SLY2N_FRAMEGREP"))  // (debug: "text@frame": the objects whose frame bindings print the text)
    {
        static int s_c = 0;
        char ach[256] = {};
        int n = 0;
        if (std::sscanf(psz, "%255[^@]@%d", ach, &n) == 2 && s_c++ == n)
            for (const auto& plo : psw->aploAll)
                for (CFrame* pf = plo->pframe.get(); pf; pf = pf->m_cpframeParent ? pf->m_apframeParent[0] : nullptr)
                {
                    for (const VARB& v : pf->m_avarb)
                    {
                        std::string str;
                        if (v.ref.m_tagk == TAGK_Proc && v.ref.m_pproc)
                        {
                            CRef body;
                            body.SetPair(v.ref.m_pproc->m_ppairCodeExpr);
                            PrintRef(body, &str);
                        }
                        else PrintRef(v.ref, &str);
                        if (str.find(ach) != std::string::npos)
                        {
                            const glm::vec3 p = PosLoWorld(plo.get());
                            std::fprintf(stderr, "framegrep %d cid 0x%X inworld %d at %.0f %.0f %.0f: s%d\n", plo->oid, plo->Cid(), plo->fInWorld ? 1 : 0,
                                         p.x, p.y, p.z, static_cast<int>(v.symid));
                            if (std::getenv("SLY2N_FRAMEGREPBODY")) std::fprintf(stderr, "  %s\n", str.substr(0, 3000).c_str());
                        }
                    }
                    if (pf->m_fModule) break;
                }
    }
    if (const char* psz = std::getenv("SLY2N_GLOBPRINT"))  // (debug: "oid@frame": an ALO's globs once)
    {
        static int s_c = 0;
        int oid, n;
        if (std::sscanf(psz, "%d@%d", &oid, &n) == 2 && s_c++ == n)
            for (const auto& plo : psw->aploAll)
                if (plo->oid == oid && (FAlo(plo.get()) || (std::fprintf(stderr, "options of %d:", oid), [&]{ for (const OPT& o : plo->aopt) std::fprintf(stderr, " [eopid %d grf 0x%X n %d]", o.eopid, o.grf, o.aval.empty() ? 0 : o.aval[0].n); std::fprintf(stderr, "\n"); return false; }())))
                {
                    ALO* palo = static_cast<ALO*>(plo.get());
                    std::fprintf(stderr, "options of %d:", oid);
                    for (const OPT& o : plo->aopt)
                        std::fprintf(stderr, " [eopid %d grf 0x%X k %d n %d g %g]", o.eopid, o.grf, o.aval.empty() ? -1 : o.aval[0].k,
                                     o.aval.empty() ? 0 : o.aval[0].n, o.aval.empty() ? 0.0f : o.aval[0].ag[0]);
                    std::fprintf(stderr, "\n");
                    std::fprintf(stderr, "globs of %d (%zu children):", oid, palo->aploChild.size());
                    for (LO* pc : palo->aploChild) std::fprintf(stderr, " %d/0x%X%s", pc->oid, pc->Cid(), pc->fRemoved ? "r" : "");
                    std::fprintf(stderr, "\n");
                    if (palo->pglobset)
                        for (const GLOB& g : palo->pglobset->aglob)
                            std::fprintf(stderr, "  glob n0 %d grf 0x%X hidden %d shown %d r %.0f c (%.0f %.0f %.0f) owner %d\n", g.n0, g.grfGroup, g.fHidden,
                                         FAloGlobShown(palo, g.n0, !(g.grfGroup & 0x800)), g.sRadius, g.posCenter.x, g.posCenter.y, g.posCenter.z, g.oidOwner);
                }
    }
    if (const char* psz = std::getenv("SLY2N_POSPRINT"))  // (debug: "oid,oid,...:N": their world position / heading every N calls)
    {
        static int s_c = 0;
        const char* pszN = std::strchr(psz, ':');
        const int n = pszN ? std::max(1, std::atoi(pszN + 1)) : 60;
        if (s_c++ % n == 0)
            for (const char* q = psz; q && *q && q != pszN; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
            {
                const int oid = std::atoi(q);
                for (const auto& plo : psw->aploAll)
                    if (plo->oid == oid)
                    {
                        const glm::vec3 p = PosLoWorld(plo.get());
                        float ang = 0.0f;
                        if (FAlo(plo.get()))
                            if (const XF* pxf = PxfObj(static_cast<ALO*>(plo.get())))
                                ang = glm::degrees(std::atan2(pxf->matWorld[0][1], pxf->matWorld[0][0]));
                        std::fprintf(stderr, "pos t %.3f %d cid 0x%X at %.0f %.0f %.0f ang %.1f inworld %d", g_clock.t, oid, plo->Cid(), p.x, p.y, p.z, ang, int(plo->fInWorld));
                        if (FCidDerivedFrom(plo->Cid(), 3))
                        {
                            const LOX* plox = static_cast<const LOX*>(plo.get());
                            std::fprintf(stderr, " ref %d local %.0f %.0f %.0f", plox->pxf && plox->pxf->palo ? plox->pxf->palo->oid : -1,
                                         plox->mat[3].x, plox->mat[3].y, plox->mat[3].z);
                        }
                        std::fprintf(stderr, "\n");
                        if (!std::getenv("SLY2N_POSPRINTALL")) break;
                    }
            }
    }
    if (const char* psz = std::getenv("SLY2N_NEARPRINT"))  // (debug: "x y z r@frame": every object near a point once)
    {
        static int s_c = 0;
        float x, y, z, r;
        int n;
        if (std::sscanf(psz, "%f %f %f %f@%d", &x, &y, &z, &r, &n) == 5 && s_c++ == n)
            for (const auto& plo : psw->aploAll)
            {
                const glm::vec3 p = PosLoWorld(plo.get());
                if (glm::length(p - glm::vec3(x, y, z)) < r)
                    std::fprintf(stderr, "near %d (alias %d) cid 0x%X parent %d at %.0f %.0f %.0f inworld %d removed %d active %d\n", plo->oid, plo->oidInstance,
                                 plo->Cid(), plo->paloParent ? plo->paloParent->oid : -1, p.x, p.y, p.z, plo->fInWorld, plo->fRemoved,
                                 PloRoot(plo.get())->fNodeActive);
            }
    }
    if (const char* psz = std::getenv("SLY2N_LOOID"))  // (debug: an object's activation every 120 frames)
    {
        static int s_c = 0;
        if (s_c++ % 120 == 0)
            for (const auto& plo : psw->aploAll)
                if (plo->oid == std::atoi(psz))
                {
                    const glm::vec3 p = PosLoWorld(plo.get());
                    std::fprintf(stderr, "looid %d cid 0x%X: st %d r %.0f mask 0x%X root %d active %d inworld %d removed %d iupd %d queued %d at %.0f %.0f %.0f cm %.0f %.0f %.0f grfCm 0x%X\n",
                                 plo->oid, plo->Cid(), plo->st, plo->sRadius, plo->bMask, PloRoot(plo.get())->oid, PloRoot(plo.get())->fNodeActive,
                                 plo->fInWorld, plo->fRemoved, plo->iupd, plo->fQueued, p.x, p.y, p.z, g_actview.posCm.x, g_actview.posCm.y,
                                 g_actview.posCm.z, g_actview.grfCm);
                }
    }
}

// Low-level attach wrapper: row minimum directly, followed by a single-object result.
LO* PloAttachProxy(SW* psw, LO* pproxy, int nMin) { return PloRowFirst(psw, pproxy, AttachProxy(psw, pproxy, nMin)); }

// orig 0x001CB250: the single-object check precedes row selection and attachment.
LO* PloAcquireProxyObject(SW* psw, LO* pproxy, bool allowActive)
{
    if (!psw || !pproxy) return nullptr;
    const auto it = psw->mpproxy.find(pproxy);
    if (it == psw->mpproxy.end() || it->second.iwe < 0 ||
        it->second.iwe >= static_cast<int>(psw->awe.size())) return nullptr;
    if (psw->awe[it->second.iwe].cloLoaded != 1) return nullptr;
    return PloAttachProxy(psw, pproxy, allowActive ? 1 : 2);
}

LO* PloProxyFreeRow(SW* psw, LO* pproxy, int nMin) { return PloProxyFreeRowImpl(psw, pproxy, nMin); }

// orig 0x001CB488 / 0x001CB4B8: the row of the proxy's instance plo kept (not reused) / given back
void LockProxyRowOf(SW* psw, LO* pproxy, LO* plo, bool fLock)
{
    const int i = IrowOfObject(psw, pproxy, plo);
    if (i < 0) return;
    WE& we = psw->awe[psw->mpproxy[pproxy].iwe];
    EnsureRows(we);
    we.afRowLocked[i] = fLock ? 1 : 0;
}
void UnlockProxyRowOf(SW* psw, LO* pproxy, LO* plo) { LockProxyRowOf(psw, pproxy, plo, false); }
