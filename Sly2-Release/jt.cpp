// jt.cpp: the players' interaction objects (jt.h): the lists by kind, the description parts, the pipe's frame.
#include "jt.h"
#include "region.h"

#include "alo.h"
#include "activate.h"
#include "emitter.h"
#include "brx.h"
#include "crv.h"
#include "lo.h"
#include "mat.h"
#include "so.h"
#include "sw.h"
#include "spliceglue.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfmath.h"
#include "vecmat.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using cmm::F32;

namespace
{
struct JTLDEF
{
    int slotAdd;       // the class's vt slot holding the list's add function (23: slot 23 / 22; 11: OnAdd / OnRemove)
    uint32_t pfnAdd;   // the add function (orig 0x11A220 into the list)
    uint32_t ibDesc;   // the description part's offset (0: not known)
};
const JTLDEF c_ajtldef[JTL_Max] = {
    {23, 0x21D358, 0x100},  // JTL_Point (0x13 objects)
    {23, 0x1C33E8, 0x190},  // JTL_Pipe
    {23, 0x1CB9B0, 0x190},  // JTL_Rail
    {23, 0x1FF438, 0xE0},   // JTL_Spire
    {23, 0x183998, 0},      // JTL_Jump (cid 0x58)
    {23, 0x182EB8, 0x190},  // JTL_Ledge
    {23, 0x183370, 0xE0},   // JTL_Wall
    {23, 0x1836F8, 0xE0},   // JTL_Swing
    {23, 0x183FB8, 0xE0},   // JTL_Use
    {11, 0x19C9E8, 0},      // JTL_Jump2 (cid 0x28)
    {11, 0x185C28, 0},      // JTL_Point2 (cid 0x09)
    {23, 0x1844A0, 0x130},  // JTL_Hook
};

// the description getters of the load's options (grf 0x2000): each returns the object plus its part's offset
bool FDescGetter(uint32_t pfn)
{
    switch (pfn)
    {
    case 0x2599F8: case 0x259A08: case 0x259A18: case 0x259A28: case 0x259A38: case 0x259A48: case 0x259A78:
    case 0x259B00: case 0x259B10: case 0x259C10:
        return true;
    default: return false;
    }
}

SW* s_pswLists = nullptr;
int s_nStampLists = -1;
std::array<std::vector<LO*>, JTL_Max> s_aaplo;
}  // namespace

// the class's slot reaches the list's add: the add itself, or the booty stand's 0x13CFD8 (cid 0x25), which calls the
// use's 0x183FB8 first (the only caller of a list add, by the ELF's call sites)
bool FJtListAdd(const CLS& c, JTL jtl)
{
    const JTLDEF& d = c_ajtldef[jtl];
    const uint32_t pfn = c.PfnOrig(d.slotAdd);
    return pfn == d.pfnAdd || (jtl == JTL_Use && pfn == 0x13CFD8);
}

bool FJtInList(LO* plo, JTL jtl)
{
    if (!plo || !plo->pcls || !plo->fInWorld || plo->fRemoved) return false;
    if (!FJtListAdd(*plo->pcls, jtl)) return false;
    // (slots 23 / 22 add to / remove from the update table (0x210268): the original's lists hold the active objects;
    // natively the objects in the world: TODO(world) the Sly harness's activation of the kind-3 objects (pipe 123964,
    // st 3, is active in the reference at vblank 2412 and not natively), then plo->iupd >= 0 here)
    return true;
}

uint32_t IbJtDesc(JTL jtl) { return c_ajtldef[jtl].ibDesc; }

// orig: 0x00184670 (the order also matters for subclasses)
JTL JtlDescFromLo(LO* plo)
{
    if (!plo) return JTL_Max;
    const struct { CID cid; JTL jtl; } kinds[] = {
        {0x58, JTL_Jump}, {0x1B, JTL_Hook}, {0x22, JTL_Wall}, {0xC, JTL_Ledge}, {0x23, JTL_Use},
        {0x13, JTL_Point}, {0xB, JTL_Pipe}, {0xD, JTL_Rail}, {0x27, JTL_Spire},
    };
    for (const auto& kind : kinds)
        if (FIsLoDerivedFrom(plo, kind.cid)) return kind.jtl;
    return JTL_Max;
}

bool FSafeTargetReattach(LO* plo)
{
    if (!plo) return false;
    for (CID cid : {0x13, 0xB, 0xD, 0x27, 0xC, 0x22, 0x26})
        if (FIsLoDerivedFrom(plo, cid)) return true;
    return false;
}

const std::vector<LO*>& AploJtList(SW* psw, JTL jtl)
{
    // (rebuilt each call: the lists are short, and the objects come and go with the world's adds / removes)
    std::vector<LO*>& a = s_aaplo[jtl];
    a.clear();
    if (!psw) return a;
    for (CID cid = 0; cid < CID_Max; ++cid)
    {
        const CLS& c = g_acls[cid];
        if (!FJtListAdd(c, jtl)) continue;
        for (LO* plo : psw->mpcidaplo[cid])
            if (FJtInList(plo, jtl)) a.push_back(plo);
    }
    (void)s_pswLists;
    (void)s_nStampLists;
    return a;
}

namespace
{
// a player's bit (ACTOR +0x1000: Sly 1, Bentley 2, Murray 4) by its class
uint32_t GrfPlayerOfCid(CID cid) { return cid == 0x35 ? 1u : cid == 0x37 ? 2u : cid == 0x36 ? 4u : 0u; }

// A use's players (+0xFC: its description's +0x1C), kept in that field once worked out:
//   0x183F08 (the use's InitLo): 0; cid 0x24's 0x184268: 4 (Murray's);
//   the loader's setter options, in their order: EOPID 1073 (0x184028, objects: each listed one's +0x1000 ORed in, after
//   the load by 0x13E5E0) and EOPID 1074 (0x184040, a class: 0x34 all = 7, 0x35 Sly |= 1, 0x37 Bentley |= 2, 0x36
//   Murray |= 4);
//   0x183F50 (the use's PostLoad): 0 -> 7.
// R49: 1074 was not applied, so the uses only Murray may take (Ep4 Trojan Tank's crank bars 80399 / 80401: 1074 = 0x36)
// were everybody's; Sly took them and the use's script waited for Murray forever.
uint32_t UseMaskInit(LO* plo, uint32_t ib)
{
    const uint32_t uCached = plo->U32Field(0, ib + 0x1C, 0xFFFFFFFFu);
    if (uCached != 0xFFFFFFFFu) return uCached;
    if (!g_psw) return FIsLoDerivedFrom(plo, 0x24) ? 4u : 7u;
    uint32_t mask = FIsLoDerivedFrom(plo, 0x24) ? 4u : 0u;
    for (const OPT& o : plo->aopt)
    {
        if (o.eopid == 1073)
        {
            for (const OPTVAL& v : o.aval)
                for (const auto& p : g_psw->aploAll)
                    if (p->oid == v.n) mask |= GrfPlayerOfCid(p->Cid());
        }
        else if (o.eopid == 1074)
        {
            for (const OPTVAL& v : o.aval)
            {
                if (v.n == 0x34) mask = 7u;
                else mask |= GrfPlayerOfCid(static_cast<CID>(v.n));
            }
        }
    }
    if (!mask) mask = 7u;
    if (std::getenv("SLY2N_JTPRINT")) std::printf("jt use %d: players 0x%X\n", plo->oid, mask);
    plo->SetField(0, ib + 0x1C, mask);
    return mask;
}
}  // namespace

JTDESC JtDescOf(LO* plo, JTL jtl)
{
    JTDESC d;
    const uint32_t ib = IbJtDesc(jtl);
    if (!plo || !ib) return d;
    // the class constructors' defaults (0x182C70: 7 / -1 / FLT_MAX; the subclasses' ctors)
    switch (jtl)
    {
    case JTL_Pipe: d.mask = 1; break;  // the PIPE ctor 0x1C3390 (+0x190 +0x1C = 1: Sly's only)
    case JTL_Use:
        if (FIsLoDerivedFrom(plo, 0x24)) d.prio = -1;  // 0x184268
        else if (FIsLoDerivedFrom(plo, 0x23)) d.prio = 1;  // 0x183F08
        if (FIsLoDerivedFrom(plo, 0x23)) d.mask = UseMaskInit(plo, ib);
        break;
    default: break;
    }
    for (const auto& [key, u] : plo->afield)
    {
        const uint32_t pfn = static_cast<uint32_t>(key >> 32), off = static_cast<uint32_t>(key);
        uint32_t sub;
        if (pfn == 0 && off >= ib && off < ib + 0x20) sub = off - ib;
        else if (FDescGetter(pfn) && off < 0x20) sub = off;
        else continue;
        float g;
        std::memcpy(&g, &u, 4);
        switch (sub)
        {
        case 0x0: d.oidZone = u; break;
        case 0x8: d.height = g; break;
        case 0x10: d.prio = static_cast<int>(u); break;
        case 0x14: d.id = static_cast<int>(u); break;
        case 0x18: d.kind = u; break;
        case 0x1C: d.mask = u; break;
        default: break;
        }
    }
    return d;
}

// orig: 0x001A6318 (vt slot 31: an SO's object record's world matrix; a LOX's matrix in its reference record's frame)
glm::mat4 MatJtLo(LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        if (const XF* pxf = PxfObj(static_cast<ALO*>(plo))) return pxf->matWorld;
        return MatWorldOfAlo(static_cast<ALO*>(plo));
    }
    if (FIsLoDerivedFrom(plo, 3))
    {
        const LOX* plox = static_cast<const LOX*>(plo);
        return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    }
    return glm::mat4(1.0f);
}

// orig: 0x001A6F00
glm::vec3 PosJtConvert(LO* a, LO* b, const glm::vec3& v)
{
    glm::vec3 r = v;
    if (a) r = glm::vec3(MatJtLo(a) * glm::vec4(r, 1.0f));
    if (b) r = glm::vec3(glm::inverse(MatJtLo(b)) * glm::vec4(r, 1.0f));
    return r;
}
// orig: 0x001A6FC0
glm::vec3 VecJtConvert(LO* a, LO* b, const glm::vec3& v)
{
    glm::vec3 r = v;
    if (a) r = glm::mat3(MatJtLo(a)) * r;
    if (b) r = glm::transpose(glm::mat3(MatJtLo(b))) * r;
    return r;
}

const CRV* PcrvJtPipe(LO* ppipe) { return ppipe && FIsLoDerivedFrom(ppipe, 0x0A) ? PcrvFromLo(ppipe) : nullptr; }

// orig: 0x001E6728
float SClampCrv(const CRV* pcrv, float s)
{
    if (pcrv->fClosed) return SWrapCrv(pcrv, s);
    const float s0 = pcrv->as.front(), s1 = SLengthCrv(pcrv);
    if (s < s0) return s0;
    if (s1 < s) return s1;
    return s;
}

namespace
{
glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& vDefault)
{
    const float l = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    if (l < F32(0x38D1B717)) return vDefault;
    return v * (1.0f / l);
}
glm::vec3 VecPipeField(LO* ppipe, uint32_t ib)
{
    glm::vec3 v(0.0f);
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t u = ppipe->U32Field(0, ib + 4 * i, 0);
        std::memcpy(&v[i], &u, 4);
    }
    return v;
}
}  // namespace

// orig: 0x001C3500 (the climber's frame at the distance s: the pipe's direction there (a point 50 either side; when it
// turns back against the start's direction, the nearest of +-20, +-40.. that does not); the start's direction rotated
// onto it (0x11BA08) takes the pipe's vector +0x1C0 along: z the direction, y = z x that vector, x = y x z; fUp keeps
// z up the pipe, else z and y are reversed)
glm::mat3 MatPipeFrame(LO* ppipe, bool fUp, float s)
{
    const CRV* pcrv = PcrvJtPipe(ppipe);
    if (!pcrv) return glm::mat3(1.0f);
    const float s0 = pcrv->as.front(), s1 = SLengthCrv(pcrv);
    glm::vec3 dir0(0.0f);
    EvalCrv(pcrv, pcrv->ag.front(), nullptr, &dir0);  // 0x1E6610: the first point's parameter
    dir0 = NormalizeOr(dir0, glm::vec3(1, 0, 0));
    glm::vec3 t0(0.0f, 0.0f, 1.0f), d(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 64; ++i)
    {
        const float off = (i & 1) ? 10.0f * static_cast<float>(i + 1) : static_cast<float>(i) * -10.0f;
        float s2 = s + off;
        if (!pcrv->fClosed)
        {
            if (s2 < s0) continue;
            if (s1 < s) continue;  // (sic: the original compares the length with s, not with s2)
        }
        else s2 = s0 + GModPositive(s2 - s0, s1 - s0);
        glm::vec3 a(0.0f), b(0.0f);
        EvalCrvS(pcrv, SClampCrv(pcrv, s2 + 50.0f), &a, nullptr);
        EvalCrvS(pcrv, SClampCrv(pcrv, s2 - 50.0f), &b, nullptr);
        d = NormalizeOr(a - b, glm::vec3(0, 0, 1));
        if (i == 0) t0 = d;
        const float dot = (dir0.x * d.x + dir0.y * d.y) + dir0.z * d.z;
        if (F32(0xBF666666) < dot) break;
    }
    const glm::mat3 m = BuildRotateVectorsMatrix(dir0, d);  // 0x11BA08
    const glm::vec3 v = VecJtConvert(ppipe, nullptr, VecPipeField(ppipe, 0x1C0));
    const glm::vec3 w = m * v;
    glm::vec3 t = NormalizeOr(t0, glm::vec3(0, 0, 1));
    glm::vec3 side = glm::cross(t, w);
    {
        const float l = std::sqrt((side.x * side.x + side.y * side.y) + side.z * side.z);
        side = l < F32(0x38D1B717) ? PerpendicularVector(t) : side * (1.0f / l);
    }
    const glm::vec3 front = glm::cross(side, t);
    if (!fUp)
    {
        t = -t;
        side = -side;
    }
    glm::mat3 r;
    r[0] = front;
    r[1] = side;
    r[2] = t;
    return r;
}

// (the defaults are the pipe's constructor's, 0x1C3390: attach 8 (the hook's clink), release 9; a world's option
// replaces them. Without them no pipe in the game made its sounds: R31)
int NPipeEffect(LO* ppipe, bool fRelease)
{
    return ppipe ? static_cast<int>(ppipe->U32Field(0, fRelease ? 0x1D4 : 0x1D0, fRelease ? 9 : 8)) : 0;
}

void PrintJtLists(SW* psw)
{
    static const char* s_aname[JTL_Max] = {"point", "pipe", "rail", "spire", "jump", "ledge", "wall", "swing", "use", "jump2", "point2", "hook"};
    for (int i = 0; i < JTL_Max; ++i)
    {
        const std::vector<LO*>& a = AploJtList(psw, static_cast<JTL>(i));
        if (a.empty()) continue;
        std::printf("jt list %s: %zu\n", s_aname[i], a.size());
        for (LO* plo : a)
        {
            const JTDESC d = JtDescOf(plo, static_cast<JTL>(i));
            const glm::vec3 p = PosJtLo(plo);
            std::printf("   %d cid 0x%X at %.0f %.0f %.0f: height %g prio %d id 0x%X kind 0x%X mask 0x%X fields", plo->oid, plo->Cid(), p.x, p.y,
                        p.z, d.height, d.prio, d.id, d.kind, d.mask);
            for (const auto& [key, u] : plo->afield)
                std::printf(" (%X,%X)=%X", static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key), u);
            std::printf("\n");
            if (const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr)
            {
                const float len = SLengthCrv(pcrv);
                std::printf("      curve len %.0f closed %d:", len, pcrv->fClosed ? 1 : 0);
                for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
                {
                    glm::vec3 q(0.0f), dq(0.0f);
                    EvalCrvS(pcrv, u * len, &q, &dq);
                    std::printf(" (%.0f %.0f %.0f)", q.x, q.y, q.z);
                }
                std::printf("\n");
            }
        }
    }
}

// orig: D_002DFB84 (0x184510's alternate linked-object XY branch).
// No gameplay writer was found in the matched sources; retain the original mode for controlled comparisons.
bool g_fHookLinkedPosition = false;

namespace
{
// The hook's authored point, transformed through its own reference frame (0x184510 / 0x1845A0).
glm::vec3 PosHookAnchor(LO* plo)
{
    const glm::vec3 local = static_cast<LOXSHAPE*>(plo)->posBound;
    return PosJtConvert(plo, nullptr, local);
}

// orig: 0x00184510. A reference option is stored as an OID by the native loader.
// Resolve it in the hook's namespace, just as the original's loaded object reference.
glm::vec3 PosHookTarget(LO* plo)
{
    const OID oid = static_cast<OID>(plo->U32Field(0, 0x15C, 0));
    LO* linked = oid ? PloFindRelative(plo, oid) : nullptr;
    const glm::vec3 linkedPos = linked ? PosJtLo(linked) : PosJtLo(plo);
    glm::vec3 point = g_fHookLinkedPosition ? linkedPos : PosHookAnchor(plo);
    point.z = linkedPos.z + 50.0f;
    if (static const bool s_fPrint = std::getenv("SLY2N_HOOKPOINT") != nullptr; s_fPrint)
    {
        const glm::vec3 center = PosJtLo(plo);
        const glm::vec3 anchor = PosHookAnchor(plo);
        std::printf("HOOK_TARGET {\"oid\":%u,\"center\":[%.9g,%.9g,%.9g],\"anchor\":[%.9g,%.9g,%.9g],\"point\":[%.9g,%.9g,%.9g]}\n", plo->oid, center.x, center.y, center.z, anchor.x, anchor.y, anchor.z, point.x, point.y, point.z);
    }
    return point;
}
}

// ---- the candidates (0x194A40 / 0x194688 / 0x194F70) ----
#include "actor.h"
#include "hudwidget.h"
#include "sly.h"
#include "po.h"
#include "pickup.h"
#include "task.h"
#include "clock.h"

#include <algorithm>
#include <map>
#include <string>

glm::vec3 PosActorProbe0(ACTOR* pa);  // jump.cpp (+0x218: the custom entry's probe point)

JTTRACK g_jttrack;
void ResetJtTrack() { g_jttrack = JTTRACK{}; }
LO* PloNewJtAura(LO* pproxy, const JTENT& e);

// orig: 0x00195608
void ShowJtAura(SW* psw, LO* plo)
{
    if (!plo || !psw) return;
    static LO* s_pproxy = nullptr;  // SW +0x4744 (0x1957C0 at the world start: the object 0x624 found globally)
    static uint32_t s_serial = 0;
    if (s_serial != psw->serial)
    {
        s_serial = psw->serial;
        s_pproxy = PloFindSwObjectByOid(psw, 0x624);
    }
    if (std::getenv("SLY2N_AURAPRINT"))
    {
        std::printf("jt aura %d: proxy %d, %d tracked:", plo->oid, s_pproxy ? s_pproxy->oid : -1, g_jttrack.c);
        for (int i = 0; i < g_jttrack.c; ++i) std::printf(" %d", g_jttrack.ae[i].plo ? g_jttrack.ae[i].plo->oid : -1);
        std::printf("\n");
    }
    if (!s_pproxy) return;
    for (int i = 0; i < g_jttrack.c; ++i)
    {
        JTENT& e = g_jttrack.ae[i];
        if (e.plo != plo) continue;
        if (LO* pnew = PloNewJtAura(s_pproxy, e))
        {
            if (e.ploAura) RemoveLo(psw, e.ploAura);  // vt slot 9
            e.ploAura = pnew;
        }
        return;
    }
}

namespace
{
void WarnOnceJt(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "jt: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

// a curve candidate's distance nearest the point q, kept off its ends unless closed (the methods 2 of the pipe
// 0x1C3958 (100 / 35), the rail 0x1CBAC0 (50 / 50), the ledge 0x183040 (30 / 30))
float GCurveNearest(const CRV* pcrv, const glm::vec3& q, float sLo, float sHi)
{
    float s = 0.0f;
    NearestCrvAll(pcrv, q, nullptr, nullptr, nullptr, &s);
    if (pcrv->fClosed) return s;
    const float len = SLengthCrv(pcrv);
    if (s < sLo) return sLo;
    if (len - sHi < s) return len - sHi;
    return s;
}

// method 1 (0x1C3BB0 / 0x1CBA68 / 0x182FE8; the others 0x182CC0): a closed curve's range
bool FJtRange(LO* plo, JTL jtl, float* pgMin, float* pgMax)
{
    if (jtl != JTL_Pipe && jtl != JTL_Rail && jtl != JTL_Ledge) return false;
    const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr;
    if (!pcrv || !pcrv->fClosed) return false;
    *pgMin = pcrv->as.front();
    *pgMax = SLengthCrv(pcrv);
    return true;
}

// method 2: the point's parameter for the actor
float GJtInit(LO* plo, JTL jtl, ACTOR* pa)
{
    const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr;
    switch (jtl)
    {
    case JTL_Pipe: return pcrv ? GCurveNearest(pcrv, PosAloCenter(pa->palo), 100.0f, 35.0f) : 0.0f;  // 0x1C3958
    case JTL_Rail: return pcrv ? GCurveNearest(pcrv, PosActorProbe0(pa), 50.0f, 50.0f) : 0.0f;            // 0x1CBAC0
    case JTL_Ledge: return pcrv ? GCurveNearest(pcrv, PosActorProbe0(pa), 30.0f, 30.0f) : 0.0f;           // 0x183040
    case JTL_Wall:
    {
        // orig: 0x001834D0
        const glm::vec3 d = PosActorProbe0(pa) - PosJtLo(plo);
        const uint32_t u = plo->U32Field(0, 0x10C, 0);
        float radius;
        std::memcpy(&radius, &u, sizeof(radius));
        return std::atan2(d.y, d.x) * radius;
    }
    default: return 0.0f;  // 0x182CC8
    }
}

// method 3: the point at g
glm::vec3 PosJt(LO* plo, JTL jtl, ACTOR* pa, float g)
{
    const glm::vec3 base(pa->vecBase);
    const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr;
    switch (jtl)
    {
    case JTL_Point: return PosJtLo(plo) + glm::vec3(0.0f, 0.0f, -150.0f);  // 0x184F98, D_002BE910
    case JTL_Pipe:
    {
        // 0x1C3A08: the pipe's point, 50 out of its frame's x row
        glm::vec3 pos(0.0f);
        if (pcrv) EvalCrvS(pcrv, g, &pos, nullptr);
        const glm::mat3 m = MatPipeFrame(plo, true, g);
        return pos + m[0] * F32(0xC2480000);
    }
    case JTL_Rail:
    {
        glm::vec3 pos(0.0f);
        if (pcrv) EvalCrvS(pcrv, g, &pos, nullptr);
        return pos - base;  // 0x1CBB58
    }
    case JTL_Ledge:
    {
        // 0x1830D8: the edge's point out along dir x z by the actor's +0x103C, less +0x240
        glm::vec3 pos(0.0f), dir(0.0f);
        if (pcrv) EvalCrvS(pcrv, g, &pos, &dir);
        glm::vec3 n = glm::cross(dir, glm::vec3(0.0f, 0.0f, 1.0f));
        const float l = std::sqrt((n.x * n.x + n.y * n.y) + n.z * n.z);
        n = l < F32(0x38D1B717) ? glm::vec3(1.0f, 0.0f, 0.0f) : n * (1.0f / l);
        return (pos + n * pa->g103C) - base;
    }
    case JTL_Spire: return PosJtLo(plo) - base;  // 0x1FF4F0
    case JTL_Wall:
    {
        // orig: 0x00183540 (the parameter is arc length around the wall)
        const uint32_t u = plo->U32Field(0, 0x10C, 0);
        float radius;
        std::memcpy(&radius, &u, sizeof(radius));
        return PosJtLo(plo) + cmm::SetVectorCylind(g / radius, radius + pa->g103C, 0.0f) - base;
    }
    case JTL_Swing:
    {
        // orig: 0x00183818 (kind 0 has the actor's outward offset)
        const float offset = plo->U32Field(0, 0x10C, 0) == 0 ? pa->g103C : 0.0f;
        return (PosJtLo(plo) - base) + glm::vec3(MatJtLo(plo)[0]) * offset;
    }
    case JTL_Use: return PosJtLo(plo);            // 0x184208
    case JTL_Hook: return PosHookTarget(plo);  // 0x184510
    default: WarnOnceJt("the description methods of list " + std::to_string(jtl)); return PosJtLo(plo);
    }
}

// method 4: the distance adjusted (the uses: less their radius +0x110 (200) - 100, 0x184238)
float DistJtAdjust(LO* plo, JTL jtl, float d)
{
    if (jtl != JTL_Use) return d;
    float r = 200.0f;
    const uint32_t u = plo->U32Field(0, 0x110, 0);
    if (u) std::memcpy(&r, &u, 4);
    return d - (r - (250.0f - 150.0f));
}

// orig: 0x00194688
void RateJt(ACTOR* pa, const glm::vec3& pos, LO* plo, JTL jtl, JTENT* aent, int c)
{
    if (plo->paloParent)
    {
        const glm::vec3 d = PosJtLo(plo) - pos;
        if (jtl == JTL_Use && std::getenv("SLY2N_JTPRINT") && !std::strcmp(std::getenv("SLY2N_JTPRINT"), "4"))
            std::printf("jt use %d: radius %.1f distance %.1f (at %.0f %.0f %.0f, actor %.0f %.0f %.0f)\n", plo->oid, plo->sRadius, glm::length(d),
                        PosJtLo(plo).x, PosJtLo(plo).y, PosJtLo(plo).z, pos.x, pos.y, pos.z);
        if (plo->sRadius * plo->sRadius < (d.x * d.x + d.y * d.y) + d.z * d.z) return;
    }
    if (plo == pa->jump.ploC || plo == pa->jump.ploLeft || plo == pa->ploF5C) return;
    // TODO(actors): the carried +0x4D4 and its group (+0x4D8), Sly's +0x18C4 / +0x1F18, 0x2E1E4C's exclusions
    if (SLY* p = dynamic_cast<SLY*>(pa); p && plo == p->ploOther) return;
    const JTDESC desc = JtDescOf(plo, jtl);
    if (jtl == JTL_Use && std::getenv("SLY2N_JTPRINT") && std::atoi(std::getenv("SLY2N_JTPRINT")) >= 3)
        std::printf("jt rate use %d: mask 0x%X allowed 0x%X\n", plo->oid, desc.mask, static_cast<uint32_t>(pa->n1000));
    if (!(static_cast<uint32_t>(pa->n1000) & desc.mask)) return;
    JTENT e;
    e.plo = plo;
    e.jtl = jtl;
    e.g = GJtInit(plo, jtl, pa);
    e.pos = PosJt(plo, jtl, pa, e.g);
    const glm::vec3 d = pos - e.pos;
    e.prio = desc.prio;
    e.dist = DistJtAdjust(plo, jtl, 0.0f + std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z));
    if (std::getenv("SLY2N_JTPRINT") && jtl == JTL_Pipe) std::printf("jt rate pipe %d: g %.1f dist %.1f\n", plo->oid, e.g, e.dist);
    if (1000.0f < e.dist) return;
    int i = c;
    if (desc.kind)
    {
        // one per group: a nearer one replaces it
        for (int j = 0; j < c && aent[j].plo; ++j)
        {
            if (JtDescOf(aent[j].plo, aent[j].jtl).kind != desc.kind) continue;
            if (!(e.dist < aent[j].dist)) return;
            for (int k = j; k < c - 1; ++k) aent[k] = aent[k + 1];
            aent[c - 1] = JTENT{};
            break;
        }
    }
    // after the higher priorities and the same priority's nearer ones
    for (;;)
    {
        const JTENT& q = aent[i - 1];
        --i;
        if (e.prio < q.prio || (q.prio == e.prio && q.dist < e.dist))
        {
            ++i;
            break;
        }
        if (i == 0) break;
    }
    if (i == c) return;
    for (int k = c - 1; k > i; --k) aent[k] = aent[k - 1];
    e.t = g_clock.t;
    e.v = 0.0f;
    aent[i] = e;
}

}  // namespace
// orig: 0x001956D0 (the aura: an instance of the proxy, out of any parent and the world, faded in over 0.25 s at the
// entry's point, unrotated and still)
LO* PloNewJtAura(LO* pproxy, const JTENT& e)
{
    if (!pproxy) return nullptr;
    LO* plo = PloAttachProxy(g_psw, pproxy, 0);  // 0x1CB250
    if (!plo) return nullptr;
    SetLoParentNoneRoot(plo);                    // 0x1A58D0(p, 0, 0) (the root record unrotated: the instance was the
                                                 // player's part, so it kept the player's facing and the emblem faced away)
    RemoveLo(g_psw, plo);                        // vt slot 9
    SwitchLoFaded(plo, true, 0.25f);             // 0x1F7C68
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        SetAloPos(palo, e.pos);                    // vt slot 29
        SetAloRotation(palo, glm::mat3(1.0f));     // vt slot 30 (D_002A2D00)
        SetAloVelocityTo(palo, glm::vec3(0.0f));   // vt slots 35 / 36 (D_002A2DD0)
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
    }
    if (std::getenv("SLY2N_AURAPRINT"))
        std::printf("aura: %d (proxy %d) on %d at %.0f %.0f %.0f t %.3f\n", plo->oid, pproxy->oid, e.plo ? e.plo->oid : -1, e.pos.x, e.pos.y,
                    e.pos.z, g_clock.t);
    return plo;
}

namespace
{
// the player's aura proxy (its part +0x9C4: Sly 1569, Bentley 1570, Murray 1571)
LO* PloAuraProxy(ACTOR* pa)
{
    if (!pa || !pa->palo) return nullptr;
    for (const auto& [ib, plo] : pa->palo->apart)
        if (ib == 0x9C4) return plo;
    return nullptr;
}

// orig: 0x00194F70 (the tracked targets: kept ones updated, their aura following its point; dropped ones' auras faded
// out over 0.5 s; new ones added with the game event 0x3D, the nearest prompted one with the player's aura)
void TrackJt(ACTOR* pa, int n, JTENT* aent)
{
    auto byDist = [](const JTENT& a, const JTENT& b) { return a.dist < b.dist; };
    if (n > 0)
    {
        std::stable_sort(aent, aent + n, byDist);  // (the original's qsort)
        if (JtDescOf(aent[0].plo, aent[0].jtl).id != -1) aent[0].prio = 1;
    }
    JTTRACK& tr = g_jttrack;
    for (int i = tr.c - 1; i >= 0; --i)
    {
        JTENT& old = tr.ae[i];
        int j = n - 1;
        for (; j >= 0; --j)
            if (old.plo == aent[j].plo) break;
        if (j < 0)
        {
            if (old.ploAura) SwitchLoFaded(old.ploAura, false, 0.5f);  // 0x1F7BF8
            for (int k = i; k < 3; ++k) tr.ae[k] = tr.ae[k + 1];
            tr.ae[3] = JTENT{};
            --tr.c;
            continue;
        }
        old.dist = aent[j].dist;
        if (!old.ploAura)
        {
            old.g = aent[j].g;
            old.v = 0.0f;
        }
        else
        {
            // the parameter followed (across a closed curve's seam the short way: method 1's range), smoothed
            // (0x11D018 with D_002BF718 {1500, 0, 2}; D_002E1E50's spring is off), the aura moved to its point
            float gMin = 0.0f, gMax = 0.0f;
            bool fWrap = FJtRange(old.plo, old.jtl, &gMin, &gMax);
            const float gRange = gMax - gMin;
            if (fWrap)
            {
                float dg = aent[j].g - old.g;
                if (gRange * 0.5f < dg) dg -= gRange;
                else if (dg < gRange * -0.5f) dg += gRange;
                else fWrap = false;
                aent[j].g = old.g + dg;
            }
            static const SMP s_smp{1500.0f, 0.0f, 2.0f};
            old.g = GSmooth(old.g, aent[j].g, g_clock.dt, &s_smp, &old.v);
            if (fWrap)
            {
                if (gMax < old.g) old.g -= gRange;
                else if (old.g < gMin) old.g += gRange;
            }
            if (FIsLoDerivedFrom(old.ploAura, 0x30)) SetAloPos(static_cast<ALO*>(old.ploAura), PosJt(old.plo, old.jtl, pa, old.g));  // method 3, slot 29
        }
        for (int k = j; k < n - 1; ++k) aent[k] = aent[k + 1];
        --n;
    }
    LO* pproxy = PloAuraProxy(pa);  // +0x9C4
    for (int k = 0; k < n && tr.c < 4; ++k)
    {
        if (pproxy && aent[k].prio == 1)
        {
            aent[k].ploAura = PloNewJtAura(pproxy, aent[k]);  // 0x1956D0
            if (!aent[k].ploAura) break;  // (the original leaves the rest untracked when no instance is free)
        }
        tr.ae[tr.c++] = aent[k];
        if (std::getenv("SLY2N_AURAPRINT")) std::printf("jt track %d prio %d t %.3f\n", aent[k].plo ? aent[k].plo->oid : -1, aent[k].prio, g_clock.t);
        BroadcastTaskEvent(g_psw, 0x3D, aent[k].plo);  // 0x1619A0
    }
    std::stable_sort(tr.ae, tr.ae + tr.c, byDist);
}
}  // namespace

void DropJtTrack() { TrackJt(nullptr, 0, nullptr); }

// the description's methods 2 / 3 (0x182CD8) for the interaction: the parameter, then the point and the facing angle
// (the ledge's 0x1830D8: atan2 of its outward normal, flipped by the actor's +0x1040; the others: the actor's heading)
float GJtDescInit(LO* plo, JTL jtl, ACTOR* pa) { return GJtInit(plo, jtl, pa); }
glm::vec3 PosJtDesc(LO* plo, JTL jtl, ACTOR* pa, float g, float* prad)
{
    const glm::vec3 pos = PosJt(plo, jtl, pa, g);
    if (prad)
    {
        *prad = pa->radHeading;
        const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr;
        if (jtl == JTL_Ledge && pcrv)
        {
            glm::vec3 p(0.0f), dir(0.0f);
            EvalCrvS(pcrv, g, &p, &dir);
            glm::vec3 n = glm::cross(dir, glm::vec3(0.0f, 0.0f, 1.0f));
            const float l = std::sqrt((n.x * n.x + n.y * n.y) + n.z * n.z);
            n = l < F32(0x38D1B717) ? glm::vec3(1.0f, 0.0f, 0.0f) : n * (1.0f / l);
            if (pa->n1040 == 1) n = -n;
            *prad = std::atan2(n.y, n.x);
        }
        else if (jtl == JTL_Wall)
        {
            const uint32_t u = plo->U32Field(0, 0x10C, 0);
            float radius;
            std::memcpy(&radius, &u, sizeof(radius));
            *prad = g / radius + (pa->n1040 == 1 ? 3.1415927f : 0.0f);  // 0x183540
        }
        else if (jtl == JTL_Swing)
        {
            const glm::vec3 x = MatJtLo(plo)[0];
            *prad = std::atan2(x.y, x.x) + (pa->n1040 == 1 ? 3.1415927f : 0.0f);  // 0x183818
        }
        else WarnOnceJt("the description's facing angle (method 3) of list " + std::to_string(jtl));
    }
    return pos;
}

// orig: 0x001E8008 (a random distance on the curve around s within spread: the range kept on an open curve, the draw
// r |r| (D_002C3650 = 1), a closed curve's distance wrapped (0x1E66C8))
float SRandCrvNear(const CRV* pcrv, float s, float spread)
{
    float lo, hi;
    const float len = SLengthCrv(pcrv);  // 0x1E6648
    if (pcrv->fClosed)
    {
        hi = s + spread;
        lo = s - spread;
    }
    else
    {
        const float c = s < spread ? spread : (len - spread < s ? len - spread : s);
        hi = std::min(c + spread, len);
        lo = std::max(c - spread, 0.0f);
    }
    float r = GRandInRange(-1.0f, 1.0f);
    r = r * std::fabs(r);
    const float u = r * 0.5f + 0.5f;
    float g = (1.0f - u) * lo + u * hi;
    if (pcrv->fClosed)
    {
        const float s0 = pcrv->as.empty() ? 0.0f : pcrv->as.front();
        g = s0 + GModPositive(g - s0, len - s0);
    }
    return g;
}

namespace
{
// orig: 0x0011E1A0 SetVectorCylind (cos r, sin r, z) / 0x0011E208 SetVectorSphere
glm::vec3 VecCylind(float rad, float r, float z) { return glm::vec3(std::cos(rad) * r, std::sin(rad) * r, z); }
glm::vec3 VecSphere(float radPan, float radTilt, float r)
{
    return glm::vec3(std::cos(radTilt) * std::cos(radPan) * r, std::cos(radTilt) * std::sin(radPan) * r, std::sin(radTilt) * r);
}
// the unit horizontal normal dir x z (D_002A2DC0), (1, 0, 0) when dir is vertical
glm::vec3 VecSideOf(const glm::vec3& dir)
{
    const glm::vec3 c = glm::cross(dir, glm::vec3(0.0f, 0.0f, 1.0f));
    const float l = std::sqrt((c.x * c.x + c.y * c.y) + c.z * c.z);
    return l < F32(0x38D1B717) ? glm::vec3(1.0f, 0.0f, 0.0f) : c * (1.0f / l);
}
}  // namespace

// the description's method 5: a random point near the target (the sparkles' place) and the direction they leave in
void RandomJtPoint(LO* plo, JTL jtl, float g, glm::vec3* ppos, glm::vec3* pdir)
{
    const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0A) ? PcrvFromLo(plo) : nullptr;
    glm::vec3 pos(0.0f), dir(0.0f);
    switch (jtl)
    {
    case JTL_Pipe:
        if (pcrv)
        {
            // orig: 0x001C3A88 (around the pipe in its frame (0x1C3500, up), out to 25)
            const float s = SRandCrvNear(pcrv, g, 150.0f);  // D_002C0D10
            const glm::mat3 m = MatPipeFrame(plo, true, s);
            EvalCrvS(pcrv, s, &pos, nullptr);  // 0x1E6F10
            const float n = GRandInRange(-1.0f, 1.0f);
            const float mag = std::fabs(n);
            const glm::vec3 v = VecCylind(n * mag * 1.8f, -1.0f, 0.0f);  // D_002C0D08
            dir = m[0] * v.x + m[1] * v.y + m[2] * v.z;
            *ppos = pos + dir * (mag * GRandInRange(0.0f, 25.0f));  // D_002C0D0C
            *pdir = dir;
            if (std::getenv("SLY2N_SPARKLEPRINT"))
                std::printf("sparkle pipe %d s %.0f curve %.0f %.0f %.0f x row %.2f %.2f %.2f dir %.2f %.2f %.2f\n", plo->oid, s, pos.x, pos.y, pos.z,
                            m[0].x, m[0].y, m[0].z, dir.x, dir.y, dir.z);
            return;
        }
        break;
    case JTL_Rail:
        if (pcrv)
        {
            // orig: 0x001CBBA8 (around the wire: its side, its side x dir, out to 25)
            const float s = SRandCrvNear(pcrv, g, 150.0f);  // D_002C1EC4
            EvalCrvS(pcrv, s, &pos, &dir);
            const glm::vec3 x = VecSideOf(dir);
            const glm::vec3 y = glm::cross(x, dir);
            const float n = GRandInRange(-1.0f, 1.0f);
            const float mag = std::fabs(n);
            const glm::vec3 v = VecCylind(n * mag * 1.8f, 1.0f, 0.0f);  // D_002C1EC8
            const glm::vec3 d = y * v.x + x * v.y + dir * v.z;
            *ppos = pos + d * (mag * GRandInRange(0.0f, 25.0f));  // D_002C1ECC
            *pdir = d;
            return;
        }
        break;
    case JTL_Ledge:
        if (pcrv)
        {
            // orig: 0x00183218 (out from the edge to 50, rising)
            const float s = SRandCrvNear(pcrv, g, 150.0f);  // D_002BE8B8
            EvalCrvS(pcrv, s, &pos, &dir);
            *ppos = pos + VecSideOf(dir) * GRandInRange(0.0f, 50.0f);  // D_002BE8BC
            *pdir = glm::vec3(0.0f, 0.0f, 1.0f);
            return;
        }
        break;
    case JTL_Hook:
    {
        // orig: 0x001845A0 (a 75-unit disk around the transformed authored anchor)
        const float pan = GRandInRange(-F32(0x40490FDB), F32(0x40490FDB));
        const float r = GRandInRange(0.0f, 75.0f);  // D_002BE90C
        *ppos = PosHookAnchor(plo) + VecCylind(pan, r, 0.0f);
        *pdir = glm::vec3(0.0f, 0.0f, 1.0f);
        if (std::getenv("SLY2N_HOOKPOINT"))
            std::printf("HOOK_SPARK {\"oid\":%u,\"pos\":[%.9g,%.9g,%.9g]}\n", plo->oid, ppos->x, ppos->y, ppos->z);
        return;
    }
    default: break;
    }
    // orig: 0x00182D78 (the others: a sphere of 35 around the object's centre, the tilt r |r| pi / 2)
    const float pan = GRandInRange(-F32(0x40490FDB), F32(0x40490FDB));
    const float t = GRandInRange(-1.0f, 1.0f);
    const float tilt = std::fabs(t) * t * 1.5707964f;
    const float r = GRandInRange(0.0f, 35.0f);  // D_002BE8B0
    const glm::vec3 c = FIsLoDerivedFrom(plo, 0x30) ? PosAloCenter(static_cast<ALO*>(plo)) : PosJtLo(plo);  // 0x1A62E8
    *ppos = c + VecSphere(pan, tilt, r);
    *pdir = glm::vec3(0.0f, 0.0f, 1.0f);
}

// orig: 0x00195368 (the player's emitter part +0x12F8 (oid 385) sprays at the candidates: per candidate with a
// priority, its rate (EMITTER +0xFC) x dt particles (the fraction by a draw), at most 10 in all, each at the
// description's random point (method 5), its speed EMITB +0x50..+0x54 along the direction, born +0xA0..+0xA4 ago,
// living +0x8C, advanced by +0x98..+0x9C when they differ; into the particle system (0x15B428))
void SprayJtSparkles(ACTOR* pa, int n, const JTENT* aent)
{
    LO* pemit = nullptr;
    for (const auto& [ib, plo] : pa->palo->apart)
        if (ib == 0x12F8) pemit = plo;
    if (!pemit || n <= 0) return;
    glm::vec3 apos[10], avel[10];
    float at0[10], at1[10];
    int c = 0;
    for (int k = 0; k < n && c < 10; ++k)
    {
        const JTENT& e = aent[k];
        if (e.prio == -1 || !e.plo) continue;
        float g = GEmitterLo(pemit, false, 0xFC) * g_clock.dt;
        const float r = GRandInRange(0.0f, 1.0f);
        int cNew = static_cast<int>(g);
        g -= static_cast<float>(cNew);
        if (r < g) ++cNew;
        for (; cNew > 0 && c < 10; --cNew)
        {
            glm::vec3 pos(0.0f), dir(0.0f);
            RandomJtPoint(e.plo, e.jtl, e.g, &pos, &dir);
            const glm::vec3 vel = dir * GRandInRange(GEmitterLo(pemit, true, 0x50), GEmitterLo(pemit, true, 0x54));
            const float t0 = g_clock.t - GRandInRange(GEmitterLo(pemit, true, 0xA0), GEmitterLo(pemit, true, 0xA4));
            at0[c] = t0;
            at1[c] = t0 + GEmitterLo(pemit, true, 0x8C);
            const float g98 = GEmitterLo(pemit, true, 0x98), g9C = GEmitterLo(pemit, true, 0x9C);
            if (g98 != g9C) pos += vel * GRandInRange(g98, g9C);
            apos[c] = pos;
            avel[c] = vel;
            ++c;
        }
    }
    if (c > 0) EmitLoParticlesAt(pemit, c, apos, avel, at0, at1);
    if (std::getenv("SLY2N_SPARKLEPRINT") && c > 0)
        std::printf("sparkles t %.3f: %d at %.0f %.0f %.0f (emitter %d %p of %d)\n", g_clock.t, c, apos[0].x, apos[0].y, apos[0].z, pemit->oid, (void*)pemit,
                    pa->palo->oid);
}

// orig: 0x00194A40
void UpdateJtCandidates(ACTOR* pa, uint32_t grf)
{
    if (!pa || pa->palo != PloPoCurrent(g_psw)) return;  // 0x1C40C0
    JTENT aent[4];
    const glm::vec3 pos = PosAloCenter(pa->palo);
    auto scan = [&](JTL jtl) {
        for (LO* plo : AploJtList(g_psw, jtl)) RateJt(pa, pos, plo, jtl, aent, 4);
    };
    if (grf & 8) scan(JTL_Ledge);
    if (grf & 0x10) scan(JTL_Wall);
    if (grf & 0x80) scan(JTL_Use);
    if (grf & 4) scan(JTL_Hook);
    if (grf & 0x400) scan(JTL_Spire);
    if (grf & 0x200) scan(JTL_Rail);
    if (grf & 2) scan(JTL_Pipe);
    if (grf & 1)
        for (LO* plo : AploJtList(g_psw, JTL_Point))
            if (FIsLoDerivedFrom(plo, 0x13) && (plo->U32Field(0, 0xE8, 0) & 1)) RateJt(pa, pos, plo, JTL_Point, aent, 4);
    if (grf & 0x20) scan(JTL_Swing);
    int n = 4;
    if (!aent[3].plo)
    {
        n = 3;
        while (n > 0 && !aent[n - 1].plo) --n;
    }
    SprayJtSparkles(pa, n, aent);  // 0x195368
    TrackJt(pa, n, aent);
    // the prompt of the nearest within 250 (D_002BF660), left alone while another's prompt shows (0x1DA5B0); an
    // abandon icon on it (the 0x624 proxy's instance, 0x195608: a job's exit) found after 1 s (D_002BF70C) gives
    // 0x125 ("Warning! Using this passageway will abandon ...")
    const int idOld = IdHudPrompt();
    if (idOld != -1 && idOld != g_jttrack.idShown) return;
    int id = -1;
    if (g_jttrack.c)
    {
        const glm::vec3 d = pos - g_jttrack.ae[0].pos;
        if (!(250.0f < 0.0f + std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z)))
        {
            LO* pproxy = g_jttrack.ae[0].ploAura ? PproxyOfInstance(g_jttrack.ae[0].ploAura) : nullptr;  // 0x1CB658
            if (pproxy && pproxy->oid == 0x624 && 1.0f < g_jttrack.ae[0].t) id = IdText(0x125);
            else id = JtDescOf(g_jttrack.ae[0].plo, g_jttrack.ae[0].jtl).id;
        }
    }
    const bool fChanged = id != idOld;
    if (fChanged)
    {
        if (id == -1) HudPromptEnd(g_jttrack.idShown);
        else HudPrompt(id, -1.0f);
        g_jttrack.idShown = id;
    }
    if (std::getenv("SLY2N_JTPRINT") && (fChanged || std::getenv("SLY2N_JTPRINT")[0] == '2'))
    {
        std::printf("jt t %.3f: %d tracked:", g_clock.t, g_jttrack.c);
        for (int i = 0; i < g_jttrack.c; ++i) std::printf(" %d (list %d, %.1f)", g_jttrack.ae[i].plo->oid, g_jttrack.ae[i].jtl, g_jttrack.ae[i].dist);
        std::printf("; prompt %d\n", id);
    }
}

// orig: 0x001938C0 (the choice part)
int KindJtCircle(ACTOR* pa, LO** pplo)
{
    for (int i = 0; i < g_jttrack.c; ++i)
    {
        const JTENT& e = g_jttrack.ae[i];
        if (250.0f < e.dist) break;
        LO* plo = e.plo;
        int k = -1;
        if (FIsLoDerivedFrom(plo, 0x13) || FIsLoDerivedFrom(plo, 0x0B)) k = dynamic_cast<SLY*>(pa) && pa->palo == PloPoCurrent(g_psw) ? 6 : 5;
        else if (FIsLoDerivedFrom(plo, 0x1B)) k = 8;
        else if (FIsLoDerivedFrom(plo, 0x0C) || FIsLoDerivedFrom(plo, 0x22)) k = 0;
        else if (FIsLoDerivedFrom(plo, 0x26)) k = static_cast<int>(plo->U32Field(0, 0x10C, 0));
        else if (FIsLoDerivedFrom(plo, 0x23)) k = 7;
        if (k != -1)
        {
            if (pplo) *pplo = plo;
            return k;
        }
    }
    return -1;
}

bool FUseAllows(LO* puse, const ACTOR* pa)
{
    // orig 0x001841F0 (EOPID 1072's method 0x10D660): the use's players AND the actor's +0x1000
    if (!puse || !pa || !FIsLoDerivedFrom(puse, 0x23)) return false;
    return (JtDescOf(puse, JTL_Use).mask & static_cast<uint32_t>(pa->n1000)) != 0;
}
