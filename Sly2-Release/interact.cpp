// interact.cpp: the players' interaction sub-object ACTOR +0x4C0 (interact.h). Ported from the original's code (see
// interact.h); the parts each function warns about once ("interact: ... not ported") are left out.
#include "booty.h"

#include <bit>
#include "interact.h"
#include "hide.h"
#include "detect.h"
#include "guard.h"

#include "actmesh.h"
#include "activate.h"
#include "actor.h"
#include "animfind.h"
#include "alo.h"
#include "asega.h"
#include "aseg.h"
#include "binoc.h"
#include "brx.h"
#include "bsp.h"
#include "blend.h"
#include "climb.h"
#include "clock.h"
#include "cmworld.h"
#include "cm.h"
#include "cplcy.h"
#include "crv.h"
#include "gang.h"
#include "gadget.h"
#include "joy.h"
#include "jt.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "scpt.h"
#include "sly.h"
#include "smrun.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "aseg.h"
#include "task.h"
#include "update.h"
#include "vecmat.h"
#include "xfmath.h"
#include "xfphys.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>

using cmm::F32;

extern SW* g_psw;
bool FJumpPathEntry(OXA* poxaOwn, SOE* psoe);  // jumptgt.cpp (0x20F948)
void PairActorWith(ACTOR* pa, LO* plo, bool f);  // climb.cpp (0x204F78)
glm::vec3 PosActorProbe0(ACTOR* pa);  // jump.cpp (+0x218)

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "interact: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_INTERPRINT") != nullptr;
    return s_f;
}

LO* PloPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return plo;
    return nullptr;
}

// orig 0x0018ED70 (Sly's cane reach in mode 0)
void UpdateSlyReach(SLY* p)
{
    if (p->mode == 0) UpdateReach(&p->reach);  // 0x1CC2E8
}

template <class T>
T* PPart(ALO* palo, uint32_t ib)
{
    return dynamic_cast<T*>(PloPart(palo, ib));
}

constexpr uint32_t GRF_CROSS = 0x40, GRF_CIRCLE = 0x20, GRF_R3 = 0x400;  // (the PADCTL's layout)
constexpr uint32_t IB = 0x4C0;  // the sub-object's offset in the actor (its parts' offsets are IB + its own)

PADCTL* PpadOf(ACTOR* pa)
{
    PODATA* ppd = PpodataFromLo(pa->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
bool FTake(PADCTL* pc, uint32_t grf, float dt) { return pc && GrfPadctlTakePressed(pc, grf, dt); }     // 0x1C46C0
bool FWithin(PADCTL* pc, uint32_t grf, float dt) { return pc && GrfPadctlPressedWithin(pc, grf, dt); }  // 0x1C4660
bool FMainSly(ACTOR* pa) { return PslyFromLo(pa->palo) != nullptr; }  // (the actor == 0x2E1E40, the game's Sly)

// the actor's vt slot 99 (n 3: the binocucom allowed; Sly 0x18D760, Bentley / Murray their abilities)
bool FActorCanDo(ACTOR*, int) { return true; }  // TODO(actors): slot 99's checks (sly.cpp's ground states skip them too)

// an object's vt slot 3 (the base 0x1A61E0: its splice event n, then the game's event 0x1A (5) / 0x1B (6))
void LoEvent(LO* plo, int n)
{
    if (!plo) return;
    HandleLoSpliceEvent(plo, static_cast<uint32_t>(n), false, 0, nullptr);
    if (n == 5 || n == 6) BroadcastTaskEvent(g_psw, n == 5 ? 0x1A : 0x1B, plo);
}



// orig 0x002046F0 (the heading toward an object) / 0x00204CA8 (the turn's angular velocity at once)
void FaceObject(ACTOR* pa, LO* plo)
{
    if (!plo) return;
    const glm::vec3 d = PosJtLo(plo) - PosAloCenter(pa->palo);
    SetStepPhysHeadingToward(pa->palo, d);
}
void TurnNow(ACTOR* pa)
{
    const glm::mat4 m = MatJtLo(pa->palo);
    const float h = std::atan2(m[0][1], m[0][0]);
    glm::vec3 wv = AngVelAlo(pa->palo);
    const STEPPHYS& s = StepPhysOf(pa->palo);
    const cmm::SMP smp{s.vBrake.x, s.vBrake.y, s.vBrake.z};
    float dz = wv.z;
    cmm::RadSmooth(h, s.radHeading, 0.0f, &smp, &dz);  // 0x11D2D0
    wv.z = dz;
    SetAloAngVelocityTo(pa->palo, wv);  // vt slot 36
}

// the description's point for the actor (0x182CD8: the description's method 2 (its parameter) then 3 (the point and
// the facing angle))
glm::vec3 PosInteractDesc(ACTOR* pa, INTERACT& i, float* prad)
{
    i.s20 = GJtDescInit(i.ploTarget, i.jtl, pa);
    return PosJtDesc(i.ploTarget, i.jtl, pa, i.s20, prad);
}

float SMinCrv(const CRV* p) { return p && !p->as.empty() ? p->as.front() : 0.0f; }  // 0x1E6620
float SMaxCrv(const CRV* p) { return p && !p->as.empty() ? p->as.back() : 0.0f; }   // 0x1E6648

ASEGBL* PblOf(ROOTBLEND& rb) { return rb.pbl; }
float GDurationAseg(ASEG* paseg) { return paseg ? PasegdOf(paseg)->tMax : 0.0f; }  // +0x74
float WeightRb(const ROOTBLEND& rb, int k) { return rb.pbl && k < static_cast<int>(rb.pbl->abl.size()) ? rb.pbl->abl[k].weight : 0.0f; }

// an object field holding an object (its oid): the object's own ones first, then the world's
LO* PloOidResolve(LO* plo, OID oid);
LO* PloOidField(LO* plo, uint32_t ib)
{
    if (!plo) return nullptr;
    return PloOidResolve(plo, static_cast<OID>(plo->U32Field(0, ib, 0)));
}

// the object named by an oid seen from plo (its namespace, then its parents', then the world's)
LO* PloOidResolve(LO* plo, OID oid)
{
    if (!plo || oid == 0) return nullptr;
    std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
        for (LO* pc : p->aploOwned)
        {
            if (pc->oid == oid) return pc;
            if (LO* q = find(pc)) return q;
        }
        return nullptr;
    };
    // (the object's namespace, then its parents': a proxied instance's own copy before the world's first, e.g. the
    // hide tables' points, a sibling of the hook in each instance)
    // (an ancestor named by the oid is itself the instance meant: a use's owner (+0x180) is its own parent; the
    // world's first object of that oid can be a removed proxy template)
    for (LO* p = plo; p; p = p->paloParent)
    {
        if (p != plo && p->oid == oid) return p;
        if (LO* q = find(p)) return q;
    }
    return PloFindSwObjectByOid(g_psw, oid);
}

// orig: 0x0020FAA0 (the overlapping active entries, filtered by 0x20F948, at most 256)
std::vector<SOE*> AsoeInteractBox(ACTOR* pa, const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
    OXA* poxaOwn = Poxa(pa->palo);
    std::vector<SOE*> apsoe;
    FlushSwOxaDirty1();  // 0x133E30
    UpdateSwAaox();      // 0x133E80
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y || mx.z < poxa->posMin.z ||
            poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (!FJumpPathEntry(poxaOwn, psoe)) continue;
            if (apsoe.size() < 256) apsoe.push_back(psoe);
        }
    }
    return apsoe;
}

// orig: 0x001911A8 (kind 8: an entry within 200 above the feet, cut not at the segment's start)
bool FInteractCovered(ACTOR* pa)
{
    const glm::vec3 a = PosAloCenter(pa->palo) + glm::vec3(pa->vecBase);
    const glm::vec3 b = a + glm::vec3(0.0f, 0.0f, 200.0f);  // (0x2BF5B0)
    for (SOE* psoe : AsoeInteractBox(pa, a, b))
    {
        LSG alsg[8];
        const int c = ClipSegmentEntry(psoe, a, b, 8, alsg);  // 0x14C6A0
        for (int k = 0; k < c; ++k)
            if (alsg[k].t0 != 0.0f) return true;
    }
    return false;
}

// orig: 0x001911A8 (0x19148C..0x191688: keep crawling while the standing mesh has any contacts)
bool FInteractRoomBlocked(ACTOR* pa)
{
    ALO* palo = pa->palo;
    const int isoe = StepPhysOf(palo).isoeSource;  // STEP +0x268: the original standing entry
    if (isoe < 0 || isoe >= static_cast<int>(palo->asoe.size())) return false;
    SOE* psoeBody = &palo->asoe[isoe];
    UpdateSoeWorld(psoeBody);  // 0x1F38A8 (the source entry is off while the custom crawl mesh is active)
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const glm::vec3& p : psoeBody->geomWorld.apos) mn = glm::min(mn, p);  // 0x11F288
    for (const glm::vec3& p : psoeBody->geomWorld.apos) mx = glm::max(mx, p);  // 0x11F2B8
    const std::vector<SOE*> apsoe = AsoeInteractBox(pa, mn, mx);
    int cmax = 0;
    for (SOE* psoe : apsoe) cmax = std::max(cmax, static_cast<int>(psoe->bsp.x4));
    if (0x1554 < cmax) cmax = 0x1555;
    const glm::vec3 center = mn * 0.5f + mx * 0.5f;
    const glm::vec3 d = center - mn;
    const float radius = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
    std::vector<BSPN> absp;
    for (SOE* psoe : apsoe)
    {
        XP* pxp = nullptr;
        if (psoe->f49)
            MarkSphereBspContacts(psoe, psoeBody, nullptr, &pxp);  // 0x1A9B28 (candidate sphere first)
        else
        {
            absp.clear();
            int cnodeLeft = cmax;
            const int kind = ClipBspPlane(BSPREF(psoe->bsp, psoe->geomWorld), 0, INT_MAX, center, radius,
                                          &cnodeLeft, &absp);  // 0x118A88
            if (kind == 2)
                MarkBspBspContacts(psoeBody, psoe, absp, static_cast<int>(absp.size()) - 1, &pxp);  // 0x1A8B50
        }
        if (pxp)
        {
            FreeSwXpList(pxp);  // 0x1A7E10 (these are only a clearance query)
            return true;
        }
    }
    return false;
}

// orig 0x001940B0 SetJtHfdk
void SetHfdk(ACTOR* pa, int h)
{
    INTERACT& i = pa->inter;
    if (h == i.hfdk) return;
    i.hfdk = h;
    i.tHfdk = g_clock.t;
    i.g50 = 0.0f;
    if (h <= 0) return;
    if (h < 3) i.n34 = pa->plo1024 == i.ploZone && i.ploZone ? 2 : 0;
    else if (h == 3) i.n34 = 1;
}

// orig 0x00184130 (the use's vt slot 51: its user +0x10C, its message 6 (vt slot 4), the state machines of its list
// +0x118 / +0x11C (0x20FF70), the checkpoint's tasks of +0x15C / +0x160 (0x161F48), its event 5, its script +0x114
// started (0x1DFD58)); 0x00184330 (cid 0x24: then Murray takes it, 0x1B7000)
void UseObjectBase(LO* puse, ACTOR* pa)
{
    if (!puse) return;
    if (FSafeHouseDoorUse(puse, pa)) return;   // (QoL, po.h: the walkable safe house's door just opens)
    if (FPrint()) std::printf("interact t %.3f: %d uses %d (cid 0x%X)\n", g_clock.t, pa->palo->oid, puse->oid, puse->Cid());
    puse->SetField(0, 0x10C, static_cast<uint32_t>(pa->palo->oid));  // (+0x10C: the user)
    NotifyLo(puse, 6, puse);  // vt slot 4 (0x133508)
    // 0x20FF70 (the world, +0x118, +0x11C, the use, 1): each entry's state machine run to its trigger state (0x20FFF0)
    const int cRsmg = static_cast<int>(puse->U32Field(0, 0x118));
    for (int k = 0; k < cRsmg; k++)
    {
        const uint32_t ib = 0x11C + 0x10 * k;
        const OID oidRoot = static_cast<OID>(puse->U32Field(0, ib)), oidSm = static_cast<OID>(puse->U32Field(0, ib + 4));
        const OID oidState = static_cast<OID>(puse->U32Field(0, ib + 8));
        LO* proot = PloFindSwNearest(puse, oidRoot);  // (0x15E650 kind 0x204: one found)
        if (!proot || !FIsLoDerivedFrom(proot, 0x30) || oidState == -1) continue;
        for (SMA* psma : g_apsma)  // 0x1F67E0
            if (psma->palo == proot && (oidSm == -1 || (psma->psm && psma->psm->oid == oidSm)))
            {
                if (FPrint()) std::printf("interact: the use %d runs %d's state machine to %d\n", puse->oid, oidRoot, oidState);
                SetSmaGoal(psma, oidState);  // 0x1F0660
                break;
            }
    }
    // 0x161F48 (the task manager, +0x15C, +0x160, the use): the tasks' states
    std::vector<std::pair<OID, int>> atask;
    const int cTask = static_cast<int>(puse->U32Field(0, 0x15C));
    for (int k = 0; k < cTask; k++)
        atask.emplace_back(static_cast<OID>(puse->U32Field(0, 0x160 + 8 * k)), static_cast<int>(puse->U32Field(0, 0x164 + 8 * k)));
    if (!atask.empty()) ApplyCheckpointTasks(g_psw, puse, atask);
    LoEvent(puse, 5);  // vt slot 3
    LO* pscpt = PloOidField(puse, 0x114);
    if (FPrint())
        std::printf("interact: the use %d's script +0x114 oid %d: %d (cid 0x%X), %d machines, %d tasks\n", puse->oid, static_cast<int>(puse->U32Field(0, 0x114, 0)),
                    pscpt ? pscpt->oid : -1, pscpt ? pscpt->Cid() : 0, cRsmg, cTask);
    if (pscpt) StartScpt(pscpt, pa->palo);  // 0x1DFD58
    if (FIsLoDerivedFrom(puse, 0x24) && PmurrayFromLo(pa->palo)) MurrayTakeObject(pa, puse);  // 0x184330 -> 0x1B7000
}
// the use's vt slot 51: the booty's stands (cid 0x25: 0x13D020, booty.cpp, which ends with the base), else the base
void UseObject(LO* puse, ACTOR* pa)
{
    if (FUseBootyStand(puse, pa)) return;
    UseObjectBase(puse, pa);
}

// orig 0x00194118 (the camera zone +0x68's defaults: cmworld.cpp)
void ResetInteractZone(ACTOR* pa) { ResetZoneInteract(pa->inter.ploZone); }

void BindRootBlendIb(ROOTBLEND* prb, ALO* palo, uint32_t ib)
{
    bool fAny = false;
    for (int k = 0; k < 8; ++k)
    {
        prb->apaseg[k] = PPart<ASEG>(palo, ib + 4 + 0x20 * k);
        fAny = fAny || prb->apaseg[k];
    }
    if (fAny) BindRootBlend(prb, palo);  // 0x1A0300
}

void BindGaitIb(GAIT* pgait, ALO* palo, uint32_t ib)
{
    pgait->apaseg[0] = PPart<ASEG>(palo, ib + 0x14);
    pgait->apaseg[1] = PPart<ASEG>(palo, ib + 0x54);
    if (pgait->apaseg[0] || pgait->apaseg[1]) BindGait(pgait);  // 0x20B868
}
}  // namespace

// orig: 0x00140C88 (the current player's camera zone +0x1024: the old one off, the new one on (0x221268))
void SetActorZone(ACTOR* pa, LO* plo)
{
    static const bool s_fPrint = std::getenv("SLY2N_ZONEPRINT") != nullptr;
    if (s_fPrint)
        std::printf("actor zone t %.3f: %d%s %d -> %d (kind %d state %d)\n", g_clock.t, pa->palo->oid,
                    pa->palo != PloPoCurrent(g_psw) ? " (not current)" : "", pa->plo1024 ? pa->plo1024->oid : -1,
                    plo ? plo->oid : -1, pa->n4C8, pa->nSub4CC);
    if (pa->palo != PloPoCurrent(g_psw)) return;
    if (pa->plo1024) SetZoneForced(pa->plo1024, 0);
    pa->plo1024 = plo;
    if (plo) SetZoneForced(plo, 1);
}

// the camera zone of an interaction object's description part (its +0x0: a ledge's / pipe's +0x190, a wall's /
// swing's +0xE0)
LO* PloJtDescZone(LO* plo, JTL jtl)
{
    if (!plo || jtl == JTL_Max) return nullptr;
    return PloOidResolve(plo, static_cast<OID>(JtDescOf(plo, jtl).oidZone));
}

// (for gang.cpp: Murray taking a used object, 0x1B7000)
void FaceActorToward(ACTOR* pa, LO* plo) { FaceObject(pa, plo); }
void TurnActorNow(ACTOR* pa) { TurnNow(pa); }  // orig 0x204CA8
void UseObjectBy(LO* puse, ACTOR* pa) { UseObject(puse, pa); }
void UseObjectBaseBy(LO* puse, ACTOR* pa) { UseObjectBase(puse, pa); }  // (booty.cpp: 0x184130)  // (interactsplice.cpp: the use's method 0x10D5D0)
LO* PloInteractOidField(LO* plo, uint32_t ib) { return PloOidField(plo, ib); }

// orig: 0x00191090
void InitInteract(ACTOR* pa)
{
    INTERACT& i = pa->inter;
    pa->n4C8 = -1;
    pa->nSub4CC = -1;
    pa->n9BC = -1;  // (+0x4C0's +0x4FC is the actor's +0x9BC)
    InitGait(&i.gait180, pa->palo, 0x6F);  // 0x20B840
    InitGait(&i.gait240, pa->palo, 0x6F);
}

// orig: 0x001910F8
void PostLoadInteract(ACTOR* pa)
{
    INTERACT& i = pa->inter;
    ALO* palo = pa->palo;
    // +0x68 = 0x13E3C8(0xF, 0, 0x148, -1): a camera zone for the ledges / walls / swings, off (0x221268(0)); 0x194118
    i.ploZone = PloNew(0x0F, g_psw, nullptr, 0x148, -1);
    InitZoneCamera(i.ploZone);
    SetZoneForced(i.ploZone, 0);
    ResetInteractZone(pa);
    BindRootBlendIb(&i.rb70, palo, IB + 0x70);    // 0x1A0300
    BindRootBlendIb(&i.rb330, palo, IB + 0x330);
    BindGaitIb(&i.gait180, palo, IB + 0x180);     // 0x20B868
    BindGaitIb(&i.gait240, palo, IB + 0x240);
    {
        std::vector<ASEG*> a;
        for (uint32_t ib : {0x2F4u, 0x300u, 0x30Cu})
            if (ASEG* p = PPart<ASEG>(palo, IB + ib)) a.push_back(p);
        if (!a.empty()) i.pbl314 = PblNewPlain(palo, a);  // 0x137A30 (3 entries of 0xC at +0x2F0)
    }
    i.paseg174 = PPart<ASEG>(palo, IB + 0x174);
    i.paseg178 = PPart<ASEG>(palo, IB + 0x178);
    i.paseg17C = PPart<ASEG>(palo, IB + 0x17C);
    i.psm230 = PPart<SM>(palo, IB + 0x230);
    i.paseg234 = PPart<ASEG>(palo, IB + 0x234);
    i.paseg238 = PPart<ASEG>(palo, IB + 0x238);
    i.paseg320 = PPart<ASEG>(palo, IB + 0x320);
    i.paseg324 = PPart<ASEG>(palo, IB + 0x324);
    i.paseg328 = PPart<ASEG>(palo, IB + 0x328);
    i.psm32C = PPart<SM>(palo, IB + 0x32C);
    i.paseg4F0 = PPart<ASEG>(palo, IB + 0x4F0);
    i.paseg4F4 = PPart<ASEG>(palo, IB + 0x4F4);
    i.paseg4F8 = PPart<ASEG>(palo, IB + 0x4F8);
    i.paseg50C = PPart<ASEG>(palo, IB + 0x50C);
    i.paseg508 = PPart<ASEG>(palo, IB + 0x508);
    i.paseg510 = PPart<ASEG>(palo, IB + 0x510);
    i.paseg514 = PPart<ASEG>(palo, IB + 0x514);
    BindReach(&i.reach, pa, &pa->pasega, IB + 0x44C, IB + 0x460, PloPart(palo, IB + 0x474));  // 0x1CC5E8 (+0x1030)
    if (FPrint())
        std::printf("interact %d: rb70 %p rb330 %p gaits %p %p 174 %p 320 %p 324 %p 328 %p sm32C %p 4F8 %p 50C %p 510 %p reach end %p\n", palo->oid,
                    (void*)i.rb70.pbl, (void*)i.rb330.pbl, (void*)i.gait180.pbl, (void*)i.gait240.pbl, (void*)i.paseg174, (void*)i.paseg320,
                    (void*)i.paseg324, (void*)i.paseg328, (void*)i.psm32C, (void*)i.paseg4F8, (void*)i.paseg50C, (void*)i.paseg510,
                    (void*)i.reach.ploEnd);
}

// orig: 0x001928F8
void StartInteract(ACTOR* pa, int n, LO* ploTarget, JTL jtl)
{
    INTERACT& i = pa->inter;
    SetInteractState(pa, -1);
    pa->n4C8 = n;
    i.tBegin = g_clock.t;
    i.jtl = jtl;
    i.ploTarget = ploTarget;
    if (FPrint()) std::printf("interact t %.3f: %d starts kind %d on %d\n", g_clock.t, pa->palo->oid, n, ploTarget ? ploTarget->oid : -1);
    NotifyActorJumpKind(pa, 2);  // vt slot 88 (2): the interaction state (Sly 7, Bentley 3, Murray 13)
    int s = 0;
    switch (n)
    {
    case 0: case 13: s = 5; break;
    case 6:
        if (SLY* p = PslyFromLo(pa->palo))
        {
            p->reach.ploTarget = ploTarget;  // (+0x1854)
            SetSlyMode(p, 0);                // 0x18E540
            UpdateSlyReach(p);               // 0x18ED70
        }
        break;
    case 5: case 9:
        i.reach.ploTarget = i.ploTarget;  // (+0x4C4)
        SetReachState(&i.reach, 0);       // 0x1CCAC0
        break;
    case 8:
        s = 7;
        ForgetGuardsInteract(pa);  // 0x192A28
        break;
    case 1: ForgetGuardsInteract(pa); break;
    case 11: case 12: i.f500 = 1; break;
    default: break;
    }
    SetInteractState(pa, s);
}

// orig: 0x001424E0
void StartPrizeInteract(ACTOR* pa, ASEG* paseg, LO* ploPrize, int n)
{
    pa->inter.paseg50C = paseg ? paseg : pa->inter.paseg508;
    StartInteract(pa, 12, ploPrize, JTL_Max);
    pa->n1318 = n;
}

// orig: 0x00192BD8
void SetInteractState(ACTOR* pa, int n)
{
    INTERACT& i = pa->inter;
    ALO* palo = pa->palo;
    const int old = pa->nSub4CC;
    if (n == old) return;
    SLY* psly = PslyFromLo(palo);
    // ---- leave the old state (0x2A6150)
    switch (old)
    {
    case -1:
    {
        // the action's start: a curve target's point nearest the actor (0x1E6F60: +0x1C its parameter, +0x20 its
        // distance), then the kind's own (0x2A6180)
        if (i.ploTarget && FIsLoDerivedFrom(i.ploTarget, 0x0A))
            if (const CRV* pcrv = PcrvFromLo(i.ploTarget)) NearestCrvAll(pcrv, PosAloCenter(palo), nullptr, nullptr, &i.g1C, &i.s20);
        switch (pa->n4C8)
        {
        case 1:
            // the hiding spot (cid 0x58): its holder +0x190 the actor, occupied (0x183B80(2)); the main Sly's cane
            // (+0x19A0) out of the world (vt slot 9)
            SetHideHolder(i.ploTarget, pa);
            SetHideState(i.ploTarget, 2);
            if (psly && psly == PslyGlobal() && psly->plo19A0) RemoveLo(g_psw, psly->plo19A0);
            i.hfdk = -1;
            break;
        case 2:
            // orig: 0x192EB0: the loop reference belongs to global Sly, even if another actor starts the interaction.
            if (SLY* p = PslyGlobal())
            {
                CSnd* sound = PsndNewPlain(0x5C);
                sound->SetSource(nullptr);
                p->snd2048 = CSndRef(sound);
            }
            i.hfdk = -1;
            break;
        case 3:
        {
            // the rail: its event 5, the actor listening, paired, which way along it the actor faces (+0x24)
            LO* pt = i.ploTarget;
            LoEvent(pt, 5);  // vt slot 3
            AddLoListener(pt, pa, [](int, void*) {});  // 0x1333C0 (TODO(actors): the rail's messages)
            PairActorWith(pa, pt, true);                    // 0x204F78
            i.n28 = -1;
            glm::vec3 dir(0.0f);
            glm::vec3 posFloor(0.0f);
            if (const CRV* pcrv = PcrvFromLo(pt))
            {
                EvalCrv(pcrv, i.g1C, &posFloor, &dir);  // 0x1E67B8 (the position into +0x230)
                pa->posFloor = glm::vec4(posFloor, pa->posFloor.w);
            }
            // the facing: the original flattens and normalizes the 16 bytes at its frame's base (sp+0, not the rail's
            // direction at sp+0x10; nothing on this path writes them: zero in the reference, RAM of railA2dc), so the
            // vector is the fallback (1, 0, 0) (0x2A2DA0) and the test is the actor's x axis' x below -0.2
            const glm::vec3 vFace(1.0f, 0.0f, 0.0f);
            const glm::vec3 x = glm::vec3(MatJtLo(palo)[0]);
            i.f24 = (vFace.x * x.x + vFace.y * x.y) + vFace.z * x.z < F32(0xBE4CCCCD) ? 1 : 0;  // -0.2 (0x2BF63C)
            if (FPrint())
                std::printf("interact: the rail %d at g %.3f s %.1f (%.1f %.1f %.1f) dir %.3f %.3f, the actor's x %.3f %.3f: facing %d\n", pt->oid,
                            i.g1C, i.s20, posFloor.x, posFloor.y, posFloor.z, dir.x, dir.y, x.x, x.y, i.f24);
            i.hfdk = -1;
            break;
        }
        case 4:
        {
            LO* pt = i.ploTarget;
            LoEvent(pt, 5);
            AddLoListener(pt, pa, [](int, void*) {});
            PairActorWith(pa, pt, true);
            pa->posFloor = glm::vec4(PosJtLo(pt), 0.0f);  // +0x230
            i.hfdk = -1;
            break;
        }
        case 11:
            if (i.ploTarget) AddLoListener(i.ploTarget, pa, [](int, void*) {});
            i.hfdk = -1;
            break;
        case 12:
            if (i.ploTarget) FaceObject(pa, i.ploTarget);  // 0x2046F0
            TurnNow(pa);                                    // 0x204CA8
            i.hfdk = -1;
            break;
        default: i.hfdk = -1; break;
        }
        pa->tFloor = g_clock.t;  // +0x20C
        break;
    }
    case 1: pa->n1038 = -1; break;
    case 7:
        if (n < 9 || 10 < n) break;
        // onto the hook's point: placed at its +0x15C less the base, the camera target above it, its heading, no
        // velocity; the camera policy CM +0x4C0
        if (LO* pp = i.ploTarget ? PloOidField(i.ploTarget, 0x15C) : nullptr)
        {
            const glm::vec3 pos = PosJtLo(pp);
            if (FPrint()) std::printf("interact: %d onto the point %d (cid 0x%X) at %.1f %.1f %.1f (target %d at %.1f %.1f %.1f)\n", palo->oid, pp->oid, pp->Cid(), pos.x, pos.y, pos.z, i.ploTarget->oid, PosJtLo(i.ploTarget).x, PosJtLo(i.ploTarget).y, PosJtLo(i.ploTarget).z);
            SetStepPos(pa, pos - glm::vec3(pa->vecBase));                                  // vt slot 29
            pa->posCmTarget = glm::vec4(pos + glm::vec3(0.0f, 0.0f, 1.0f) * pa->sHeight, 0.0f);  // +0x1060
            SetStepPhysHeadingToward(palo, glm::vec3(MatJtLo(pp)[0]));                     // 0x2046B0
            SetStepRotation(pa, StepPhysOf(palo).matHeading);                               // vt slot 30 (+0x1B0)
            SetAloVelocityTo(palo, glm::vec3(0.0f));     // vt slot 35
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));  // vt slot 36
        }
        // orig: 0x193044: the policy argument is the actor's named transform at +0x10C0.
        for (const auto& [ib, pxf] : palo->apartxf)
            if (ib == 0x10C0)
            {
                SetCmPolicy(&g_cm, 10, &g_cmpolicies.cp4C0, palo, pxf);
                break;
            }
        break;
    case 9: case 10:
        if (9 <= n && n < 11) break;
        RevokeCmPolicy(&g_cm, 2, 10, &g_cmpolicies.cp4C0, nullptr, nullptr);  // 0x14E4C8
        break;
    default: break;
    }
    // ---- enter the new one (0x2A61B0)
    ASEG* paseg = nullptr;  // s3
    SM* psm = nullptr;      // s5
    int grfapl = 1;         // s1
    float tStart = 0.0f, svt = 1.0f;  // f20 / f21
    uint32_t grfHandles = 0;          // s6
    bool fAnim = true, fZeroVel = false;
    switch (n)
    {
    case -1:
    {
        // the end: the kind's own (0x2A61E0), the zone given back, the fields cleared
        i.f500 = 0;
        switch (pa->n4C8)
        {
        case 1:
            // the hiding spot free again (0x183B80(0)); the main Sly's cane back in the world (vt slot 8)
            SetHideState(i.ploTarget, 0);
            if (psly && psly == PslyGlobal() && psly->plo19A0) AddLo(g_psw, psly->plo19A0);
            break;
        case 2:
            // orig: 0x193230: stop immediately, then release the global Sly's counted reference.
            if (SLY* p = PslyGlobal(); p && p->snd2048)
            {
                p->snd2048->Stop(0.0f);
                p->snd2048 = CSndRef();
            }
            break;
        case 3: case 4:
            LoEvent(i.ploTarget, 6);                // vt slot 3 (6)
            RemoveLoListener(i.ploTarget, pa);      // 0x133448
            PairActorWith(pa, i.ploTarget, false);       // 0x204F78
            break;
        case 5: case 9: SetReachState(&i.reach, -1); break;  // 0x1CCAC0
        case 6:
            if (psly && psly->mode == 0) SetSlyMode(psly, -1);  // 0x18E540
            break;
        case 11:
            if (i.ploTarget) RemoveLoListener(i.ploTarget, pa);
            pa->pasegAbility = nullptr;
            pa->n9BC = -1;
            break;
        case 12:
            pa->n1318 = -1;
            ReleaseReachObject(&i.reach);  // 0x1CD2C0 (+0x900)
            if (psly) psly->plo1BCC = nullptr;
            break;
        default: break;
        }
        // the camera zone: none, unless it is the target's own (+0x68, or the target's +0x18 desc's +0x0)
        if (pa->plo1024 && (pa->plo1024 == i.ploZone || (i.jtl != JTL_Max && pa->plo1024 == PloJtDescZone(i.ploTarget, i.jtl))))
            SetActorZone(pa, nullptr);  // 0x140C88
        pa->n1038 = -1;
        i.g1C = 0.0f;
        pa->n9BC = -1;  // (+0x4C0's +0x4FC: the actor's +0x9BC)
        pa->n4C8 = -1;
        pa->nSub4CC = -1;
        i.ploTarget = nullptr;
        i.jtl = JTL_Max;
        i.s20 = 0.0f;
        i.n34 = 0;
        fAnim = false;
        break;
    }
    case 0: case 5:
        // the still pose (0x2A6210 by kind)
        grfapl = 1;
        switch (pa->n4C8)
        {
        case 13: paseg = n != 0 ? i.paseg174 : i.paseg510; grfapl = 9; break;
        case 0: paseg = i.paseg174; grfapl = 9; break;
        case 2: paseg = i.paseg514; grfapl = 9; break;
        case 1: psm = i.psm230; grfapl = 1; break;
        case 4: paseg = i.paseg234; grfapl = 9; grfHandles = 7; break;
        case 3:
            if (i.ploTarget && i.ploTarget->U32Field(0, 0x1BC, 0))
            {
                paseg = i.pbl314;
                if (i.pbl314) { i.g318 = 0.0f; i.g31C = 0.0f; }
                else paseg = i.paseg238;
            }
            else paseg = i.paseg238;
            grfapl = 9;
            grfHandles = 0x1F;
            break;
        case 6: case 7: paseg = i.paseg320; grfapl = 9; break;
        case 5: case 9:
            i.reach.ploTarget = i.ploTarget;  // (+0x4C4)
            svt = 0.0f;
            SetReachState(&i.reach, 0);  // 0x1CCAC0
            paseg = i.reach.pbl;          // (+0x470)
            tStart = GDurationAseg(paseg) * 0.5f;
            break;
        case 10:
        {
            PADCTL* pc = PpadOf(pa);
            paseg = pc && 0.5f <= pc->uJoy ? i.paseg4F0 : i.paseg4F4;  // (0x2BF640)
            break;
        }
        case 11:
            // Guttural Roar (+0x9BC 21): Murray's (the global 0x2F7900) part +0x1AF0, else +0x4F8
            paseg = i.paseg4F8;
            if (pa->n9BC == 21 && g_psw && !g_psw->mpcidaplo[0x36].empty())
                if (ASEG* pr = PPart<ASEG>(static_cast<ALO*>(g_psw->mpcidaplo[0x36].front()), 0x1AF0)) paseg = pr;
            break;
        case 12: paseg = i.paseg50C; break;
        default: break;  // 8
        }
        fZeroVel = true;
        break;
    case 1:
        switch (pa->n4C8)
        {
        case 0:
        {
            paseg = PblOf(i.rb70);  // (+0x170)
            grfapl = 9;
            if (pa->plo1024) break;
            LO* pt = i.ploTarget;
            // the target's own zone, else the sub-object's +0x68 set up for it (0x193438..0x193588)
            ZONECM* pzcm = nullptr;
            if (pt && FIsLoDerivedFrom(pt, 0x0C))
            {
                if (LO* pz = PloJtDescZone(pt, JTL_Ledge)) SetActorZone(pa, pz);  // (its description +0x190's +0)
                else
                {
                    // a ledge: the camera along it (+0x10C its path)
                    ResetInteractZone(pa);
                    if ((pzcm = PzonecmFromLo(i.ploZone)))
                    {
                        const CRV* pcrv = PcrvFromLo(pt);
                        pzcm->pcrv = pcrv ? pt : nullptr;
                        pzcm->crvk = pcrv ? pcrv->crvk : 0;
                        pzcm->ccrvPoint = pcrv ? static_cast<int>(pcrv->apos.size()) : 0;
                    }
                    SetActorZone(pa, i.ploZone);
                }
            }
            else if (pt && FIsLoDerivedFrom(pt, 0x22))
            {
                if (LO* pz = PloJtDescZone(pt, FIsLoDerivedFrom(pt, 0x22) ? JTL_Wall : JTL_Swing)) SetActorZone(pa, pz);  // (+0xE0's +0)
                else
                {
                    // a wall: the camera on an arc about its position (+0x12C, +0x150) of its radius +0x10C (+0x160)
                    ResetInteractZone(pa);
                    if ((pzcm = PzonecmFromLo(i.ploZone)))
                    {
                        pzcm->fArc = 1;
                        pzcm->posArc = PosJtLo(pt);  // 0x1A6F00(0, zone, 0x1A62E8(target)): the zone has no frame
                        pzcm->sArcRadius = std::bit_cast<float>(pt->U32Field(0, 0x10C, 0));
                    }
                    SetActorZone(pa, i.ploZone);
                }
            }
            else if (pt && FIsLoDerivedFrom(pt, 0x26))
            {
                if (LO* pz = PloJtDescZone(pt, FIsLoDerivedFrom(pt, 0x22) ? JTL_Wall : JTL_Swing)) SetActorZone(pa, pz);  // (+0xE0's +0)
                else
                {
                    // a swing: the entry's offsets (+0x10 / +0x20 / +0x30) = D_002BF650 (225, 0, 125) in its frame
                    ResetInteractZone(pa);
                    if ((pzcm = PzonecmFromLo(i.ploZone)) && pzcm->pacmeShared)
                    {
                        const glm::vec3 v = glm::mat3(MatJtLo(pt)) * glm::vec3(F32(0x43610000), 0.0f, F32(0x42FA0000));
                        pzcm->pacmeShared->acoef[0].x = v.x;
                        pzcm->pacmeShared->acoef[1].x = v.y;
                        pzcm->pacmeShared->acoef[2].x = v.z;
                    }
                    SetActorZone(pa, i.ploZone);
                }
            }
            break;
        }
        case 13: paseg = PblOf(i.rb70); grfapl = 9; break;
        default: break;  // (the state only: no clip)
        }
        break;
    case 2: paseg = i.paseg178; fZeroVel = true; break;
    case 3: paseg = i.paseg17C; fZeroVel = true; break;
    case 4:
        switch (pa->n4C8)
        {
        case 2:
            if (psly) { ResetGaitFeet(&psly->gaitRun); paseg = psly->gaitRun.pbl; }  // (+0x1C30 / +0x1C34)
            grfapl = 9;
            break;
        case 1: ResetGaitFeet(&i.gait180); paseg = i.gait180.pbl; grfapl = 9; break;
        case 3: ResetGaitFeet(&i.gait240); paseg = i.gait240.pbl; grfapl = 9; break;
        default: break;  // (no clip)
        }
        break;
    case 6: break;
    case 7:
        // the hook's start: facing its point's x row, no velocity, its event 5
        paseg = i.paseg324;
        if (LO* pp = i.ploTarget ? PloOidField(i.ploTarget, 0x15C) : nullptr) SetStepPhysHeadingToward(palo, glm::vec3(MatJtLo(pp)[0]));  // 0x2046B0
        pa->vecVel = glm::vec3(0.0f);
        LoEvent(i.ploTarget, 5);  // vt slot 3
        pa->nSub4CC = n;
        i.tState = g_clock.t;
        goto play;
    case 8:
        paseg = i.paseg328;
        pa->vecVel = glm::vec3(0.0f);
        LoEvent(i.ploTarget, 6);
        pa->nSub4CC = n;
        i.tState = g_clock.t;
        goto play;
    case 9:
        psm = i.psm32C;
        if (old == 7) WarnOnce("kind 8's parts hidden (the flag 0x10 of the actor's +0x40 and Sly's +0x19A0 / +0x19A4)");
        fZeroVel = true;
        break;
    case 10:
        pa->n1038 = -1;
        paseg = PblOf(i.rb330);  // (+0x430)
        grfapl = 9;
        fZeroVel = true;
        break;
    default: break;
    }
    if (fZeroVel) pa->vecVel = glm::vec3(0.0f);  // +0x190 (0x2A2DD0)
    if (n != -1) pa->nSub4CC = n;
    i.tState = g_clock.t;
    if (n == -1 || !fAnim) return;
play:
    // the animation and the state machine
    RetractAsegaPp(&pa->pasega);   // 0x130658
    ClearSmaHandle(&pa->psma);     // 0x1F15B8
    ApplyAsegCur(paseg, palo, tStart, svt, grfapl, &pa->pasega);  // 0x1305B8
    ReplaceSmaHandle(psm, palo, -1, grfapl, &pa->psma);           // 0x1F1448
    UpdateActorStepMesh(pa);                                      // vt slot 90
    if (psly) SetSlyHandles(psly, grfHandles);  // 0x18C120 (Sly, cid 0x35: the state's handles, none removes them)
    if (FPrint())
        std::printf("interact t %.3f: %d kind %d state %d -> %d (clip %d sm %d)\n", g_clock.t, palo->oid, pa->n4C8, old, n, paseg ? paseg->oid : -1,
                    psm ? psm->oid : -1);
}

// orig: 0x001911A8
void UpdateInteractTransitions(ACTOR* pa)
{
    INTERACT& i = pa->inter;
    PADCTL* pc = PpadOf(pa);
    for (int iter = 0; iter < 32; ++iter)
    {
        const int sOld = pa->nSub4CC;
        bool fReturn = false;
        auto toGround = [&](int k) { NotifyActorJumpKind(pa, k); };  // vt slot 88
        auto binoc = [&]() {
            if (!FWithin(pc, GRF_R3, 0.15f) || !FActorCanDo(pa, 3)) return false;
            ForgetPadctlPresses(pc, GRF_R3);  // 0x1C4738
            OpenBinoc(pa, 0, nullptr, nullptr);
            return true;
        };
        switch (pa->n4C8)
        {
        case 1:
            if (0.15f < g_clock.t - i.tState && FTake(pc, GRF_CROSS, 0.25f))
            {
                SetJumpKind(&pa->jump, 7, 0, i.ploTarget);  // 0x195A00 (the jump out of the hiding spot)
                break;
            }
            binoc();
            break;
        case 3: case 4:
            if (FTake(pc, GRF_CROSS, 0.1f)) { SetJumpKind(&pa->jump, 1, 0, 0); break; }
            binoc();
            break;
        case 7:
            // the use: back to the ground, the object's vt slot 51 with the actor
        {
            LO* puse = i.ploTarget;  // (s0, before the state's end clears it)
            toGround(0);
            UseObject(puse, pa);
        }
            break;
        case 8:
            if (sOld == 7)
            {
                if (pa->pasega && FAsegaRetracted(pa->pasega)) SetInteractState(pa, 9);  // 0x131968
                break;
            }
            if (sOld == 8)
            {
                if (pa->pasega && FAsegaRetracted(pa->pasega)) toGround(0);
                break;
            }
            // Still under cover, or no room for the standing body: retain the crawl. Only clear space exits.
            if (!FInteractCovered(pa) && !FInteractRoomBlocked(pa)) SetInteractState(pa, 8);
            break;
        case 9:
        {
            // the reach's end object within 25 of the target: the reach's grab (0x1CD098), back to the ground
            const glm::vec3 a = i.ploTarget ? PosJtLo(i.ploTarget) : glm::vec3(0.0f);
            const glm::vec3 b = i.reach.ploEnd ? PosJtLo(i.reach.ploEnd) : glm::vec3(0.0f);
            const glm::vec3 d = a - b;
            if (std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z) < 25.0f)  // 0x2BF5B4
            {
                WarnOnce("kind 9's grab 0x1CD098");
                toGround(0);
            }
            break;
        }
        case 10:
            if (!pa->pasega || FAsegaRetracted(pa->pasega)) { toGround(0); break; }
            if (F32(0x3EB33333) < g_clock.t - i.tBegin)  // 0.35 (0x2BF5B8)
            {
                if ((pc && 0.0f < pc->uJoy) || (pc && (pc->grfbtn & 0xD0)))
                {
                    toGround(0);
                    fReturn = true;
                }
            }
            break;
        case 11: case 12:
            if (!pa->pasega || FAsegaRetracted(pa->pasega)) toGround(0);
            break;
        case 5:
            // the reach near a pipe: climb it (state 8 at the distance +0x20) when 0x1423A0 allows
            if (!i.ploTarget || !FIsLoDerivedFrom(i.ploTarget, 0x0B)) break;
            WarnOnce("kind 5's climb test 0x1423A0");
            break;
        case 2:
            // orig: 0x191824: depleted power and target kind 4 precede the lock/held-button tests.
            if (NActorPower(pa) <= 0 || pa->n10EC == 4) toGround(0);
            else if (!(g_grfjoyLock & 1) && !(pc && (pc->grfbtn & GrfAbilityButton(pa, 40)))) toGround(0);
            break;
        default:
            // 0, 6, 13: CROSS jumps off; R3 the binocucom (kinds 0 / 13); else back to the ground unless held (+0x500),
            // paused, or circle held
            if (FTake(pc, GRF_CROSS, 0.1f)) { SetJumpKind(&pa->jump, 1, 0, 0); break; }
            if (FWithin(pc, GRF_R3, 0.15f) && FActorCanDo(pa, 3) && (pa->n4C8 == 0 || pa->n4C8 == 13))
            {
                ForgetPadctlPresses(pc, GRF_R3);
                OpenBinoc(pa, 0, nullptr, nullptr);
                break;
            }
            if (i.f500) break;
            // (0x2DFBC4 & 1: the game paused)
            if (pc && (pc->grfbtn & GRF_CIRCLE)) break;
            toGround(0);
            break;
        }
        if (fReturn) return;
        if (pa->nSub4CC == sOld || pa->nSub4CC == -1) return;
    }
}

// orig: 0x00191A10
void UpdateInteract(ACTOR* pa)
{
    INTERACT& i = pa->inter;
    ALO* palo = pa->palo;
    PADCTL* pc = PpadOf(pa);
    const glm::vec3 pos = PosAloCenter(palo);
    switch (pa->n4C8)
    {
    case 0: case 13:
    {
        // toward the description's point (flat), at the sidestep clip's speed; the stick along the facing moves sideways
        float rad = 0.0f;
        glm::vec3 posT = PosInteractDesc(pa, i, &rad);  // 0x182CD8
        const float svt = pa->pasega ? pa->pasega->svt : 1.0f;
        const glm::vec3 v2 = pa->n1040 == 1 ? glm::vec3(i.rb70.avel[2]) : glm::vec3(i.rb70.avel[4]);  // (+0xC0 / +0x100)
        const float sv = std::sqrt((v2.x * v2.x + v2.y * v2.y) + v2.z * v2.z) * svt;
        posT.z = pos.z;
        const cmm::SMP smp{sv, 0.0f, F32(0x3E99999A)};
        glm::vec3 v40(0.0f);
        cmm::PosSmooth(pos, posT, g_clock.dt, &smp, &v40);  // 0x11F088
        glm::vec3 v1(0.0f);
        float f21 = 0.0f;
        if (!(i.ploTarget && FIsLoDerivedFrom(i.ploTarget, 0x26)))
        {
            const float radSide = pa->n1040 == 1 ? rad - F32(0x3FC90FDB) : rad + F32(0x3FC90FDB);
            const glm::vec3 a = cmm::SetVectorCylind(radSide, 1.0f, 0.0f);  // 0x11E1A0
            const glm::vec3 b = cmm::SetVectorCylind(pc ? pc->radJoy : 0.0f, 1.0f, 0.0f);
            float f3 = ((a.x * b.x + a.y * b.y) + a.z * b.z) * 1.5f;  // (0x2BF5BC)
            if (f3 < -1.0f) f3 = -1.0f;
            else if (1.0f < f3) f3 = 1.0f;
            f3 *= pc ? pc->uJoy : 0.0f;
            if (std::fabs(f3) < F32(0x3E4CCCCD)) f3 = 0.0f;
            const float l2 = (v40.x * v40.x + v40.y * v40.y) + v40.z * v40.z;
            f21 = f3 * sv;
            // orig: 0x00191CA8 takes the square root of the magnitude. The
            // correction speed can round just above sv at the start of a shimmy.
            f21 = std::min(std::sqrt(std::fabs(sv * sv - l2)), f21);
            float g = f21;
            LO* pt = i.ploTarget;
            if (pt && FIsLoDerivedFrom(pt, 0x0C) && !pt->U32Field(0, 0xE4, 0))
            {
                // an open ledge: not past its ends (30 in)
                const CRV* pcrv = PcrvFromLo(pt);
                float vLo = 0.0f, vHi = 0.0f;
                GSmooth(i.s20, SMinCrv(pcrv) + 30.0f, g_clock.dt, reinterpret_cast<const SMP*>(&smp), &vLo);  // 0x11D018
                GSmooth(i.s20, SMaxCrv(pcrv) - 30.0f, g_clock.dt, reinterpret_cast<const SMP*>(&smp), &vHi);
                if (f21 < vLo) g = vLo;
                else if (vHi < f21) g = vHi;
            }
            v1 = a * g;
        }
        // the velocity in the heading's frame (+0x190 its negation)
        const glm::vec3 v = v40 + v1;
        const glm::mat3& mh = pa->matHeading;
        pa->vecVel = -glm::vec3(glm::dot(mh[0], v), glm::dot(mh[1], v), glm::dot(mh[2], v));
        const float lv = glm::length(pa->vecVel);
        int s = 0;
        if (25.0f < lv)
        {
            s = 1;
            if (pa->n4C8 == 0)
            {
                const float l40 = std::sqrt((v40.x * v40.x + v40.y * v40.y) + v40.z * v40.z);
                if (45.0f < l40) SetHfdk(pa, 0);
                else if (20.0f < pa->vecVel.y) SetHfdk(pa, pa->n1040 == 1 ? 1 : 2);
                else if (pa->vecVel.y < -20.0f) SetHfdk(pa, pa->n1040 != 1 ? 1 : 2);
            }
        }
        else
        {
            LO* pt = i.ploTarget;
            if (pt && FIsLoDerivedFrom(pt, 0x0C))
            {
                const uint32_t ends = pt->U32Field(0, 0x1BC, 0);
                if (f21 < -25.0f && (ends & 1)) { if (pa->n4C8 == 0) s = pa->n1040 ? 2 : 3; else s = 2; }
                else if (25.0f < f21 && (ends & 2)) { if (pa->n4C8 == 0) s = pa->n1040 ? 3 : 2; else s = 3; }
            }
            if (pa->n4C8 == 0) SetHfdk(pa, 3);
        }
        SetStepPhysHeading(palo, rad);  // 0x204660
        if (pa->nSub4CC == 5)
        {
            const glm::mat4 m = MatJtLo(palo);
            const float h = std::atan2(m[0][1], m[0][0]);  // 0x204740
            if (0.25f < std::fabs(cmm::RadNormalize(h - rad))) s = 5;
            if (FPrint()) std::printf("interact t %.3f: state 5 heading %.3f goal %.3f step %.3f -> %d\n", g_clock.t, h, rad, RadStepHeading(PstepFromLo(palo)), s);
        }
        SetInteractState(pa, s);
        break;
    }
    case 1:
        if (i.ploTarget && FHideFollows(i.ploTarget) && pc && 0.0f < pc->uJoy && i.gait180.pbl)
        {
            SetInteractState(pa, 4);
            pa->vecVel = glm::vec3(SetGaitMix(&i.gait180, pc->uJoy));  // 0x20BB58
            SetStepPhysHeading(palo, pc->radJoy);
            UpdateGaitFeet(&i.gait180);  // 0x20BC08
        }
        else SetInteractState(pa, 0);
        break;
    case 2:
    {
        DrainActorPower(pa, 3.0f);  // orig: 0x192060 / D_002BF5C0
        SLY* p = PslyFromLo(palo);
        const bool fGo = pa->nSub4CC == 0 ? (pc && 0.1f < pc->uJoy) : (pc && 0.0f < pc->uJoy);
        if (!fGo || !p) { SetInteractState(pa, 0); break; }
        SetInteractState(pa, 4);
        pa->vecVel = glm::vec3(SetGaitMix(&p->gaitRun, pc->uJoy * 0.75f));  // (0x2BF5C4)
        SetStepPhysHeading(palo, pc->radJoy);
        break;
    }
    case 3:
    {
        // the rail: the nearest point from the last (0x1E6ED8 of the probe point +0x218: +0x1C / +0x20); off the ends of
        // an open sliding rail falls (the jump's kind 0); the stick along the rail (or its guide curve +0x1C8) picks
        // the way (+0x28: -1 still, 0 along, 1 back; +0x24 the facing), the gait +0x240's mix up to the smoothed end
        // speed (none on a sliding rail +0x1BC); state 4 walking (0x20BB58, 0x204660, 0x20BC08) or 0 facing
        LO* prail = i.ploTarget;
        const CRV* pcrv = prail ? PcrvFromLo(prail) : nullptr;
        if (!pcrv) break;
        glm::vec3 pos(0.0f), dir(0.0f);
        NearestCrv(pcrv, PosActorProbe0(pa), i.g1C, &pos, &dir, &i.g1C, &i.s20);
        const bool fSlide = prail->U32Field(0, 0x1BC, 0) != 0;
        if (fSlide && !pcrv->fClosed && !pcrv->ag.empty())
            if (i.g1C < pcrv->ag.front() + F32(0x3C23D70A) || pcrv->ag.back() - F32(0x3C23D70A) < i.g1C)
            {
                SetJumpKind(&pa->jump, 0, 0, 0);  // 0x195A00
                break;
            }
        auto flat = [](glm::vec3 v) {
            v.z = 0.0f;
            const float l = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
            return l < F32(0x38D1B717) ? glm::vec3(0.0f) : v * (1.0f / l);
        };
        dir = flat(dir);
        float rad = std::atan2(dir.y, dir.x);
        const glm::vec3 stick = cmm::SetVectorCylind(pc ? pc->radJoy : 0.0f, 1.0f, 0.0f);  // 0x11E1A0
        glm::vec3 guide = dir;
        if (LO* pguide = PloOidField(prail, 0x1C8))
            if (const CRV* pcrvG = PcrvFromLo(pguide))
            {
                NearestCrvAll(pcrvG, PosActorProbe0(pa), nullptr, &guide, nullptr, nullptr);  // 0x1E6F60
                guide = flat(guide);
            }
        float g = (stick.x * guide.x + stick.y * guide.y) + stick.z * guide.z;
        const float u = pc ? pc->uJoy : 0.0f;
        switch (i.n28)
        {
        case 0:
            if (u < 0.25f) i.n28 = -1;
            else if (g < F32(0xBF333333)) i.n28 = 1;
            break;
        case 1:
            if (u < 0.25f) i.n28 = -1;
            else if (F32(0x3F333333) < g) i.n28 = 0;
            break;
        case -1:
            if (F32(0x3EB33333) < u) i.n28 = 0.0f <= g ? 0 : 1;
            break;
        default: break;
        }
        if (i.n28 == 0) i.f24 = 0;
        else if (i.n28 == 1) i.f24 = 1;
        if (fSlide) g = 0.0f;
        else
        {
            const GAIT& gait = i.gait240;
            const SMP smp{gait.avel[1].x, gait.avel[0].x, F32(0x3E4CCCCD)};  // (0x2BF5C8)
            float vMax = gait.avel[1].x;
            if (!pcrv->fClosed)
            {
                if (i.f24) { GSmooth(i.s20, 50.0f, g_clock.dt, &smp, &vMax); vMax = -vMax; }  // 0x11D018
                else GSmooth(i.s20, SLengthCrv(pcrv) - 50.0f, g_clock.dt, &smp, &vMax);
            }
            const float uMax = (vMax - gait.avel[0].x) / (gait.avel[1].x - gait.avel[0].x);
            g = u < 0.0f ? 0.0f : (uMax < u ? uMax : u);
        }
        if (i.f24) rad = cmm::RadNormalize(rad + F32(0x40490FDB));  // 0x11CF60
        if (0.0f < g)
        {
            SetInteractState(pa, 4);
            pa->vecVel = glm::vec3(SetGaitMix(&i.gait240, g));  // 0x20BB58
            SetStepPhysHeading(palo, rad);                       // 0x204660
            UpdateGaitFeet(&i.gait240);                          // 0x20BC08
        }
        else
        {
            SetInteractState(pa, 0);
            SetStepPhysHeading(palo, rad);
        }
        break;
    }
    case 4:
        if (pc) SetStepPhysHeading(palo, pc->radJoy);
        break;
    case 5: case 9: UpdateReach(&i.reach); break;  // 0x1CC2E8
    case 8:
    {
        const int st = pa->nSub4CC;
        if (st == 7 || st == 8) break;
        if (pc && 0.0f < pc->uJoy)
        {
            // the walk: forward / back by the stick's y (entry 1's speed), sideways by its x (entry 3's, else 2's)
            SetInteractState(pa, 10);
            const glm::vec3 vSide = i.rb330.apaseg[3] ? glm::vec3(i.rb330.avel[3]) : glm::vec3(i.rb330.avel[2]);
            const glm::vec3 vFwd(i.rb330.avel[1]);
            const float lSide = std::sqrt(vSide.x * vSide.x + vSide.y * vSide.y);
            const float lFwd = std::sqrt(vFwd.x * vFwd.x + vFwd.y * vFwd.y);
            pa->vecVel = glm::vec3(-pc->y * lSide, pc->x * lFwd, 0.0f);
            SteerActorRootBlend(pa, &i.rb330);  // 0x1A0450 (+0x430)
            // the sideways part turns the actor (55 per radian)
            const glm::vec3 ax(1.0f, 0.0f, 0.0f);
            const float h = StepPhysOf(palo).radHeading + (-pa->vecVel.y / 55.0f) * g_clock.dt;  // (0x2BF5CC)
            pa->vecVel = ax * glm::dot(pa->vecVel, ax);
            SetStepPhysHeading(palo, h);  // 0x204660
        }
        else SetInteractState(pa, 9);
        // 0x191A10: crawling follows the plane through the original three probes.
        // Sly's points (D_002BF5D0) sit lower than the other actors' (D_002BF600).
        static const glm::vec3 slyProbes[3] = {{0.0f, 0.0f, -50.0f}, {45.0f, -40.0f, -50.0f}, {45.0f, 40.0f, -50.0f}};
        static const glm::vec3 actorProbes[3] = {{0.0f, 0.0f, -15.0f}, {45.0f, -40.0f, -15.0f}, {45.0f, 40.0f, -15.0f}};
        FitStepToFloor(palo, -50.0f, 0.0f, 3, FMainSly(pa) ? slyProbes : actorProbes);  // D_002BF630

        break;
    }
    case 10:
    {
        // orig: 0x192740..0x1927B4 (part of 0x191A10): the current clip's
        // root-channel derivative, sampled at its effective time/speed and reversed.
        pa->vecVel = glm::vec3(0.0f);
        if (ASEGA* clip = pa->pasega)
            if (const ASEGCHN* channel = PchnFindAseg(clip->paseg, palo, 1, true); channel && channel->ptrkp)
            {
                glm::vec4 velocity(0.0f);
                channel->ptrkp->Eval(nullptr, &velocity, clip->t, clip->svt);
                pa->vecVel = -glm::vec3(velocity);
            }
        break;
    }
    case 11:
        if (i.ploTarget)
        {
            FaceObject(pa, i.ploTarget);  // 0x2046F0
            TurnNow(pa);                   // 0x204CA8
        }
        break;
    default: break;
    }
    // the states' ends
    const int st = pa->nSub4CC;
    if (st == 1)
    {
        SteerActorRootBlend(pa, &i.rb70);  // 0x1A0450 (+0x170)
        if (pa->plo1024 == i.ploZone && i.ploZone && 0 < i.hfdk && i.hfdk < 3)
            i.g50 = std::max(i.g50, WeightRb(i.rb70, i.hfdk == 1 ? 1 : 0));  // (+0x90 / +0x70: the entries' weights)
    }
    else if (st == 0 && pa->n4C8 == 3)
        WarnOnce("kind 3's arms (0x189C40 / 0x193C98)");
}

// orig: 0x00192A28
void ForgetGuardsInteract(ACTOR* pa)
{
    // the guards (list 0x3E1444) whose detector +0xF64 sees the actor (its entry's state not 0, the detector not
    // fixed by its flag 1) forget it (0x154080(0)) when farther than 1500 (0x2BF638) or when something solid is on
    // the line between them (0x20FAA0 with the filter 0x154568, 0x14CC58)
    if (!g_psw || !pa) return;
    const glm::vec3 b = PosAloCenter(pa->palo);  // 0x1A62E8
    // 0x154080 sends script event28. It can unregister guards; locate the next
    // member after the callback instead of retaining a vector iterator. Original
    // 0x11A2E8 clears a removed current member's next link, ending this traversal.
    const auto nextGuard = [](GUARD* current) -> GUARD* {
        const auto& active = ApguardActive();
        if (!current) return active.empty() ? nullptr : active.front();
        auto it = std::find(active.begin(), active.end(), current);
        return it == active.end() || ++it == active.end() ? nullptr : *it;
    };
    for (GUARD* pg = nextGuard(nullptr); pg; pg = nextGuard(pg))
    {
        LO* plo = pg->palo;
        DETECTOR* pdet = pg ? pg->pdet : nullptr;
        if (!pdet) continue;
        DETENTRY* pe = PdetentryFind(pdet, pa->palo);
        if (!pe || !pe->st || (pdet->grf & 1)) continue;
        const glm::vec3 a = PosAloCenter(static_cast<ALO*>(plo));
        const glm::vec3 d = a - b;
        const float dist = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
        if (F32(0x44BB8000) < dist)
        {
            SetDetentryState(pdet, pe, 0);
            continue;
        }
        // 0x154568: not a sensor (bit 48), not the guard's own (its +0x14 group: natively its hierarchy), not under
        // a stepping object (0x33) (TODO(ai): the entry owner's +0xEC +0x70 bit 2)
        const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
        std::vector<SOE*> apsoe;
        for (OXA* poxa : g_physw.apoxaActive)
        {
            if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
                mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
                continue;
            for (SOE* psoe : poxa->apsoe)
            {
                if (psoe->f48 || !psoe->palo || PloRoot(psoe->palo) == PloRoot(plo)) continue;
                bool fStep = false;
                for (const LO* p = psoe->palo; p && !fStep; p = p->paloParent) fStep = FCidDerivedFrom(p->Cid(), 0x33);
                if (!fStep && apsoe.size() < 0x100) apsoe.push_back(psoe);
            }
        }
        LSG lsg;
        if (apsoe.empty() || !PsoeHitTestLineObjects(0, a, b, apsoe, &lsg)) continue;  // 0x14CC58
        SetDetentryState(pdet, pe, 0);
    }
}

// orig: 0x001938C0
bool FInteractCircle(ACTOR* pa)
{
    if (std::getenv("SLY2N_GANGDEBUG"))
    {
        std::printf("interact circle t %.3f: %d tracked:", g_clock.t, g_jttrack.c);
        for (int k = 0; k < g_jttrack.c; ++k) std::printf(" %d (list %d, %.1f)", g_jttrack.ae[k].plo->oid, g_jttrack.ae[k].jtl, g_jttrack.ae[k].dist);
        std::printf("\n");
    }
    for (int k = 0; k < g_jttrack.c; ++k)
    {
        const JTENT& e = g_jttrack.ae[k];
        if (250.0f < e.dist) break;  // (0x2BF660)
        LO* plo = e.plo;
        int n = -1;
        if (FIsLoDerivedFrom(plo, 0x13) || FIsLoDerivedFrom(plo, 0x0B)) n = FMainSly(pa) ? 6 : 5;
        else if (FIsLoDerivedFrom(plo, 0x1B)) n = 8;
        else if (FIsLoDerivedFrom(plo, 0x0C)) n = 0;
        else if (FIsLoDerivedFrom(plo, 0x22)) n = 0;
        else if (FIsLoDerivedFrom(plo, 0x26)) n = static_cast<int>(plo->U32Field(0, 0x10C, 0));
        else if (FIsLoDerivedFrom(plo, 0x23)) n = 7;
        if (n != -1)
        {
            StartInteract(pa, n, plo, e.jtl);
            return true;
        }
    }
    return false;
}


// orig: 0x00142140 / 0x00142340
void SetActorInteractObject(ACTOR* pa, int n, LO* plo)
{
    if (plo) StartInteract(pa, n, plo, JTL_Max);  // 0x1928F8
    else if (pa->n4C8 == n) NotifyActorJumpKind(pa, 0);  // vt slot 88 (0)
}

// orig: 0x00193DE8
void AddInteractContacts(ACTOR* pa)
{
    INTERACT& i = pa->inter;
    if (pa->n4C8 != 3 && pa->n4C8 != 4) return;
    // the players' vt slot 82 (0x140C30) first: a held object out of the world drops the actor (0x195A00 with 0), as
    // AddClimbContacts / AddJumpLedgeContacts do. R41: Ep3's water buckets remove their spire 9332 (ASEG 98042, the
    // bottom opening at the top of the loop), which in the original throws Sly out; natively he rode on for good.
    if (i.ploTarget && !i.ploTarget->fInWorld)
    {
        SetJumpKind(&pa->jump, 0, 0, 0);
        return;
    }
    if (pa->n4C8 == 4)
    {
        // the spire: its point held by the line contacts 0x23 (vt slot 82 0x140C30 -> 0x205008)
        if (i.ploTarget) AddActorLineXps(pa, 0x23, PosJtLo(i.ploTarget) - pa->vecDrawOffset, glm::vec3(0.0f, 1.0f, 0.0f), PosActorProbe0(pa));
        return;
    }
    // the rail: the nearest point from the last (+0x1C / +0x20), held along the rail's line (grf 0x22: the axes across
    // it; within 50 of the end it goes toward, an open walking rail pins it there, 0x2A)
    LO* prail = i.ploTarget;
    const CRV* pcrv = prail ? PcrvFromLo(prail) : nullptr;
    if (!pcrv) return;
    glm::vec3 pos(0.0f), dir(0.0f);
    NearestCrv(pcrv, PosActorProbe0(pa), i.g1C, &pos, &dir, &i.g1C, &i.s20);  // 0x1E6ED8
    int grf = 0x22;
    if (!prail->U32Field(0, 0x1BC, 0) && !pcrv->fClosed)
    {
        const glm::vec3 v = VelAloPos(pa->palo);  // vt slot 33
        const float vAlong = (v.x * dir.x + v.y * dir.y) + v.z * dir.z;
        const glm::vec3 w = glm::mat3(MatJtLo(pa->palo)) * glm::vec3(pa->vecVel);  // (0x1A6318's rows x +0x190)
        const float vWant = -((w.x * dir.x + w.y * dir.y) + w.z * dir.z);
        const float sFirst = pcrv->as.empty() ? 0.0f : pcrv->as.front();  // 0x1E6620
        if (vAlong < -2.0f || vWant < 2.0f)
        {
            if (i.s20 < sFirst + 50.0f) { grf = 0x2A; EvalCrvS(pcrv, sFirst + 50.0f, &pos, nullptr); }  // 0x1E6F10
        }
        else if (SLengthCrv(pcrv) - 50.0f < i.s20) { grf = 0x2A; EvalCrvS(pcrv, SLengthCrv(pcrv) - 50.0f, &pos, nullptr); }
    }
    AddActorLineXps(pa, grf, pos - pa->vecDrawOffset, -dir, PosActorProbe0(pa));
}

// orig: 0x00194170
void AccelAloPos(ALO* p, const glm::vec3& v);  // solve.cpp: orig 0x1FAC30
void UpdateInteractMotion(ACTOR* pa)
{
    // a sliding rail (+0x1BC): along its direction at the parameter +0x1C, the gravity's part along it scaled by the
    // rail's +0x1C4, the result kept between the accelerations that smooth the speed along toward +-(+0x1C0) (SMP
    // D_002BF700 {2500, 0, 1}), less that gravity part (the gravity itself is applied elsewhere), as an acceleration
    if (pa->n4C8 != 3) return;
    LO* prail = pa->inter.ploTarget;
    if (!prail || !prail->U32Field(0, 0x1BC, 0)) return;
    const CRV* pcrv = PcrvFromLo(prail);
    if (!pcrv) return;
    ALO* palo = pa->palo;
    const glm::vec3 v = VelAloPos(palo);  // vt slot 33
    glm::vec3 w(0.0f);
    EvalCrv(pcrv, pa->inter.g1C, nullptr, &w);  // 0x1E67B8
    const float d1 = (v.x * w.x + v.y * w.y) + v.z * w.z;
    const SOST* ps = Psost(palo);
    const glm::vec3 grav = ps ? ps->dvGravity : glm::vec3(0.0f);  // (+0xE8 +0x80)
    const float d2 = (w.x * grav.x + w.y * grav.y) + w.z * grav.z;
    // (+0x1C0 / +0x1C4: the rail's InitLo 0x1CB908 sets 1000 / 1 unless the options do)
    auto field = [&](uint32_t ib, float gDefault) {
        uint32_t uDefault;
        std::memcpy(&uDefault, &gDefault, 4);
        const uint32_t u = prail->U32Field(0, ib, uDefault);
        float g;
        std::memcpy(&g, &u, 4);
        return g;
    };
    const float sMax = field(0x1C0, 1000.0f), rGrav = field(0x1C4, 1.0f);
    static const SMP s_smp{2500.0f, 0.0f, 1.0f};  // D_002BF700
    float a = 0.0f, b = 0.0f;
    GSmooth(d1, sMax, g_clock.dt, &s_smp, &a);   // 0x11D018
    GSmooth(d1, -sMax, g_clock.dt, &s_smp, &b);
    float x = rGrav * d2;
    x = x < b ? b : (a < x ? a : x);
    if (std::getenv("SLY2N_SLIDEPRINT"))
        std::printf("slide %d: g %.3f w %.2f %.2f %.2f v.w %.1f grav.w %.1f max %.1f r %.2f -> %.1f (a %.1f b %.1f) state %d\n", prail->oid,
                    pa->inter.g1C, w.x, w.y, w.z, d1, d2, sMax, rGrav, x - d2, a, b, pa->nSub4CC);
    AccelAloPos(palo, w * (x - d2));  // 0x1FAC30
}


// orig: 0x001423A0 (see interact.h)
bool FActorPoseDone(ACTOR* pa)
{
    if (pa->n4C8 != 5 || !pa->inter.ploTarget) return true;
    if (LO* ploEnd = pa->inter.reach.ploEnd)   // +0x934
    {
        // the end's vt slot 32: its record and its matrix in it (ALO 0x1F3FB0: the object record, the identity; LOX
        // 0x231550: +0x50 (else the identity record) and +0x60), the record's chain predicted at dt 0 (0x2309E8)
        XF* pxf = nullptr;
        glm::mat4 matLocal(1.0f);
        if (FIsLoDerivedFrom(ploEnd, 0x30)) pxf = PxfObj(static_cast<ALO*>(ploEnd));
        else if (FIsLoDerivedFrom(ploEnd, 3))
        {
            pxf = static_cast<LOX*>(ploEnd)->pxf;
            matLocal = static_cast<LOX*>(ploEnd)->mat;
        }
        glm::mat4 m(1.0f);
        if (pxf) PredictXfChain(pxf, nullptr, 0.0f, &m, nullptr);
        const glm::vec3 d = PosLoWorld(ploEnd) - glm::vec3(m * matLocal[3]);  // 0x1A62E8
        const float dist = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
        if (FPrint())
        {
            const glm::vec3 pw = PosLoWorld(ploEnd), pp = glm::vec3(m * matLocal[3]);
            std::printf("interact t %.3f: the pose's end %d at (%.2f %.2f %.2f) is %.3f from its prediction (%.2f %.2f %.2f)\n",
                        g_clock.t, ploEnd->oid, pw.x, pw.y, pw.z, dist, pp.x, pp.y, pp.z);
        }
        if (5.0f < dist) return false;
    }
    if (pa->inter.reach.state != 1) return true;
    return !(g_clock.t < pa->inter.reach.t94);
}

// orig 0x001840B8 (FAddRsmg 0x20FEC0 with the trigger flag)
void AddUseRsmg(LO* puse, OID oidRoot, OID oidSm, OID oidState)
{
    const int c = static_cast<int>(puse->U32Field(0, 0x118));
    for (int k = 0; k < c; k++)
    {
        const uint32_t ib = 0x11C + 0x10 * k;
        const OID r = static_cast<OID>(puse->U32Field(0, ib)), sm = static_cast<OID>(puse->U32Field(0, ib + 4));
        if (r == oidRoot && (sm == -1 || sm == oidSm))
        {
            puse->SetField(0, ib + 8, static_cast<uint32_t>(oidState));
            return;
        }
    }
    if (!(c + 1 < 4)) return;
    const uint32_t ib = 0x11C + 0x10 * c;
    puse->SetField(0, 0x118, static_cast<uint32_t>(c + 1));
    puse->SetField(0, ib, static_cast<uint32_t>(oidRoot));
    puse->SetField(0, ib + 4, static_cast<uint32_t>(oidSm));
    puse->SetField(0, ib + 8, static_cast<uint32_t>(oidState));
    puse->SetField(0, ib + 12, uint32_t(-1));
}

// orig 0x001840F8 (0x161F10)
void AddUseTask(LO* puse, OID oidTask, int state)
{
    const int c = static_cast<int>(puse->U32Field(0, 0x15C));
    if (!(c < 4)) return;
    puse->SetField(0, 0x15C, static_cast<uint32_t>(c + 1));
    puse->SetField(0, 0x160 + 8 * c, static_cast<uint32_t>(oidTask));
    puse->SetField(0, 0x164 + 8 * c, static_cast<uint32_t>(state));
}


// orig: 0x00194558 (the interaction's slot 99: what the actor may do meanwhile, by the interaction's kind +0x4C8)
bool FInteractCanDo(const ACTOR* pa, int k)
{
    if (k == 3)
    {
        switch (pa->n4C8)
        {
        case 0: case 1: case 3: case 4: case 13:
            return pa->f12F0 && (g_grfjoyt & 2) && !FActiveCplcy(&g_cmpolicies.cpdefault);
        default: return false;
        }
    }
    switch (pa->n4C8)
    {
    case 1: case 8: case 11: case 12: return false;
    case 3: case 4: return k == 1 || k == 2 || k == 0x1F;
    default: return true;
    }
}
