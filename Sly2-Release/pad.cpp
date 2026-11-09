// The touch pads (cid 0x53, module 0x148B18..0x1492F0: an ALO with a trigger state object at +0x150): its state
// (+0x1E0, the time +0x1E4, the re-arm time +0x1E8 (EOPID 419), no sound +0x1EC (421)), the kind +0x2A8 (413: 0
// touched by an object of its lists, 1 / 2 hit (slot 75, 0x148F20: TODO), 3 the player's state), its lists (+0x2AC the
// objects, +0x2D0 the classes: EOPID 418 = 0x149248) and the toucher +0x2F8 (EOPID 427). Entering state 1 runs its
// splice event 5: the intro's bounce pads (8448, 150508) launch the toucher with the actors' method 730 (0x10A790 ->
// 0x141968) toward their s1088 after 0.05 s. And the actors' launch itself (jump kind 8).
#include "coop.h"
#include "sw.h"
#include "actor.h"
#include "worldglobals.h"
#include "alo.h"
#include "cls.h"
#include "clock.h"
#include "jump.h"
#include "lo.h"
#include "frame.h"
#include "objmethod.h"
#include "ref.h"
#include "so.h"
#include "sound.h"
#include "sly.h"
#include "spliceglue.h"
#include "trigger.h"
#include "update.h"
#include "vtnative.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
glm::vec3 VelBallistic(float g, const glm::vec3& pos, const glm::vec3& posTarget, const glm::vec3& acc);
}

// orig: 0x00141968 (the actor launched to a point posT: g the elevation's share, h the air control's weight): the
// velocity from its feet (+0x248) to the point by its gravity; without a damage reaction the jump kind 8, the air
// control +0x40 3 toward that velocity's horizontal part, +0x34 h (EOPID 730, SCPT 0x2B)
void LaunchActorTo(ACTOR* pa, const glm::vec3& posT, float g, float h)
{
    LO* plo = pa->palo;
    glm::vec3 pos = PosAloCenter(pa->palo);  // 0x1A62E8
    pos.z += pa->vecBase.z;
    const SOST* pst = pa->palo->psort ? pa->palo->psort->psost.get() : nullptr;
    const glm::vec3 v = VelBallistic(g, pos, posT, pst ? pst->dvGravity : glm::vec3(0.0f, 0.0f, -2275.0f));
    SetAloVelocityTo(pa->palo, v);  // vt slot 35
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("launch %d from %.0f %.0f %.0f to %.0f %.0f %.0f (g %.2f h %.2f): v %.1f %.1f %.1f t %.9g\n", plo->oid, pos.x, pos.y,
                    pos.z, posT.x, posT.y, posT.z, g, h, v.x, v.y, v.z, g_clock.t);
    if (pa->nSubDF4 == -1)
    {
        SetJumpKind(&pa->jump, 8, 0, 0);  // 0x195A00
        pa->jump.n40 = 3;
        pa->jump.vecA0 = glm::vec3(v.x, v.y, 0.0f);
        pa->jump.g34 = h;
        if (h == 0.0f)
        {
            pa->jump.f28 = 0;
            pa->jump.f2C = 0;
        }
    }
}

namespace
{
struct PAD : TRIGGERSTATE
{
    bool fInit = false;
    float dtRearm = 1.0f;    // +0x1E8 (0x148718: 1)
    int fNoSound = 0;        // +0x1EC
    int fStay = 0;           // +0x1F4 (+0xA4: stays at 1)
    int kind = 0;            // +0x2A8
    std::vector<OID> aoid;   // +0x2AC
    std::vector<int> acid;   // +0x2D0
    LO* ploAction = nullptr;   // +0x2F4 (EOPID 414)
};
std::unordered_map<LO*, PAD>& MpPad()
{
    static std::unordered_map<LO*, PAD> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}

PAD& Pad(LO* plo)
{
    PAD& p = MpPad()[plo];
    if (p.fInit) return p;
    p.fInit = true;
    p.owner = plo;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval.front();
        switch (o.eopid)
        {
        case 413: p.kind = v.n; break;
        case 419: p.dtRearm = v.k == 1 ? v.ag[0] : static_cast<float>(v.n); break;
        case 421: p.fNoSound = v.n; break;
        case 423: p.fStay = v.n; break;
        case 417: if (p.aoid.size() < 8) p.aoid.push_back(v.n); break;  // (0x149220)
        case 418: if (p.acid.size() < 8) p.acid.push_back(v.n); break;  // 0x149248
        default: break;
        }
    }
    if (std::getenv("SLY2N_PADDEBUG") && plo->pframe)
        if (CRef* pr = plo->pframe->PrefFindBinding(1088, 1))
        {
            const glm::vec3 v = pr->GetVector();
            const glm::vec3 c = FCidDerivedFrom(plo->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(plo)) : glm::vec3(0.0f);
            std::printf("pad %d at %.0f %.0f %.0f: kind %d, s1088 (tag %d) %.0f %.0f %.0f\n", plo->oid, c.x, c.y, c.z, p.kind, pr->m_tagk, v.x, v.y, v.z);
        }
    p.kind = static_cast<int>(plo->U32Field(0, 0x2A8, p.kind));
    p.fStay = static_cast<int>(plo->U32Field(0, 0x1F4, p.fStay));
    const OID action = static_cast<OID>(plo->U32Field(0, 0x2F4, uint32_t(-1)));
    if (action != -1) p.ploAction = PloFindRelative(plo, action);
    p.state = 0;  // 0x148C38: 0x1490A0(0) from -1 (nothing run)
    p.tState = g_clock.t;
    return p;
}

// orig: 0x00149100 (the object in the lists: its oid among the objects', and of one of the classes)
bool FPadAccepts(const PAD& p, LO* plo)
{
    if (!plo) return false;
    if (!p.aoid.empty())
    {
        bool f = false;
        for (OID oid : p.aoid) f = f || plo->oid == oid || plo->oidInstance == oid;
        if (!f) return false;
    }
    if (!p.acid.empty())
    {
        bool f = false;
        for (int cid : p.acid) f = f || FCidDerivedFrom(plo->Cid(), static_cast<CID>(cid));  // 0x132B10
        if (!f) return false;
    }
    return true;
}

// orig: 0x001490A0 -> 0x00148A78 (+ 0x1487A0 / 0x148860: entering 1 from 0 runs event 5, the sound 2 unless +0x1EC,
// then the enter ASEGs and 2 unless +0xA4; 0 runs event 6 and the leave ASEGs)
void EnterPad(LO* plo, PAD& p);
void LeavePad(LO* plo, PAD& p);
void SetPadState(LO* plo, PAD& p, int n)
{
    const int old = p.state;
    if (old == n) return;
    const bool f1 = old == 0 && n == 1, f0 = old != -1 && n == 0;
    if (f1 || f0) CoopOnPadState(plo->oid, plo->iclone, n);   // (coop.h: a bell / switch / plate here, in the other game too)
    p.state = n;
    p.tState = g_clock.t;
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad %d: state %d -> %d (toucher %d) t %.3f\n", plo->oid, old, n, p.ploFound ? p.ploFound->oid : -1, g_clock.t);
    if (f1) EnterPad(plo, p);
    if (f0) LeavePad(plo, p);
}

// orig: 0x001487A0 (the enter actions: event 5, the sound 2 unless +0x1EC, the enter list, state 2 unless +0xA4, the
// owner's message 6)
void EnterPad(LO* plo, PAD& p)
{
    HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
    if (!p.fNoSound)
    {
        CSnd* ps = PsndNewPlain(2);
        ps->SetSource(plo);
    }
    RunEmbeddedTriggerList(plo, 0, p.ploFound);
    if (!p.fStay) SetPadState(plo, p, 2);
    NotifyLo(plo, 6, static_cast<TRIGGERSTATE*>(&p));
}

// orig: 0x00148860 (the leave actions: event 6, the leave list, message 7)
void LeavePad(LO* plo, PAD& p)
{
    HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
    RunEmbeddedTriggerList(plo, 1, p.ploFound);
    NotifyLo(plo, 7, static_cast<TRIGGERSTATE*>(&p));
}

// orig: 0x00149010 (the deferred call 0x2A4E80 of a touch: an actor just bounced off (its jump kind 3 within 0.15 s)
// or in the jump's state 0x11 is ignored; else state 1)
void PadTouched(LO* plo, PAD& p)
{
    if (ACTOR* pa = p.ploFound ? PactorFromLo(p.ploFound) : nullptr)
    {
        if (pa->jump.state == 0x11 || (pa->jump.kind == 3 && g_clock.t - pa->jump.t20 < 0.15f))
        {
            p.ploFound = nullptr;
            return;
        }
    }
    SetPadState(plo, p, 1);
}

// orig: 0x00149010, queued through 0x149AB8. The callback reads the current
// toucher/jump state at dispatch, after the detached batch has been taken.
void DeferredPadTouched(void* object, int)
{
    LO* plo = static_cast<LO*>(object);
    PadTouched(plo, Pad(plo));
}

// orig: 0x00148C98 (vt slot 16)
void UpdatePad(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's
    PAD& p = Pad(plo);
    switch (p.state)
    {
    case 2:
        // orig: 0x148C98: this hold test applies to every pad kind.
        if (SLY* sly = PslyGlobal(); sly && sly->state == 3 && sly->ploOther == p.ploAction) p.tState = g_clock.t;
        if (0.0f <= p.dtRearm && p.dtRearm < g_clock.t - p.tState) SetPadState(plo, p, 4);
        break;
    case 4:
        if (p.kind == 1) SetPadState(plo, p, 0);
        else if (p.kind == 0 || p.kind == 2 || p.kind == 3) SetPadState(plo, p, 1);
        break;
    case 0: case 1:
    {
        int k = p.state;
        if (p.kind == 0)
        {
            // an object of the lists in contact with the pad (its attachment's overlaps' contacts, not kind 10):
            // the toucher kept and the touch's call queued (0x149AB8); the state stays, else 0
            k = 0;
            ALO* palo = FCidDerivedFrom(plo->Cid(), 0x30) ? static_cast<ALO*>(plo) : nullptr;
            OXA* poxa = palo && palo->psort ? palo->psort->poxa : nullptr;
            LO* ploFound = nullptr;
            for (OX* pox = poxa ? poxa->pox : nullptr; pox && !ploFound; pox = pox->poxNext)
                for (XP* pxp = pox->pxp; pxp && !ploFound; pxp = pxp->pxpNext)
                {
                    if (pxp->xpk == 10) continue;
                    const int i = poxa->pso == pxp->axpd[0].paloRoot ? 0 : 1;
                    if (pxp->axpd[i].palo != palo) continue;
                    LO* po = pxp->axpd[1 - i].paloRoot;
                    if (FPadAccepts(p, po)) ploFound = po;
                    if (ploFound && std::getenv("SLY2N_PADDEBUG"))
                        std::printf("pad %d: touched by %d (xpk %d, entry of %d at %.0f %.0f %.0f, normal %.2f %.2f %.2f)\n", plo->oid, po->oid,
                                    pxp->xpk, pxp->axpd[i].palo ? pxp->axpd[i].palo->oid : -1, pxp->axpd[i].pos.x, pxp->axpd[i].pos.y,
                                    pxp->axpd[i].pos.z, pxp->normal.x, pxp->normal.y, pxp->normal.z);
                }
            if (ploFound)
            {
                p.ploFound = ploFound;
                PostDeferredCall(plo, 0x149010, DeferredPadTouched);
                k = p.state;
            }
        }
        else if (p.kind == 3)
        {
            SLY* sly = PslyGlobal();
            k = sly && sly->state == 3 && sly->ploOther == p.ploAction ? 1 : 0;
        }
        SetPadState(plo, p, k);
        break;
    }
    default: break;
    }
}

// orig: 0x00148F20 (vt slot 75: a contact accepted (0x1F7588), not of the byte 4, from an object of the pad's lists:
// kind 1 goes to state 1 from 0; kind 2 to state 1 from 0 once +0x1E8 s are over since the last change, back to 0
// from 1 (a bell / switch hit with the cane, e.g. Dimitri's boat bell 142627, job 1818's start))
bool FAcceptPadContact(ALO* palo, CTMSG* pctm)
{
    const bool r = pctm->grfAccepted != 0;  // 0x1F7588
    PAD& p = Pad(palo);
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad %d: contact from %d accepted 0x%X kind %d state %d\n", palo->oid, pctm->ploSender ? pctm->ploSender->oid : -1,
                    pctm->grfAccepted, p.kind, p.state);
    if (r && !(pctm->grfAccepted & 4) && FPadAccepts(p, pctm->ploSender))
    {
        switch (p.kind)
        {
        case 1:
            if (p.state == 0) SetPadState(palo, p, 1);
            break;
        case 2:
            if (p.state == 0)
            {
                if (p.dtRearm <= g_clock.t - p.tState) SetPadState(palo, p, 1);
            }
            else if (p.state == 1) SetPadState(palo, p, 0);
            break;
        default: break;
        }
    }
    return r;
}

// orig: 0x001D6848 (the launch velocity from pos to the target: the angle g of the way between the line's elevation
// and the vertical (sin g), with the acceleration acc; 0 when unreachable)
glm::vec3 VelBallistic(float g, const glm::vec3& pos, const glm::vec3& posTarget, const glm::vec3& acc)
{
    float k = std::sin(g);
    glm::vec3 d = posTarget - pos;
    const float z = d.z;
    d.z = 0.0f;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    float angle = std::atan2(z, len);
    if (angle < 0.0f) angle = 0.0f;
    float q = acc.z;  // (dot with (0, 0, 1))
    k = angle + (1.57079637f - angle) * k;
    q = q / ((z - len * std::tan(k)) * 2.0f);
    if (q <= 0.0f) return glm::vec3(0.0f);
    q = std::sqrt(q);
    const float speed = len * q / std::cos(k);
    glm::vec3 v(d.x, d.y, len * std::tan(k));
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0.0f ? v * (speed / l) : glm::vec3(0.0f);
}

struct INITPAD
{
    INITPAD()
    {
        RegisterLoUpdate(0x148C98, UpdatePad);
        // Direct embedded fields must also update the live pad cache when scripts
        // change them. Loading stores these fields before Pad's first update.
        RegisterObjectSetter(413, [](LO* plo, const CRef& r) { Pad(plo).kind = r.RefCoerceS32().m_n; });
        RegisterObjectGetter(413, [](LO* plo) { CRef r; r.SetS32(Pad(plo).kind); return r; });
        RegisterObjectSetter(414, [](LO* plo, const CRef& r) {
            Pad(plo).ploAction = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr;
        });
        RegisterObjectGetter(414, [](LO* plo) { CRef r; r.SetBasic(Pad(plo).ploAction); return r; });
        RegisterObjectSetter(419, [](LO* plo, const CRef& r) { Pad(plo).dtRearm = r.RefCoerceF32().m_g; });
        RegisterObjectGetter(419, [](LO* plo) { CRef r; r.SetF32(Pad(plo).dtRearm); return r; });
        RegisterObjectSetter(421, [](LO* plo, const CRef& r) { Pad(plo).fNoSound = !r.FFalse(); });
        RegisterObjectGetter(421, [](LO* plo) { CRef r; r.SetBool(Pad(plo).fNoSound); return r; });
        RegisterObjectSetter(423, [](LO* plo, const CRef& r) { Pad(plo).fStay = !r.FFalse(); });
        RegisterObjectGetter(423, [](LO* plo) { CRef r; r.SetBool(Pad(plo).fStay); return r; });
        // orig: 0x1490A0 -> 0x148A78 (EOPID426).
        RegisterObjectSetter(426, [](LO* plo, const CRef& r) { SetPadState(plo, Pad(plo), r.RefCoerceS32().m_n); });
        RegisterObjectGetter(426, [](LO* plo) { CRef r; r.SetS32(Pad(plo).state); return r; });
        // EOPID 427: the pad's toucher (+0x2F8)
        RegisterObjectGetter(427, [](LO* plo) {
            CRef r;
            PAD* pp = MpPad().count(plo) ? &MpPad()[plo] : nullptr;
            if (pp && pp->ploFound) r.SetBasic(pp->ploFound);
            else r.SetBasic(nullptr);
            return r;
        });
        // EOPID 759 (0x144068): the actor busy (its AI jobs +0x270 or a script's drive +0x1084)
        RegisterObjectGetter(759, [](LO* plo) {
            CRef r;
            ACTOR* pa = PactorFromLo(plo);
            r.SetBool(pa && (pa->n270 > 0 || pa->prunScript != nullptr));
            return r;
        });
        // EOPIDs 428 / 429 (0x105200 -> 0x1490C0 / 0x105278 -> 0x1490E0): the pad's enter (0x1487A0) / leave
        // (0x148860) actions run by a script (its +0x150 state object); natively stubs (audit 2026-11 P7)
        RegisterObjectMethod(0x105200, [](LO* plo, const std::vector<CRef>&) {
            if (plo && FCidDerivedFrom(plo->Cid(), 0x53)) EnterPad(plo, Pad(plo));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x105278, [](LO* plo, const std::vector<CRef>&) {
            if (plo && FCidDerivedFrom(plo->Cid(), 0x53)) LeavePad(plo, Pad(plo));
            return CRef(TAGK_Void);
        });
        // EOPID 730 (0x10A790 -> 0x141968): the actor launched to a point (g the elevation's share, h the air
        // control's weight): the velocity from its feet (+0x248) to the point by its gravity; without a damage
        // reaction the jump kind 8, the air control +0x40 3 toward that velocity's horizontal part, +0x34 h
        RegisterObjectMethod(0x10A790, [](LO* plo, const std::vector<CRef>& a) {
            ACTOR* pa = PactorFromLo(plo);
            if (!pa || a.size() < 3) return CRef(TAGK_Void);
            glm::vec3 posT = a[0].GetVector();
            if (LO* po = PloArg(a, 0); po && FCidDerivedFrom(po->Cid(), 0x30)) posT = PosAloCenter(static_cast<ALO*>(po));
            const float g = GArg(a, 1), h = GArg(a, 2);
            LaunchActorTo(pa, posT, g, h);  // 0x141968
            return CRef(TAGK_Void);
        });
    }
} s_initpad;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetPads() { MpPad().clear(); }
NATIVEFN(0x148F20, FAcceptPadContact);

// (co-op, coop.h) the other game's switch / bell / plate set here (its enter / leave actions, scripts and all)
bool FCoopApplyPadState(SW* psw, int oid, int iclone, int n)
{
    if (!psw) return false;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo || plo->oid != oid || plo->iclone != iclone || !MpPad().count(plo)) continue;
        PAD& p = Pad(plo);
        if (p.state == n || (n == 1 && p.state != 0)) return false;
        SetPadState(plo, p, n);
        return true;
    }
    return false;
}
