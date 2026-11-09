// The guards' detectors. See detect.h. Reference: Sly2-Reference/Sly2-Release/game/m_14F030_p3.cpp (0x153DE0, 0x153EF8),
// m_153FF8_p3.cpp (0x154080, 0x154180, 0x1541D8), m_227BB8.cpp (0x227C90) and the original's code for 0x153E60.

#include "coop.h"
#include "detect.h"
#include "worldglobals.h"
#include "worldaccess.h"
#include "guard.h"

#include "activate.h"
#include "task.h"
#include "actor.h"
#include "ai.h"
#include "aipursuit.h"
#include "bsp.h"
#include "so.h"
#include "stepphys.h"
#include "bsp.h"
#include "clock.h"
#include "crv.h"
#include "po.h"
#include "objmethod.h"
#include "spliceobj.h"
#include "update.h"
#include "sw.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

namespace
{
std::unordered_map<const LO*, std::unique_ptr<DETECTOR>>& Map()
{
    static std::unordered_map<const LO*, std::unique_ptr<DETECTOR>> s_map;
    WORLD_TABLE_LOCAL(s_map);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_map;
}

// orig 0x227C90: the point inside the shape (its local frame: the BSP of its geometry; the bounding sphere +0x110 /
// +0x120 natively skipped: the tree decides)
// the shape's tree (an instance's copy shares the loaded object's geometry and tree: natively the clone has none of
// its own)
const LOXSHAPE* PshapeTree(LO* plo)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x16)) return nullptr;
    const LOXSHAPE* pshape = static_cast<const LOXSHAPE*>(plo);
    for (const LO* p = plo; pshape->bsp.abspn.empty() && p->ploCloneOf; p = p->ploCloneOf)
        if (FCidDerivedFrom(p->ploCloneOf->Cid(), 0x16)) pshape = static_cast<const LOXSHAPE*>(p->ploCloneOf);
    return pshape->bsp.abspn.empty() ? nullptr : pshape;
}

bool FInsideShape(LO* plo, const glm::vec3& pos)
{
    const LOXSHAPE* pshape = PshapeTree(plo);
    if (!pshape) return false;
    const glm::vec3 q = glm::vec3(glm::inverse(MatWorldLox(plo)) * glm::vec4(pos, 1.0f));
    const BSPREF ref(pshape->bsp, pshape->geom);
    return IbspFindPoint(ref, 0, ref.cnode, q) != IBSP_NONE;
}
}  // namespace

DETECTOR* PdetectorNew(LO* plo, LO* ploOwner)
{
    auto& p = Map()[plo];
    p = std::make_unique<DETECTOR>();
    p->plo = plo;
    p->ploOwner = ploOwner;
    p->ae.reserve(8);
    return p.get();
}
DETECTOR* PdetectorFromLo(LO* plo)
{
    auto it = Map().find(plo);
    return it == Map().end() ? nullptr : it->second.get();
}
void ResetDetectors() { Map().clear(); }

// orig: 0x00154180
DETENTRY* PdetentryFind(DETECTOR* pdet, LO* plo)
{
    if (!plo) return nullptr;
    for (DETENTRY& e : pdet->ae)
        if (e.plo == plo) return &e;
    return nullptr;
}

// orig: 0x00153EF8 / 0x00153F18
void AddDetectorTarget(DETECTOR* pdet, LO* plo)
{
    if (PdetentryFind(pdet, plo) || pdet->ae.size() >= 8) return;  // (a null object is added too: 0x154180 finds none)
    DETENTRY e;
    e.plo = plo;
    pdet->ae.push_back(e);
}

// orig: 0x00153E60: the detector's eye (a guard owner's 80 above it)
glm::vec3 PosDetectorEye(const DETECTOR* pdet)
{
    if (!pdet->ploOwner) return PosAi(pdet->plo);
    const glm::vec3 pos = PosAi(pdet->ploOwner);
    if (!FCidDerivedFrom(pdet->ploOwner->Cid(), 0x38)) return pos;
    return pos + glm::vec3(0.0f, 0.0f, 80.0f);  // (D_2A2DC0 x 80)
}

namespace
{
// the entries that may block the sight (0x154568: not bit 48, not the stepping objects' (0x33) trees; flag 4:
// 0x154528, the class-0x31 objects only)
bool FBlocks(const DETECTOR* pdet, const SOE* psoe)
{
    if (psoe->f48) return false;
    ALO* palo = psoe->palo;
    if (!palo) return false;
    // the object's attachment (+0xEC) marked 4 never blocks (0x154568 / 0x20FA10)
    if (const OXA* poxa = Poxa(palo); poxa && (poxa->grf & 4)) return false;
    // flag 4 (0x154528): only the class-0x31 objects
    if (pdet->grf & 4) return palo->Cid() == 0x31;
    // nor an object under the same root (+0x14, the root's activation node) as the detector: e.g. a searchlight's
    // own lamp does not hide what its detector looks at
    if (pdet->plo && PloRoot(palo) == PloRoot(pdet->plo)) return false;
    for (const LO* p = palo; p; p = p->paloParent)
        if (FCidDerivedFrom(p->Cid(), 0x33)) return false;
    return true;
}

// orig: 0x001545F0: something solid between the eye and the target's point (0x20FAA0 + 0x14CC58)
bool FSightBlocked(const DETECTOR* pdet, LO* pso)
{
    const glm::vec3 eye = PosDetectorEye(pdet);
    const glm::vec3 pt = PosAi(pso);  // (D_2BDBF0 (0, 0, 0) in the target's frame, 0x1A6F00)
    const glm::vec3 mn = glm::min(eye, pt), mx = glm::max(eye, pt);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
            if (FBlocks(pdet, psoe) && apsoe.size() < 0x100) apsoe.push_back(psoe);
    }
    if (apsoe.empty()) return false;
    LSG lsg;
    return PsoeHitTestLineObjects(0, eye, pt, apsoe, &lsg) != nullptr;
}

}  // namespace

// orig: 0x00208BE0's perception 1 ("seen" noises): the point inside the view volume (0x227C90; the volume in the
// world, its +0x28 bit 60), and nothing between the guard's place and it (0x20FAA0 with the filter 0x20FA10: not a
// sensor (bit 48), not an attachment marked 4, not the guard's own attachment +0xEC; then 0x14CC58). Natively the
// seen kind was never accepted (audit 2026-11 A9)
bool FGuardSeesPoint(LO* ploGuard, LO* ploVolume, const glm::vec3& pos, const glm::vec3& posT)
{
    if (!ploVolume || !ploVolume->fInWorld || !FInsideShape(ploVolume, posT)) return false;
    const OXA* poxaSelf = ploGuard && FCidDerivedFrom(ploGuard->Cid(), 0x30) ? Poxa(static_cast<ALO*>(ploGuard)) : nullptr;
    const glm::vec3 mn = glm::min(pos, posT), mx = glm::max(pos, posT);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (psoe->f48 || !psoe->palo) continue;
            const OXA* po = Poxa(psoe->palo);
            if (!po || (po->grf & 4) || po == poxaSelf) continue;
            if (apsoe.size() < 0x100) apsoe.push_back(psoe);
        }
    }
    if (apsoe.empty()) return true;
    LSG lsg;
    return PsoeHitTestLineObjects(0, pos, posT, apsoe, &lsg) == nullptr;
}

namespace
{
// orig: 0x00154770: a player hiding by its sub-object's kind (+0x4C8; the reference's +0x4CC / +0x4D4)
bool FTargetHidden(const DETECTOR* pdet, LO* pso)
{
    ACTOR* pa = FCidDerivedFrom(pso->Cid(), 0x34) ? PactorFromLo(pso) : nullptr;
    if (!pa || pa->nSub4CC == -1) return false;
    switch (pa->n4C8)
    {
    case 1: return pa->nSub4CC == 0;
    case 2: return true;
    case 8: return static_cast<unsigned>(pa->nSub4CC - 9) < 2;
    case 0: case 13:
    {
        // the interaction's object (+0x4D4): a path (class 0xC) with +0x194 or a class-0x22 surface with +0xE4 set
        // never hides; another class hides unless its +0xE4; else hidden when the target faces away from the
        // detector (its x axis . (target - detector) <= 0), flipped for kind 0 by the side +0x1040. Natively these
        // kinds never hid (audit 2026-11 A8)
        LO* pt = pa->inter.ploTarget;
        if (!pt) return false;
        if (FCidDerivedFrom(pt->Cid(), 0xC))
        {
            if (pt->U32Field(0, 0x194, 0)) return false;
        }
        else if (FCidDerivedFrom(pt->Cid(), 0x22))
        {
            if (pt->U32Field(0, 0xE4, 0)) return false;
        }
        else return !pt->U32Field(0, 0xE4, 0);
        const glm::vec3 d = PosAi(pso) - PosAi(pdet->plo ? pdet->plo : pdet->ploOwner);
        const glm::vec3 ax = glm::vec3(MatWorldOfAlo(pa->palo)[0]);  // (0x1A6318: the matrix's first row)
        const bool f = glm::dot(d, ax) <= 0.0f;
        const bool g = pa->n4C8 == 0 ? !pa->n1040 : false;
        return f != g;
    }
    default: return false;
    }
}
}  // namespace

namespace
{
// orig: 0x001541D8's loop over the list 0x3E1480: the class-0x17 shapes in the world (added by their slot 22
// 0x227E58, removed by slot 23 0x227E20; e.g. the Smoke Bomb's cloud) hide what is behind them: the eye-target line
// is clipped to the shape's tree (0x118668 with no node limit, at most 4 pieces) in its local frame (0x1A6F00)
bool FSightShapeBlocked(const DETECTOR* pdet, const glm::vec3& pos)
{
    if (!g_psw) return false;
    const glm::vec3 eye = PosDetectorEye(pdet);
    for (LO* plo : g_psw->mpcidaplo[0x17])
    {
        if (!plo || plo->Cid() != 0x17 || !plo->fInWorld) continue;
        const LOXSHAPE* pshape = PshapeTree(plo);
        if (!pshape) continue;
        const glm::mat4 inv = glm::inverse(MatWorldLox(plo));
        const glm::vec3 a = glm::vec3(inv * glm::vec4(eye, 1.0f)), b = glm::vec3(inv * glm::vec4(pos, 1.0f));
        const BSPREF ref(pshape->bsp, pshape->geom);
        LSG alsg[4];
        if (ClipSegmentBsp(ref, 0, ref.cnode, a, b, 4, alsg) > 0)
        {
            static const bool s_fDebug = std::getenv("SLY2N_SHAPEDEBUG") != nullptr;  // (debug: who blocks the sight)
            if (s_fDebug)
                std::printf("sight: t %.3f shape %d (parent %d) hides %s from %d\n", g_clock.t, plo->oid,
                            plo->paloParent ? plo->paloParent->oid : -1, "the target", pdet->ploOwner ? pdet->ploOwner->oid : -1);
            return true;
        }
    }
    return false;
}
}  // namespace

// orig: 0x001541D8
int NDetectState(DETECTOR* pdet, LO* pso, int k)
{
    LO* pme = g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (!pso) return 0;
    if (!pso->fInWorld) return 0;  // orig 0x1A61A0 (in the world's update list)
    if (FCidDerivedFrom(pso->Cid(), 0x32) && NPlayerStatus77(pso) == 2) return 0;  // (vt slot 77: knocked out)
    if (FCidDerivedFrom(pso->Cid(), 0x34) && pme && FCidDerivedFrom(pme->Cid(), 0x46)) return 0;
    if (FGamePausedForAi()) return 0;  // (0x20F1B8)
    if (pdet->fNever) return 0;
    // the actors' +0x11AC: unseen while non-zero. No code writes it; the scripts do through option 741 (EOPID 758):
    // Ep3's water buckets (95679, Spice Room Recon) set it while a player rides one (their events 5 / 6), so the
    // flashlight guard there does not see him (R16: natively the option had no setter and he was seen)
    if (FCidDerivedFrom(pso->Cid(), 0x34))
        if (const ACTOR* pa = PactorFromLo(pso); pa && pa->n11AC) return 0;
    // (debug: SLY2N_UNSEEN=1 sets it for every player, another value only for the player with that oid, e.g. 7 Sly,
    // as the reference's SLY2_POKE of 2E1E40@+11AC=1 does, for tests near guards without a job failure)
    static const int s_nUnseen = std::getenv("SLY2N_UNSEEN") ? std::atoi(std::getenv("SLY2N_UNSEEN")) : 0;
    if (s_nUnseen && FCidDerivedFrom(pso->Cid(), 0x34) && (s_nUnseen == 1 || pso->oid == s_nUnseen)) return 0;
    if (pdet->fAlways) return 1;
    glm::vec3 pos = PosAi(pso);
    if (FCidDerivedFrom(pso->Cid(), 0x33) && FCidDerivedFrom(pso->Cid(), 0x30))
        pos += glm::vec3(StepPhysOf(static_cast<ALO*>(pso)).vecBase);  // (+0x240)
    if (FSightShapeBlocked(pdet, pos)) return 0;
    static const bool s_fDebug = std::getenv("SLY2N_DETDEBUG") != nullptr;
    static const int s_oidDebug = std::getenv("SLY2N_DETOID") ? std::atoi(std::getenv("SLY2N_DETOID")) : 7;  // (debug: whose detection)
    if (s_fDebug && pso->oid == s_oidDebug)
        std::fprintf(stderr, "det %d k %d target (%.1f %.1f %.1f) eye (%.1f %.1f %.1f) in %d in2 %d hidden %d blocked %d grf %x vol %d\n",
                     pdet->ploOwner ? pdet->ploOwner->oid : -1, k, pos.x, pos.y, pos.z, PosDetectorEye(pdet).x,
                     PosDetectorEye(pdet).y, PosDetectorEye(pdet).z, FInsideShape(pdet->ploVolume, pos),
                     pdet->ploVolume2 ? FInsideShape(pdet->ploVolume2, pos) : -1, FTargetHidden(pdet, pso), FSightBlocked(pdet, pso),
                     pdet->grf, pdet->ploVolume ? pdet->ploVolume->oid : -1);
    if (s_fDebug && pso->oid == 7 && pdet->ploVolume)
    {
        const glm::mat4 m = MatWorldLox(pdet->ploVolume);
        const LOX* px = static_cast<const LOX*>(pdet->ploVolume);
        const LOXSHAPE* ps = static_cast<const LOXSHAPE*>(pdet->ploVolume);
        const glm::vec3 q = glm::vec3(glm::inverse(m) * glm::vec4(pos, 1.0f));
        glm::vec3 mn(1e9f), mx(-1e9f);
        for (const glm::vec3& v : ps->geom.apos) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
        std::fprintf(stderr, "   local (%.1f %.1f %.1f) geom %zu pts box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) bsp %zu surfs %zu\n", q.x, q.y, q.z,
                     ps->geom.apos.size(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, ps->bsp.abspn.size(), ps->geom.asurf.size());
        std::fprintf(stderr, "   volume t %.4f world (%.1f %.1f %.1f) x (%.4f %.4f %.4f) parent %d pxf %p\n", g_clock.t, m[3].x, m[3].y, m[3].z, m[0].x, m[0].y,
                     m[0].z, pdet->ploVolume->paloParent ? pdet->ploVolume->paloParent->oid : -1, (void*)px->pxf);
    }
    switch (k)
    {
    case 0:
        // the owner's attachment (+0xE4 = the guard's +0xEC): an object touching it (an overlap with contacts) is seen
        if (pdet->fTouchRam)
        {
            for (const auto& [plo, f] : pdet->atouchRam)
                if (plo == pso)
                {
                    if (f) return 1;
                    break;
                }
        }
        else if (pdet->ploOwner && FCidDerivedFrom(pdet->ploOwner->Cid(), 0x30))
            if (OXA* poxa = Poxa(static_cast<ALO*>(pdet->ploOwner)))
                for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
                    if (ox->psoOther == pso)
                    {
                        if (s_fDebug && ox->pxp)  // (debug: the touch's contacts, the entries' owners)
                            for (XP* x = ox->pxp; x; x = x->pxpNext)
                                std::fprintf(stderr, "det touch %d t %.3f: %d / %d xpk %d dist %.1f\n", pdet->ploOwner->oid, g_clock.t,
                                             x->axpd[0].palo ? x->axpd[0].palo->oid : -1, x->axpd[1].palo ? x->axpd[1].palo->oid : -1, x->xpk, x->gDist);
                        if (ox->pxp) return 1;
                        break;
                    }
        if (!FInsideShape(pdet->ploVolume, pos)) return 0;
        if (pdet->ploVolume2 && !FInsideShape(pdet->ploVolume2, pos)) return 0;
        if (!(pdet->grf & 1) && FTargetHidden(pdet, pso)) return 0;
        if (pdet->grf & 2) return 1;
        return FSightBlocked(pdet, pso) ? 0 : 1;
    case 1:
        if (pdet->ploVolume2 && !FInsideShape(pdet->ploVolume2, pos)) return 0;
        if (!FInsideShape(pdet->ploVolume, pos)) return 2;
        break;
    case 2:
        if (pdet->ploVolume2 && !FInsideShape(pdet->ploVolume2, pos)) return 0;
        if (FInsideShape(pdet->ploVolume, pos))
        {
            if (pdet->grf & 2) return 1;
            if (FSightBlocked(pdet, pso)) return k;
            return 1;
        }
        break;
    default: return k;
    }
    if ((pdet->grf & 8) && FSightBlocked(pdet, pso)) k = 0;
    return k;
}

// orig: 0x00154080
void SetDetentryState(DETECTOR* pdet, DETENTRY* pe, int n)
{
    if (!pe || n == pe->st) return;
    const size_t index = size_t(pe - pdet->ae.data());
    if (pe->st == 0) pe->tC = g_clock.t;
    if (std::getenv("SLY2N_DETSTATE"))  // (debug: every detector entry's state change)
        std::printf("detstate t %.3f det %d owner %d target %d %d -> %d\n", g_clock.t, pdet->plo ? pdet->plo->oid : -1, pdet->ploOwner ? pdet->ploOwner->oid : -1, pe->plo ? pe->plo->oid : -1, pe->st, n);
    // orig: 0x001331F8 / 0x00200DA0: event 28 converts the entry's
    // first field to its target object, followed by the old and requested states.
    CRef args[3];
    args[0].SetBasic(pe->plo);
    args[1].SetS32(pe->st);
    args[2].SetS32(n);
    HandleLoSpliceEvent(pdet->plo, 28, false, 3, args);
    // Event handlers may remove entries, which shifts the original inline slots.
    // A removed final slot is no longer observable in the active table.
    if (index < pdet->ae.size())
    {
        pe = &pdet->ae[index];
        pe->st = n;
        pe->t8 = g_clock.t;
    }
    // at 1 the tasks hear of the detector's guard (its parent, 0x1A58D0 at the reset) seeing it (0x13)
    if (n == 1 && pdet->ploOwner && FCidDerivedFrom(pdet->ploOwner->Cid(), 0x33) && g_psw)
        BroadcastTaskEvent(g_psw, 0x13, pdet->ploOwner);
}

// orig: 0x00153DE0
void UpdateDetector(DETECTOR* pdet)
{
    for (size_t i = 0; i < pdet->ae.size(); ++i)
    {
        int n = NDetectState(pdet, pdet->ae[i].plo, pdet->ae[i].st);
        // (co-op, coop.h: the host's mission characters seeing the other player see this game's: the job fails)
        if (n == 0 && FCoopMissionSeesRemote(pdet, pdet->ae[i].plo)) n = 1;
        SetDetentryState(pdet, &pdet->ae[i], n);
    }
}

// (public: the actors' target ranks 0x208E70)
bool FPointInsideShape(LO* plo, const glm::vec3& pos) { return FInsideShape(plo, pos); }

bool FPointInShape(LO* plo, const glm::vec3& pos) { return FInsideShape(plo, pos); }

namespace
{
LO* PloDetectorOption(LO* detector, const CRef& value)
{
    if (value.m_tagk == TAGK_Basic) return dynamic_cast<LO*>(value.m_pbasic);
    if (value.m_tagk != TAGK_S32 || !g_splicegame.pfnFindObject) return nullptr;
    return dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, value.m_n, detector, detector));
}

void BindDetectorFields(DETECTOR* detector)
{
    // The loader retains object references as OIDs until their namespace exists.
    // Resolve through the same relative lookup as 0x0013E590, including clones.
    for (const OPT& opt : detector->plo->aopt)
    {
        if (opt.aval.empty()) continue;
        const CRef value = opt.aval[0].pbasic ? [&] {
            CRef ref; ref.SetBasic(opt.aval[0].pbasic); return ref;
        }() : RefFromOptionValue(opt.aval[0]);
        switch (opt.eopid)
        {
        case 342: detector->grf = uint32_t(opt.aval[0].n); break;
        case 343: detector->fAlways = opt.aval[0].n; break;
        case 344: detector->fNever = opt.aval[0].n; break;
        case 345: detector->ploVolume = PloDetectorOption(detector->plo, value); break;
        case 346: detector->ploVolume2 = PloDetectorOption(detector->plo, value); break;
        case 348: detector->ploOwner = PloDetectorOption(detector->plo, value); break;
        default: break;
        }
    }
}

DETECTOR* PdetectorEnsure(LO* object)
{
    if (DETECTOR* detector = PdetectorFromLo(object)) return detector;
    return PdetectorNew(object, nullptr);
}

// orig: 0x00153F80: return a watched entry to zero, then shift the remaining entries.
void RemoveDetectorTarget(DETECTOR* detector, LO* target)
{
    for (size_t i = 0; i < detector->ae.size(); ++i)
        if (target && detector->ae[i].plo == target) {
            SetDetentryState(detector, &detector->ae[i], 0);
            if (i < detector->ae.size()) detector->ae.erase(detector->ae.begin() + i);
            break;
        }
}

// orig: 0x00153DB0: standalone detectors also run their base splice update.
void UpdateDetectorObject(LO* object)
{
    PostLoadDetector(object);
    if (PFNUPDATE update = PfnUpdateOrig(0x1A5E28)) update(object);
    UpdateDetector(PdetectorEnsure(object));
}

struct INITDETECTOR
{
    INITDETECTOR()
    {
        RegisterLoUpdate(0x153DB0, UpdateDetectorObject);
        // orig: 0x00104228 -> 0x00154038: a watched target's requested state.
        // OTYP 0x106B copies the enum word through 0x00201110, without coercion.
        RegisterObjectMethod(0x104228, [](LO* object, const std::vector<CRef>& args) {
            DETECTOR* detector = PdetectorFromLo(object);
            if (detector && args.size() >= 2 && args[0].m_tagk == TAGK_Basic)
                if (DETENTRY* entry = PdetentryFind(detector, dynamic_cast<LO*>(args[0].m_pbasic)))
                    SetDetentryState(detector, entry, args[1].m_n);
            return CRef(TAGK_Void);
        });
        // orig: 0x00104060 -> 0x00154038: the guard's detector at +0xF64.
        RegisterObjectMethod(0x104060, [](LO* object, const std::vector<CRef>& args) {
            GUARD* guard = PguardFromLo(object);
            DETECTOR* detector = guard ? guard->pdet : nullptr;
            if (detector && args.size() >= 2 && args[0].m_tagk == TAGK_Basic)
                if (DETENTRY* entry = PdetentryFind(detector, dynamic_cast<LO*>(args[0].m_pbasic)))
                    SetDetentryState(detector, entry, args[1].m_n);
            return CRef(TAGK_Void);
        });

        // orig: 0x00200598 with grf 0x2000: the guard's EOPIDs 312 / 313 / 314 (grf 0x2092) are fields of the
        // sub-object their PMF 0x206378 returns, the guard's detector +0xF64: +0xF8 grf, +0xF0 always sees, +0xF4
        // never sees. E.g. world 3's swarmer battle (block 123829 module 26): when one rat of s3932 sees Sly, the
        // splice sets option 437 (EOPID 313) to #t on all four, so the rooftop pair sees him too and comes down.
        static const auto s_detectorField = [](LO* object) -> DETECTOR* {
            GUARD* guard = PguardFromLo(object);
            return guard ? guard->pdet : nullptr;
        };
        RegisterObjectSetter(312, [](LO* object, const CRef& value) {
            if (DETECTOR* d = s_detectorField(object)) d->grf = uint32_t(value.RefCoerceS32().m_n);
        });
        RegisterObjectSetter(313, [](LO* object, const CRef& value) {
            if (DETECTOR* d = s_detectorField(object)) d->fAlways = value.m_n;  // (the payload word, no coercion)
        });
        RegisterObjectSetter(314, [](LO* object, const CRef& value) {
            if (DETECTOR* d = s_detectorField(object)) d->fNever = value.m_n;
        });
        RegisterObjectGetter(312, [](LO* object) {
            CRef r;
            if (DETECTOR* d = s_detectorField(object)) r.SetS32(int(d->grf));
            return r;
        });
        RegisterObjectGetter(313, [](LO* object) {
            CRef r;
            if (DETECTOR* d = s_detectorField(object)) r.SetBool(d->fAlways != 0);
            return r;
        });
        RegisterObjectGetter(314, [](LO* object) {
            CRef r;
            if (DETECTOR* d = s_detectorField(object)) r.SetBool(d->fNever != 0);
            return r;
        });

        // orig: 0x00153F18 / 0x00153F80: add/remove an authored target.
        RegisterObjectSetter(349, [](LO* object, const CRef& value) {
            DETECTOR* detector = PdetectorEnsure(object);
            if (detector->fOptionsBound) AddDetectorTarget(detector, PloDetectorOption(object, value));
        });
        RegisterObjectSetter(350, [](LO* object, const CRef& value) {
            DETECTOR* detector = PdetectorEnsure(object);
            if (detector->fOptionsBound) RemoveDetectorTarget(detector, PloDetectorOption(object, value));
        });
        // EOPID 758 (an actor's option 741, the field +0x11AC both ways): unseen by the detectors (NDetectState)
        RegisterObjectSetter(758, [](LO* object, const CRef& value) {
            if (ACTOR* pa = FCidDerivedFrom(object->Cid(), 0x34) ? PactorFromLo(object) : nullptr)
                pa->n11AC = value.m_tagk == TAGK_Bool ? int(value.m_bool) : value.RefCoerceS32().m_n;
        });
        RegisterObjectGetter(758, [](LO* object) {
            CRef r;
            const ACTOR* pa = FCidDerivedFrom(object->Cid(), 0x34) ? PactorFromLo(object) : nullptr;
            r.SetS32(pa ? pa->n11AC : 0);
            return r;
        });
    }
} s_initdetector;
}

// Native completion of loaded detector references after all namespace objects exist.
// The original loader's deferred 0x0013E590 fixups precede StartSw's script events.
void PostLoadDetector(LO* object)
{
    if (!object || !FCidDerivedFrom(object->Cid(), 0x10)) return;
    DETECTOR* detector = PdetectorEnsure(object);
    if (detector->fOptionsBound) return;
    detector->fOptionsBound = true;
    BindDetectorFields(detector);
    for (const OPT& opt : object->aopt)
        if ((opt.eopid == 349 || opt.eopid == 350) && !opt.aval.empty()) {
            CRef value = RefFromOptionValue(opt.aval[0]);
            if (opt.aval[0].pbasic) value.SetBasic(opt.aval[0].pbasic);
            LO* target = PloDetectorOption(object, value);
            if (opt.eopid == 349) AddDetectorTarget(detector, target);
            else RemoveDetectorTarget(detector, target);
        }
}

// (co-op, coop.h) would this detector see the other game's player at these points (its target point posDet, as
// NDetectState's, and its body's point posAi for the line of sight)? NDetectState's kind 0 (first sight) without the
// object tests (touch, hiding by an interaction: the other game tells its own)
bool FCoopDetectorSeesPoint(DETECTOR* pdet, const glm::vec3& posAi, const glm::vec3& posDet)
{
    if (!pdet || pdet->fNever || FGamePausedForAi()) return false;
    if (pdet->fAlways) return true;
    if (FSightShapeBlocked(pdet, posDet)) return false;
    if (!pdet->ploVolume || !FInsideShape(pdet->ploVolume, posDet)) return false;
    if (pdet->ploVolume2 && !FInsideShape(pdet->ploVolume2, posDet)) return false;
    if (pdet->grf & 2) return true;
    const glm::vec3 eye = PosDetectorEye(pdet);
    const glm::vec3 mn = glm::min(eye, posAi), mx = glm::max(eye, posAi);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
            if (FBlocks(pdet, psoe) && apsoe.size() < 0x100) apsoe.push_back(psoe);
    }
    if (apsoe.empty()) return true;
    LSG lsg;
    return PsoeHitTestLineObjects(0, eye, posAi, apsoe, &lsg) == nullptr;
}
