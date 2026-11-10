// scpt.cpp: the scripts (scpt.h). Ported from the Sly 2 decomp's matching C (src/hand, src/auto, src/splicewrap:
// 0x1DFAD8..0x1E5D80, the wrappers 0x106A80..0x10A438) and the reference's m_1DFB10_p3.cpp (0x1E3890) and
// m_1E1E30_p3.cpp (0x1E1E30).
#include "coop.h"
#include "scpt.h"
#include "worldglobals.h"
#include "transition.h"
#include "rumble.h"
#include "region.h"
#include "reach.h"
#include "joy.h"
#include "skipcut.h"
#include "jtthrow.h"
#include <list>
#include "dance.h"
#include "hudwidget.h"
int IdDialogCur();  // sound.cpp (debug)
#include "jt.h"
#include "animfind.h"
#include "shd.h"
#include "interact.h"
#include "binoc.h"
#include "brx.h"
#include "crv.h"
#include "nav.h"
#include "damage.h"
#include "emitter.h"
#include "screenrt.h"
#include "sound.h"
#include "frontend.h"
#include "gang.h"

#include "activate.h"
#include "actor.h"
#include "actorai.h"
#include "alo.h"
#include "aseg.h"
#include "asega.h"
#include "clock.h"
#include "climb.h"
#include "cls.h"
#include "cm.h"
#include "cplcy.h"
#include "frame.h"
#include "proc.h"
#include "sm.h"
#include "sly.h"
#include "smrun.h"
#include "jump.h"
#include "lo.h"
#include "vecmat.h"
#include "objmethod.h"
#include "po.h"
#include "ref.h"
#include "so.h"
#include "spliceobj.h"
bool FFloatsNear(float g1, float g2, float gEpsilon);  // xfmath.h (0x11D4D0; its SMP clashes with vecmat.h's)

#include "step.h"
#include "stepphys.h"
#include "screenrt.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::unordered_map<int, int> g_mpoidcScptStart;   // (debug, route.cpp waitscpt: each script's starts)


void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "scpt: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}
bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_SCPTPRINT") != nullptr;
    return s_f;
}

constexpr float c_degToRad = 0.017453294f;  // (0x3C8EFA36)

// A command (0x50 bytes: +0 kind, +4 b (1: no waiting), the arguments from +0x10, 16 words).
union SLOT
{
    int32_t n;
    float g;
};
struct SCPTCMD
{
    int kind = 0;
    int b = 0;
    SLOT a[18]{};  // +0x10 + 4 i (a[0] is +0x10)
    int& N(uint32_t ib) { return a[(ib - 0x10) / 4].n; }
    float& G(uint32_t ib) { return a[(ib - 0x10) / 4].g; }
};

struct SCPT;

// A runner (0xF0 bytes; vtable 0x2D8C08: slot 1 / 2 the reference count 0x1E1B58 / 0x1E1B68).
std::unordered_map<const void*, bool>& MpRunLive();
}  // namespace
bool FDropPointOnGround(glm::vec3* ppos, float gHigh, float gLow);  // aijobs.cpp: 0x173610
void TellGuardCommands(LO* pguard, LO* pcmds, bool fRun);  // scptai.cpp: orig 0x171CF8 / 0x171D58
float GSoundPitchOffset(LO* pso);                           // asegevent.cpp: 0x210740's pitch terms
bool FGuardRunsCommands(LO* pguard, LO* pcmds);             // scptai.cpp: orig 0x171DA8
void LaunchActorTo(ACTOR* pa, const glm::vec3& posT, float g, float h);  // pad.cpp: orig 0x141968
void PauseGame(int grf);   // ai.cpp: orig 0x20EFC0
void ResumeGame(int grf);  // ai.cpp: orig 0x20F030
void PauseTalk();          // ai.cpp: orig 0x20F0C0
void ResumeTalk();         // ai.cpp: orig 0x20F0F0
// (native lifetime: a runner's camera move (+0x84) can outlive the runner: a move revoked "at its end" (mode 2) keeps
// the timed policy pointing at it after the runner is retired and freed (on the PS2 the runner's memory stays). A move
// still in use is moved here with its two states, without the owner's callbacks: e.g. Ep2's elephant rampage (2078
// asked done while its scene's camera moved) crashed in CPTIMED::Update.)
static void OrphanCmTrans(CMTRANS* ptrans)
{
    bool fUsed = g_cmpolicies.cptimed.ptrans == ptrans;
    for (int i = 0; i < g_cm.ccpr; ++i) fUsed = fUsed || g_cm.acpr[i].pv == ptrans;
    if (!fUsed) return;
    struct ORPHAN { CMSTATE a, b; CMTRANS t; };
    // (never destroyed, as MpRunLive: it is first made at a reload that cut a scene's camera move, after MpScpt's
    // table, so at exit it was destroyed first, and a runner still in a scene freed with that table appended to the
    // destroyed list: a write into its freed nodes, "corrupted double-linked list" at exit, PR5 / the Ep7 boss)
    static auto* s_plorphan = new std::list<ORPHAN>;
    ORPHAN& o = s_plorphan->emplace_back();
    if (ptrans->pstateA) o.a = *ptrans->pstateA;
    if (ptrans->pstateB) o.b = *ptrans->pstateB;
    o.t = *ptrans;
    o.t.pstateA = &o.a;
    o.t.pstateB = &o.b;
    o.t.pfnStarted = o.t.pfnEnded = nullptr;
    o.t.pvOwner = nullptr;
    if (g_cmpolicies.cptimed.ptrans == ptrans) g_cmpolicies.cptimed.ptrans = &o.t;
    for (int i = 0; i < g_cm.ccpr; ++i)
        if (g_cm.acpr[i].pv == ptrans) g_cm.acpr[i].pv = &o.t;
}

namespace
{
struct RUN : SMALISTENER
{
    RUN() { MpRunLive()[this] = true; }
    ~RUN() override { ReleaseSmaWatch(); MpRunLive().erase(this); OrphanCmTrans(&trans); }
    RUN(const RUN&) = delete;
    RUN& operator=(const RUN&) = delete;
    void OnSmaState(const SMAMSG10& msg) override;
    void ReleaseSmaWatch();
    LO* pscptLo = nullptr;   // +0x14 the script
    LO* ploCtx = nullptr;    // +0x18 where its ids are looked up from (its parent's, else the script)
    LO* plo1C = nullptr;     // +0x1C the block's object (the opening command's +0x10)
    LO* plo20 = nullptr;     // +0x20 (command 8)
    LO* plo24 = nullptr;     // +0x24 the current command's object
    int icFirst = 0;         // +0x28 the opening command
    int icLast = 0;          // +0x2C its closing command
    int icCur = -1;          // +0x30 (-1: not started)
    float t34 = 0.0f;        // +0x34 when the current command started
    int fPause = 0;          // +0x38 (command 0x12: the game paused by the runner)
    int grfPause = 0;        // +0x3C its pause kinds
    int icPending = -1;      // +0x40 the command the actor works on
    LO* ploWatch = nullptr;  // +0x44 that actor
    int fBlocked = 0;        // +0x48 (commands 0x50 / 0x51: the actor's drive holds TRIANGLE, 0x1E46A0)
    LO* ploGuardD8 = nullptr, *ploCmdsDC = nullptr;  // +0xD8 / +0xDC (command 0x55 without its wait)
    ASEGA* pasega = nullptr; // +0x4C (command 0xA)
    LO* pscptSub = nullptr;  // +0x50 (command 0xB)
    LO* ploAnim = nullptr;   // +0x54 the actor animated by command 0xA
    LO* ploScreen = nullptr; // +0x64 the screen object shown by command 0xE
    CSndRef psnd;            // (the relay's sound of command 0x36, a looping one kept: pjsg->psnd)
    SCPTCMD* pcDialog = nullptr; // +0x68 the last dialog command (0x45), for 0x52
    LO* ploFade = nullptr;   // +0x60 the object faded in / out by command 0x2C / 0x2D, waited on
    float s7C = 0.0f;        // +0x7C (command 0x1A: the distance on the climbed pipe to reach)
    SMA* psma = nullptr;     // +0x58 (command 0xD)
    int fSmaGoal = 0;        // +0x5C (its goal reached: the SMA's message 10, 0x1E1BC8)
    float f80 = 0.0f;        // +0x80 when the camera block ends (FLT_MAX: not timed)
    CMSTATE acmstate[2];     // (the block's two camera states: the original points at its commands' +0x10)
    CMTRANS trans;           // +0x84..+0x98 the move's argument (0x1E57A0)
    int f9C = 0;             // +0x9C (the move pushed)
    int fE0 = 0;             // +0xE0 (commands 0x25 / 0x26)
    ACTOR* paBinoc = nullptr;  // +0xA0 the actor whose binocucom command 0x3B opened
    struct HOLD { LO* plo; int grf; int f; };
    std::vector<HOLD> ahold;   // +0xA4 / +0xA8 / +0xB8 / +0xC8 (at most 4: command 0x47's held effects)
    bool fRetired = false;
};
using PRUN = std::shared_ptr<RUN>;
std::unordered_map<const void*, bool>& MpRunLive()
{
    // (the runners alive: an actor's +0x1084 is checked against it; never destroyed: runners kept by other statics
    // are released after it at exit)
    static auto* s_pmp = new std::unordered_map<const void*, bool>;
    return *s_pmp;
}

struct SCPT
{
    std::vector<SCPTCMD> acmd;   // +0x50 / +0x54
    int cMax = -1;           // +0x58
    std::vector<PRUN> arun;  // +0x5C its runners
    LO* prun68Lo = nullptr;  // (unused)
    RUN* prun68 = nullptr;   // +0x68 the runner whose event 4 runs (command 9)
    LO* ploStarter = nullptr;  // +0x6C
    float t = 0.0f;          // +0x70
    int fRunning = 0;        // +0x74
    int f78 = 0;             // +0x78 (it set its update)
    int fDirty = 1;          // +0x7C (the closing command not yet added)
    int f80 = 0;             // +0x80
    std::vector<int> analarm;  // +0x84 / +0x88 (at most 8)
    bool fRealTime = false;  // (the flags' 0x100: the real clock, command 0x10)
    uint64_t idSkipScene = 0;  // native QoL: this particular start of a skippable scene (skipcut.h)
    int nSkipKind = -1;        // native QoL: SceneSkipKind's class, computed at the first start
};

std::unordered_map<LO*, SCPT>& MpScpt()
{
    static std::unordered_map<LO*, SCPT> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}
SCPT* PscptOf(LO* plo)
{
    if (!plo) return nullptr;
    auto it = MpScpt().find(plo);
    return it == MpScpt().end() ? nullptr : &it->second;
}

// orig: 0x001E1BC8: latch the commanded state even when the SMA immediately leaves it again.
void RUN::OnSmaState(const SMAMSG10& msg)
{
    SCPT* ps = PscptOf(pscptLo);
    if (msg.psma == psma && ps && icCur >= 0 && icCur < static_cast<int>(ps->acmd.size()) &&
        msg.oidNew == ps->acmd[static_cast<size_t>(icCur)].N(0x18))
    {
        fSmaGoal = 1;
        if (FPrint()) std::fprintf(stderr, "scpt %d: sma goal %d reached t %.3f\n", pscptLo->oid, msg.oidNew, g_clock.t);
    }
}

// orig: 0x001E5410: remove the runner from the SMA's listeners, release its reference, clear the latch.
void RUN::ReleaseSmaWatch()
{
    if (!psma) return;
    auto& a = psma->aplistener;
    a.erase(std::remove(a.begin(), a.end(), this), a.end());  // 0x133448
    --psma->cref;
    psma = nullptr;
    fSmaGoal = 0;
}

// the open iterators of the runner lists (0x2DD2FC): a removal before an iterator's next moves it back
struct DLI
{
    SCPT* pscpt;
    size_t i;
};
std::vector<DLI*>& Adli()
{
    static std::vector<DLI*> s_a;
    return s_a;
}

bool IsA(const LO* plo, CID cid) { return plo && FCidDerivedFrom(plo->Cid(), cid); }

float TScpt(const SCPT* ps) { return ps && ps->fRealTime ? g_clock.t3C : g_clock.t; }

// orig: 0x001E01F8 (a command appended; past the capacity a scratch one, 0x3DA740)
SCPTCMD* PcmdNew(SCPT* ps, int kind, int b)
{
    static SCPTCMD s_cmdScratch;
    const int cMax = ps->cMax <= -1 ? 0x200 : ps->cMax;
    if (static_cast<int>(ps->acmd.size()) >= cMax) return &s_cmdScratch;
    ps->acmd.emplace_back();
    SCPTCMD* p = &ps->acmd.back();
    p->kind = kind;
    p->b = b;
    return p;
}

void StartScptImpl(LO* pscpt, LO* ploStarter);
void StopScptImpl(LO* pscpt);
void RunRun(const PRUN& prun);
void RetireRun(const PRUN& prun);
void EndActorScriptAnim(ACTOR* pa);
bool FCmdDone(RUN* p, SCPTCMD* pc);
// // (the AI's D_003DD020: the nearest point in the horizontal plane)
void EndRunCamera(RUN* p, int fCut);

// orig: 0x001E1DB8 (the object id seen from the runner's context; a proxy gives its latest instance's object)
LO* PloFind(RUN* p, int id)
{
    if (id == -1 || !g_splicegame.pfnFindObject) return nullptr;
    LO* q = dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, static_cast<SYMID>(id), p->ploCtx, nullptr));
    if (q && IsA(q, 6))
    {
        if (PFNOBJGETTER pfn = PfnObjectGetter(1176)) q = dynamic_cast<LO*>(pfn(q).m_pbasic);  // 0x1CB2E0
        else q = nullptr;
    }
    return q;
}
LO* PloFindKind(int kind, int id)
{
    if (id == -1 || !g_splicegame.pfnFindObject) return nullptr;
    return dynamic_cast<LO*>(g_splicegame.pfnFindObject(kind, static_cast<SYMID>(id), nullptr, nullptr));
}

// orig: 0x210408 / 0x2104A0 / 0x210538: event lookups use the context
// directly, without the runner lookup's extra proxy-instance conversion.
LO* PloFindRunEvent(RUN* p, int id)
{
    if (id == -1 || !g_splicegame.pfnFindObject) return nullptr;
    return dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, static_cast<SYMID>(id), p->ploCtx, nullptr));
}

// orig: 0x001E0180 (the first runner other than pSkip whose block's opening command has id n)
PRUN PrunFind(SCPT* ps, int n, RUN* pSkip)
{
    for (const PRUN& r : ps->arun)
        if (r.get() != pSkip && ps->acmd[static_cast<size_t>(r->icFirst)].N(0x10) == n) return r;
    return nullptr;
}

// orig: 0x001E0020 / 0x001E00E8 / 0x001E0070 (the script's alarms)
static bool FAlarmPrint()
{
    static const bool s_f = std::getenv("SLY2N_ALARMPRINT") != nullptr;  // (debug: the scripts' alarms set / removed)
    return s_f;
}
void EnsureAlarm(SCPT* ps, int n)
{
    if (FAlarmPrint()) std::fprintf(stderr, "alarm %d: ensure (%zu set) t %.3f\n", n, ps->analarm.size(), g_clock.t);
    for (int a : ps->analarm)
        if (a == n) return;
    if (ps->analarm.size() < 8) ps->analarm.push_back(n);
}
bool FHasAlarm(const SCPT* ps, int n)
{
    for (int a : ps->analarm)
        if (a == n) return true;
    return false;
}
void RemoveAlarm(SCPT* ps, int n)
{
    if (FAlarmPrint()) std::fprintf(stderr, "alarm %d: remove t %.3f\n", n, g_clock.t);
    for (size_t i = 0; i < ps->analarm.size(); ++i)
        if (ps->analarm[i] == n)
        {
            ps->analarm.erase(ps->analarm.begin() + static_cast<long>(i));
            return;
        }
}

// orig: 0x001E4EB8 (the pending command and the actor working on it: 0x143DE8 its link back, +0x1084)
void SetRunPending(RUN* p, int ic, LO* plo)
{
    p->icPending = ic;
    if (plo == p->ploWatch) return;
    if (ACTOR* pa = PactorFromLo(p->ploWatch); pa && pa->prunScript == p)
    {
        // orig: 0x143DE8 releases scripted reaches before changing the runner.
        // A short switch script can retire while its parallel reach is active.
        if (pa->n4C8 == 5 || pa->n4C8 == 6) NotifyActorJumpKind(pa, 0);
        pa->prunScript = nullptr;
    }
    p->ploWatch = plo;
    if (ACTOR* pa = PactorFromLo(plo))
    {
        pa->prunScript = p;
        ClearJobs(pa);  // 0x143DE8: a script taking control releases the actor's AI stack
    }
}

// orig: 0x001E51A0 (command 0x47: a PO's effects held (vt slot 83), at most 4 per runner, each object once)
void HoldRunEffects(RUN* p, LO* plo, int grf, int f)
{
    if (!plo || !IsA(plo, 0x33)) return;
    for (const RUN::HOLD& h : p->ahold)
        if (h.plo == plo) return;
    if (p->ahold.size() >= 4) return;
    p->ahold.push_back({plo, grf, f});
    HoldActorEffects(plo, true, f != 0, static_cast<uint32_t>(grf));
}
// orig: 0x001E5280 (command 0x48 / the runner's end: released (vt slot 84))
void ReleaseRunEffects(RUN* p, LO* plo, int grf, int f)
{
    if (!plo || !IsA(plo, 0x33)) return;
    for (size_t i = 0; i < p->ahold.size(); ++i)
        if (p->ahold[i].plo == plo)
        {
            p->ahold.erase(p->ahold.begin() + static_cast<long>(i));
            HoldActorEffects(plo, false, f != 0, static_cast<uint32_t>(grf));
            return;
        }
}

// orig: 0x001E5AE0 (a new runner of the block opened at ic; its parent's context)
PRUN PrunNew(LO* pscptLo, int ic, RUN* pParent)
{
    SCPT* ps = PscptOf(pscptLo);
    auto p = std::make_shared<RUN>();
    p->pscptLo = pscptLo;
    p->ploCtx = pParent ? pParent->ploCtx : pscptLo;
    const int id = ps->acmd[static_cast<size_t>(ic)].N(0x10);
    if (id != -1) p->plo1C = PloFindKind(5, id);
    p->icFirst = ic;
    const int icLast = static_cast<int>(ps->acmd.size()) - 1;
    int depth = 1;
    p->icLast = ic;
    for (int i = ic + 1; i <= icLast; ++i)
    {
        const int k = ps->acmd[static_cast<size_t>(i)].kind;
        if (k == 0) ++depth;
        else if (k == 1 && --depth == 0)
        {
            p->icLast = i;
            break;
        }
    }
    // 0x11A250 puts the runner at the list's head: the newest runner runs first in the next pass (e.g. Ep7 Canada
    // Games' SCPT 3208: its prompt runner (block 33585) resumes in the frame the swing runner (block 33589) sets
    // alarm 33584, after Murray's update, so Murray never stands a frame between the clips; appended, it resumed a
    // frame later and the stick walked him off his spot after a failed round). An open iterator (0x2DD2FC) keeps
    // its next object: one at the list's start (nothing visited yet) sees the new head, the others move on with it.
    ps->arun.insert(ps->arun.begin(), p);
    for (DLI* pdli : Adli())
        if (pdli->pscpt == ps && pdli->i > 0) ++pdli->i;
    return p;
}

// orig: 0x001E5778
bool FRunRetired(const RUN* p) { return p->fRetired; }

// orig: 0x001E5628 (retired: out of its script's list, what it holds released)
void RetireRun(const PRUN& prun)
{
    RUN* p = prun.get();
    if (FRunRetired(p)) return;
    SCPT* ps = PscptOf(p->pscptLo);
    if (ps)
        for (size_t i = 0; i < ps->arun.size(); ++i)
            if (ps->arun[i].get() == p)
            {
                ps->arun.erase(ps->arun.begin() + static_cast<long>(i));
                for (DLI* pdli : Adli())
                    if (pdli->pscpt == ps && i < pdli->i) --pdli->i;
                break;
            }
    p->fRetired = true;
    // 0x1E5370: the animation (+0x4C), and the actor's script animation of a command 0xA (+0x54) ended (0x143FF8:
    // e.g. SCPT 1674's runner 152631 retired by command 55 lets Bentley out of clip 7627 for his walk)
    if (p->pasega)
    {
        RetractAsega(p->pasega);  // 0x130B18
        p->pasega = nullptr;
    }
    if (p->ploAnim)
    {
        if (ACTOR* pa = PactorFromLo(p->ploAnim)) EndActorScriptAnim(pa);  // 0x143FF8
        p->ploAnim = nullptr;
    }
    // 0x1E53D8: the sub-script
    if (p->pscptSub)
    {
        StopScptImpl(p->pscptSub);
        p->pscptSub = nullptr;
    }
    p->ReleaseSmaWatch();
    if (p->ploScreen)  // 0x1E5468: the screen object of a command 0xE (vt slot 50)
    {
        HideScreenObj(p->ploScreen);
        p->ploScreen = nullptr;
    }
    // 0x1E54B0: the binocucom a command 0x3B opened (+0xA0) closed (0x19CCE8), as command 0x3C does. Scripts
    // that open one and end without 0x3C rely on it: Close to Contessa's opening talk SCPT 2518 (R01: the binocucom
    // stayed up after t4_pickpocket_intro was done, a softlock)
    if (p->paBinoc)
    {
        CloseBinoc(p->paBinoc);
        p->paBinoc = nullptr;
    }
    // 0x1E54E8: the held effects released, the last first
    while (!p->ahold.empty())
    {
        const RUN::HOLD h = p->ahold.back();
        ReleaseRunEffects(p, h.plo, h.grf, h.f);
    }
    // orig: 0x001E5570: a retired path/walk command releases the actor's AI stack.
    if (ps && p->icFirst < p->icCur && p->icCur < p->icLast)
    {
        const int kind = ps->acmd[static_cast<size_t>(p->icCur)].kind;
        if (kind == 0x27 || kind == 0x28)
            if (ACTOR* pa = PactorFromLo(p->plo24)) ClearJobs(pa);
    }
    // TODO(actors): 0x1E4F20 (the controlled object), 0x1E5000 (the look target), 0x1E50E0
    // (the music), +0xEC's sound, 0x1E5A98 (the camera's +0x9C)
    SetClockFactor(&g_clock, 6, 1.0f);  // 0x14D1D0
    SetRunPending(p, -1, nullptr);
    // 0x1E50E0(p, 0, 0): the runner's pause of the game ended (command 0x12's; e.g. the safe house's exit 27254 [1 3]:
    // its letterbox and the HUD back when the script stops)
    if (FPrint()) std::fprintf(stderr, "scpt %d: run %p retired (pause %d)\n", p->pscptLo ? p->pscptLo->oid : -1, static_cast<void*>(p), p->fPause);
    if (p->fPause)
    {
        ResumeGame(p->grfPause);  // 0x20F030
        ResumeTalk();             // 0x20F0F0
        p->fPause = 0;
        p->grfPause = 0;
    }
    if (p->f9C) EndRunCamera(p, 1);
}

// orig: 0x001E1D48 (the pending command finished: the actor let go)
void FinishRunPending(RUN* p, SCPT* ps)
{
    if (p->icPending < 0) return;
    SCPTCMD* pc = &ps->acmd[static_cast<size_t>(p->icPending)];
    if (!FCmdDone(p, pc)) return;
    bool f = true;
    if (pc->kind == 0x1D) f = pc->N(0x10) == -1;
    if (f) SetRunPending(p, -1, nullptr);
}

void ExecuteRun(const PRUN& prun);

// orig: 0x001E5A98 (the camera move revoked; fCut: cut at its end)
void EndRunCamera(RUN* p, int fCut)
{
    p->trans.fCut = fCut;
    p->f9C = 0;
    RevokeCmPolicy(&g_cm, 2, 10, &g_cmpolicies.cptimed, nullptr, &p->trans);  // 0x14E4C8
}

// orig: 0x001E57A0 (the camera's timed move from the block's opening command pa (0x38) to its end pb (0x39) over
// dt: a state left at zero is the camera's current one)
void StartRunCamera(RUN* p, SCPTCMD* pa, SCPTCMD* pb, float dt)
{
    if (pa->G(0x40) == 0.0f) p->f80 = FLT_MAX;  // (0x2A8124)
    else
    {
        pb->b = 0;
        p->f80 = g_clock.t + dt;
    }
    SCPTCMD* apc[2] = {pa, pb};
    for (int i = 0; i < 2; ++i)
    {
        CMSTATE& st = p->acmstate[i];
        st.pos = glm::vec3(apc[i]->G(0x10), apc[i]->G(0x14), apc[i]->G(0x18));
        st.eul = glm::vec3(apc[i]->G(0x20), apc[i]->G(0x24), apc[i]->G(0x28));
        st.radFOV = apc[i]->G(0x30);
    }
    // B's angles taken the short way from A's
    for (int k = 0; k < 3; ++k)
        p->acmstate[1].eul[k] = p->acmstate[0].eul[k] + cmm::RadNormalize(p->acmstate[1].eul[k] - p->acmstate[0].eul[k]);
    auto fNear0 = [](const glm::vec3& v) {
        return std::fabs(v.x) < 1e-4f && std::fabs(v.y) < 1e-4f && std::fabs(v.z) < 1e-4f;  // 0x11D4D0
    };
    for (CMSTATE& st : p->acmstate)
    {
        if (fNear0(st.pos)) st.pos = g_cm.pos;
        if (fNear0(st.eul)) st.eul = cmm::DecomposeEulerMatrix(g_cm.mat);  // 0x11BFF0
        if (st.radFOV == 0.0f) st.radFOV = g_cm.radFOV;
    }
    p->trans.pstateA = &p->acmstate[0];
    p->trans.pstateB = &p->acmstate[1];
    p->trans.dt = dt == 0.0f ? FLT_MAX : dt;
    p->trans.fCut = 1;
    p->trans.icurve = pa->N(0x44);
    p->trans.pvOwner = p;
    p->f9C = 1;
    if (FActiveCplcy(&g_cmpolicies.cptimed)) RevokeCmPolicy(&g_cm, 2, 10, &g_cmpolicies.cptimed, nullptr, nullptr);
    SetCmPolicy(&g_cm, 10, &g_cmpolicies.cptimed, nullptr, &p->trans);  // 0x14E328
}

// orig: 0x001E4530 (a command's own duration, for a camera block without one)
float GCmdDuration(RUN* p, SCPTCMD* pc)
{
    if (pc->b) return 0.0f;
    switch (pc->kind)
    {
    case 0xA:
        WarnOnce("0x1E4530's ASEG duration");
        return 0.0f;
    case 0x32: return 0.5f;
    case 0x23: case 0x33: return pc->G(0x10);
    case 0x2C: case 0x2D: case 0x53: return pc->G(0x14);
    default: (void)p; return 0.0f;
    }
}

// orig: 0x001E1C40 (runs a runner: its commands until one waits; retired past its last)
void RunRun(const PRUN& prun)
{
    PRUN lock = prun;  // (LOCK: vt slots 1 / 2)
    RUN* p = prun.get();
    SCPT* ps = PscptOf(p->pscptLo);
    if (!ps) return;
    if (p->icCur < 0)
    {
        p->icCur = p->icFirst;
        ExecuteRun(prun);
        if (FRunRetired(p)) return;
    }
    // (natively a runner whose commands never wait - a loop of jumps over commands not ported - is held for the
    // frame after 100,000 commands, with a note; the original waits in one of them)
    for (long c = 0; p->icCur <= p->icLast; ++c)
    {
        SCPTCMD* pc = &ps->acmd[static_cast<size_t>(p->icCur)];
        if (c == 100000)
        {
            static int s_cNote = 0;
            if (s_cNote++ < 4)
                std::fprintf(stderr, "scpt %d: a runner ran 100000 commands without waiting (at command %d kind 0x%X): held\n",
                             p->pscptLo ? p->pscptLo->oid : -1, p->icCur, pc->kind);
            break;
        }
        if (!(pc->b || FCmdDone(p, pc))) break;
        FinishRunPending(p, ps);
        ExecuteRun(prun);
        if (FRunRetired(p)) return;
    }
    FinishRunPending(p, ps);
    if (p->icCur > p->icLast) RetireRun(prun);
}

// orig: 0x001E4D40 (the position and heading a command aims at: an object's (a curve's nearest point: TODO), else
// the runner's object's base)
void GetRunAim(RUN* p, LO* po, glm::vec3* ppos, float* prad)
{
    LO* px = p->plo24 ? p->plo24 : p->ploWatch;
    glm::vec3 pos(0.0f);
    float rad = 0.0f;
    auto matOf = [](LO* plo) {
        // vt slot 31: an ALO's object record, a LOX's matrix in its record's frame
        if (IsA(plo, 0x30)) return MatJtLo(plo);
        const LOX* plox = static_cast<const LOX*>(plo);
        return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    };
    auto headingOf = [&](LO* plo) {
        const glm::mat4 m = matOf(plo);
        return std::atan2(m[0][1], m[0][0]);  // (its x row)
    };
    if (po)
    {
        if (IsA(po, 0xA))
        {
            // a curve: its nearest point to the runner's object (0x1E6F60 over the whole curve), heading along it
            const CRV* pcrv = PcrvFromLo(po);
            if (pcrv && px)
            {
                const glm::vec3 q = IsA(px, 0x30) ? PosAloCenter(static_cast<ALO*>(px)) : glm::vec3(matOf(px)[3]);  // 0x1A62E8
                glm::vec3 dir(1.0f, 0.0f, 0.0f);
                NearestCrvAll(pcrv, q, &pos, &dir, nullptr, nullptr);
                rad = std::atan2(dir.y, dir.x);
            }
        }
        else if (IsA(po, 0x30))
        {
            pos = PosAloCenter(static_cast<ALO*>(po));
            rad = headingOf(po);
        }
        else if (IsA(po, 3))
        {
            pos = glm::vec3(matOf(po)[3]);
            rad = headingOf(po);
        }
    }
    else if (px && IsA(px, 0x30))
    {
        pos = PosAloCenter(static_cast<ALO*>(px));
        if (STEP* pstep = PstepFromLo(px))
        {
            pos += glm::vec3(pstep->vecBase);
            rad = pstep->radHeading;
        }
        else rad = headingOf(px);
    }
    if (ppos) *ppos = pos;
    if (prad) *prad = rad;
}

// orig: 0x00144038 (the actor's state is a script's animation: slot 89's kind 5 or 6)
bool FActorInScriptAnim(ACTOR* pa) { return static_cast<unsigned>(ActorJumpKind(pa) - 5) < 2; }

// orig: 0x00143FF8 (the script's animation over: slot 88 (0))
void EndActorScriptAnim(ACTOR* pa)
{
    if (FActorInScriptAnim(pa)) NotifyActorJumpKind(pa, 0);
}

// orig: 0x00143EE0 (an actor animated by a script: its slot 88 kind 5 (Sly: state 0x13, Bentley 10, Murray 17), then
// the ASEG played on its +0x1030 or the SM run on its +0x1034; oid -1: 0x143FF8). Sly's state 0x13 itself (sly.cpp
// enters it with a warning) ends here when the animation is over (FActorScriptAnim).
void PlayActorScriptAnim(LO* ploActor, LO* plo, int oid, int grfapl, int kind)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa)
    {
        WarnOnce("command 0xA on an object that is no actor (0x143EE0)");
        return;
    }
    // (debug, until the gang's script clips run: SLY2N_SCRIPTANIM_SLYONLY skips them for actors other than Sly)
    static const bool s_fSlyOnly = std::getenv("SLY2N_SCRIPTANIM_SLYONLY") != nullptr;
    if (s_fSlyOnly && !dynamic_cast<SLY*>(pa)) return;
    if (oid == -1)
    {
        EndActorScriptAnim(pa);
        return;
    }
    if (!plo) return;
    if (IsA(plo, 0x6D))
    {
        NotifyActorJumpKind(pa, kind);  // vt slot 88 (5; 6 for the scripted walks: steered by the drive)
        ClearSmaHandle(&pa->psma);   // 0x1F15B8
        ApplyAsegCur(static_cast<ASEG*>(plo), pa->palo, 0.0f, 1.0f, grfapl, &pa->pasega);  // 0x1305B8
    }
    else if (IsA(plo, 0x6F))
    {
        NotifyActorJumpKind(pa, kind);
        RetractAsegaPp(&pa->pasega);  // 0x130658
        ReplaceSmaHandle(static_cast<SM*>(plo), pa->palo, -1, grfapl, &pa->psma);  // 0x1F1448
    }
}
// orig: 0x00144038 (as the commands wait on it: in kind 5 / 6; natively the animation's end also ends Sly's state
// 0x13, whose state code is not ported)
bool FActorScriptAnim(LO* ploActor)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa || !FActorInScriptAnim(pa)) return false;
    if (!dynamic_cast<SLY*>(pa)) return true;  // (Bentley 10 / Murray 17: their transitions end it, 0x134850 / 0x1B4AA0)
    const bool fOver = pa->psma ? FSmaRetracted(pa->psma) : (!pa->pasega || FAsegaRetracted(pa->pasega));
    if (std::getenv("SLY2N_ACTANIMDBG"))
        std::printf("actor anim: sma %p asega %p svt %.3f aseg %d grf 0x%X t %.3f retracted %d\n", (void*)pa->psma, (void*)pa->pasega, pa->pasega ? pa->pasega->svt : 0.0f, pa->pasega && pa->pasega->paseg ? pa->pasega->paseg->oid : -1, pa->pasega && pa->pasega->paseg ? pa->pasega->paseg->grfAseg : 0,
                    pa->pasega ? pa->pasega->t : -1.0f, pa->pasega ? FAsegaRetracted(pa->pasega) : -1);
    if (!fOver) return true;
    NotifyActorJumpKind(pa, 0);  // (see above)
    return false;
}

// orig: 0x001E1E30 (executes the runner's next command)
void ExecuteRun(const PRUN& prun)
{
    RUN* p = prun.get();
    SCPT* ps = PscptOf(p->pscptLo);
    const int ic = ++p->icCur;
    if (p->icLast < ic) return;
    PRUN lock = prun;
    SCPTCMD* pc = &ps->acmd[static_cast<size_t>(ic)];
    const int kind = pc->kind;
    int id = -1;
    switch (kind)
    {
    case 0x16: case 0x1A: case 0x1C: case 0x1E: case 0x23: id = pc->N(0x18); break;
    case 0x27: case 0x28: id = pc->N(0x20); break;
    case 0x22: case 0x24: case 0x29: case 0x2A: case 0x2B: case 0x4A: id = pc->N(0x1C); break;
    case 0x1B: case 0x47: case 0x48: case 0x4F: case 0x50: case 0x51: case 0x57: id = pc->N(0x10); break;
    case 0x0B: case 0x18: case 0x19: case 0x1D: case 0x1F: case 0x20: case 0x21: case 0x43: case 0x4B: case 0x58:
    case 0x59: case 0x5A: id = pc->N(0x14); break;
    default: break;
    }
    LO* s0;
    if (id != -1) s0 = PloFind(p, id);
    else
    {
        s0 = p->plo20;
        if (!s0) s0 = p->plo1C;
        if (!s0) s0 = ps->ploStarter;
    }
    p->plo24 = s0;
    if (FPrint())
        std::fprintf(stderr, "scpt %d: run %p command %d kind 0x%X b %d [%d %d %d %d] object %d (t %.3f)\n", p->pscptLo->oid,
                    static_cast<void*>(p), ic, kind, pc->b, pc->N(0x10), pc->N(0x14), pc->N(0x18), pc->N(0x1C),
                    s0 ? s0->oid : -1, g_clock.t);
    auto actorOf = [&](CID cid) -> LO* { return IsA(p->plo24, cid) ? p->plo24 : nullptr; };
    static const char* s_pchzSkip = std::getenv("SLY2N_SCPTSKIP");  // (debug: "16,2C" kinds not executed)
    bool fSkip = false;
    if (s_pchzSkip)
    {
        char sz[8];
        std::snprintf(sz, sizeof sz, "%X", kind);
        for (const char* q = s_pchzSkip; *q;)
        {
            const char* e = q;
            while (*e && *e != ',') ++e;
            if (std::string(q, e) == sz) fSkip = true;
            q = *e ? e + 1 : e;
        }
    }
    switch (fSkip ? -1 : kind)
    {
    case -1: break;
    case 0x00:
    {
        PRUN r = PrunNew(p->pscptLo, ic, p);
        p->icCur = r->icLast;
        RunRun(r);
        break;
    }
    case 0x01: case 0x13: break;
    case 0x02:
    {
        const int n = pc->N(0x10);
        PRUN r = n != -1 ? PrunFind(ps, n, nullptr) : prun;
        if (r) RetireRun(r);
        break;
    }
    case 0x05:
    case 0x06:
    {
        const int n = kind == 5 ? pc->N(0x14) : pc->N(0x10);
        LO* plo = n == -1 ? p->pscptLo : PloFind(p, n);
        if (plo && !IsA(plo, 0x72)) plo = nullptr;
        if (!plo) break;
        if (kind == 5)
        {
            if (SCPT* psOther = PscptOf(plo)) EnsureAlarm(psOther, pc->N(0x10));
        }
        else StopScptImpl(plo);
        break;
    }
    case 0x07:
    {
        LO* po = PloFind(p, pc->N(0x10));
        if (!po || !IsA(po, 0x32)) break;
        if (auto pfn = PfnVt<void (*)(LO*, int)>(po, 79)) pfn(po, 1);
        break;
    }
    case 0x08: p->plo20 = PloFind(p, pc->N(0x10)); break;
    case 0x09:
    {
        // The script's event 4 with the command's argument. EOPID 624 can
        // change this runner's object lookup context while the event runs.
        ps->prun68 = p;
        if (FPrint())
            if (CRef* pref = p->pscptLo->pframe ? p->pscptLo->pframe->PrefFindBinding(4, 1) : nullptr; pref && pref->m_tagk == TAGK_Proc)
            {
                std::string str;
                CRef body;
                body.SetPair(pref->m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
                std::fprintf(stderr, "  event 4: %s\n", str.substr(0, 4000).c_str());
            }
        CRef ref;
        ref.SetS32(pc->N(0x10));
        HandleLoSpliceEvent(p->pscptLo, 4, false, 1, &ref);
        ps->prun68 = nullptr;
        break;
    }
    case 0x0A:
    {
        LO* s1 = PloFind(p, pc->N(0x14));
        if (s1)
        {
            if (IsA(s1, 0x34))
            {
                // 0x143EE0 (kind 5) finds the clip below the actor (0x15E938 kind 1), not around the script: Rajan's
                // ASEG 423 was found for Sly's knocked-down clip 423, so Sly stood in the bind pose on the lily pad
                PlayActorScriptAnim(s1, PloFindSwChild(s1, pc->N(0x10)), pc->N(0x10), pc->N(0x18) | 1, 5);
                if (pc->b == 0) p->ploAnim = s1;
                break;
            }
            if (!IsA(s1, 0x30)) s1 = nullptr;
        }
        LO* po = PloFind(p, pc->N(0x10));
        if (!po || !IsA(po, 0x6D)) break;
        ASEGA* pa = PasegaApplyAseg(static_cast<ASEG*>(po), static_cast<ALO*>(s1), 0.0f, 1.0f, pc->N(0x18));  // 0x130488
        if (pc->b != 0) break;
        p->pasega = pa;
        break;
    }
    case 0x0E:
    case 0x0F:
    {
        // a screen object (cid 0x7E) shown on part +0x14 (vt slot 49; 0xE without b: waited for until gone) / hidden
        // (vt slot 50)
        LO* plo = PloFind(p, pc->N(0x10));
        if (FPrint()) std::printf("scpt: screen %d found %d cid 0x%X\n", pc->N(0x10), plo ? 1 : 0, plo ? plo->Cid() : 0);
        if (!plo || !IsA(plo, 0x7E)) break;
        if (kind == 0x0E)
        {
            ShowScreenObj(plo, pc->N(0x14));
            if (FPrint()) std::printf("scpt: screen %d shown: state %d (real t %.3f)\n", plo->oid, NScreenObjState(plo), g_clock.t3C);
            if (pc->b == 0) p->ploScreen = plo;
        }
        else HideScreenObj(plo);
        break;
    }
    case 0x0B:
    {
        LO* plo = PloFind(p, pc->N(0x10));
        if (!plo || !IsA(plo, 0x72)) break;
        StartScptImpl(plo, s0);
        if (pc->b == 0) p->pscptSub = plo;
        break;
    }
    case 0x0C:
    {
        LO* po = PloFind(p, pc->N(0x10));
        p->ploCtx = po ? po : p->pscptLo;
        break;
    }
    case 0x0D:
    {
        // 0x20FFF0: the state machine +0x14 on the object +0x10 to the state +0x18 (+0x1C: at once), watched
        p->ReleaseSmaWatch();
        LO* o = PloFind(p, pc->N(0x10));
        if (!o)
        {
            // (0x15E650's kind 0x200: an instance's alias too, 0x1A6290: natively LO::oidInstance)
            for (const auto& q : g_psw->aploAll)
                if (q->oidInstance == pc->N(0x10) && IsA(q.get(), 0x30))
                {
                    o = q.get();
                    break;
                }
        }
        if (FPrint() && !(o && IsA(o, 0x30))) std::fprintf(stderr, "scpt: 0xD object %d not found\n", pc->N(0x10));
        if (o && IsA(o, 0x30))
        {
            SMA* y = nullptr;  // 0x1F67E0
            for (SMA* q : g_apsma)
                if (!q->fRetracted && q->palo == o && (pc->N(0x14) == -1 || (q->psm && q->psm->oid == pc->N(0x14))))
                {
                    y = q;
                    break;
                }
            const OID oid = pc->N(0x18);
            if (FPrint())
                std::fprintf(stderr, "scpt: 0xD object %d (cid 0x%X) sma %s cur %d goal %d\n", o->oid, o->Cid(), y ? "found" : "none",
                             y ? OidSmaCur(y) : -1, oid);
            if (y && oid != -1)
            {
                p->psma = y;
                ++y->cref;
                y->aplistener.push_back(p);  // 0x20FFF0 / 0x1333C0: listen before changing the goal
                if (pc->N(0x1C)) SnapSmaGoal(y, oid);  // 0x1F0948
                else SetSmaGoal(y, oid);               // 0x1F0660
            }
        }
        if (pc->b != 0)
        {
            p->ReleaseSmaWatch();
            break;
        }
        if (!p->psma || p->fSmaGoal || FSmaRetracted(p->psma)) break;  // 0x1F0638
        if (OidSmaCur(p->psma) == pc->N(0x18)) p->fSmaGoal = 1;       // 0x1F07E8
        break;
    }
    case 0x1A:
    {
        // the climbing actor (+0xF54 set) to the point of its pipe (+0xF5C) nearest the object +0x10's aim (0x1E6F60)
        ACTOR* pa = PactorFromLo(actorOf(0x34));
        if (!pa || pa->nSubF54 == -1 || !pa->pcrvF50) break;
        glm::vec3 pos;
        GetRunAim(p, PloFind(p, pc->N(0x10)), &pos, nullptr);  // 0x1E4D40
        NearestCrvAll(pa->pcrvF50, pos, nullptr, nullptr, nullptr, &p->s7C, nullptr);
        SetRunPending(p, ic, pa->palo);
        break;
    }
    case 0x29:
    {
        // a player in interaction kind 8 (+0x4C8): waited on while in it, the drive steering it to +0x10
        LO* po = actorOf(0x34);
        if (!po || !IsA(po, 0x34)) break;
        ACTOR* pa = PactorFromLo(po);
        if (!pa || pa->nSub4CC == -1) break;  // (+0x4CC: no interaction state)
        if (pa->n4C8 != 8) break;             // (+0x4C8: another kind)
        SetRunPending(p, ic, pa->palo);
        break;
    }
    case 0x2A:
    {
        // the actor goes for a target (0x19F590): the object +0x10 (a hook / line of its: 0x184670, TODO(actors);
        // else its aim dropped onto the ground, 0x173610), none: where it stands; +0x14 1: the camera's own control
        // (+0x2E8), Sly's +0x18F0 = +0x18; waited on until its jump ends (+0x2DC -1)
        ACTOR* pa = PactorFromLo(actorOf(0x34));
        if (!pa) break;
        LO* s4 = PloFind(p, pc->N(0x10));
        SAFEGROUND::ENTRY e;
        if (!s4) e.pos = PosLoWorld(pa->palo) + glm::vec3(pa->vecBase);
        else
        {
            const JTL jtl = JtlDescFromLo(s4);  // 0x184670
            if (jtl != JTL_Max)
            {
                if (pa->nSubF54 != -1 && pa->ploF5C == s4) break;
                // The recovery history's supported descriptions share this exact parameter representation.
                // Hook / hiding-spot scripted aim stays on its existing fallback until those point methods land.
                if (jtl != JTL_Hook && jtl != JTL_Jump)
                {
                    e.ploOwner = s4;
                    e.jtl = jtl;
                    e.pos.x = GJtDescInit(s4, jtl, pa);
                }
                else
                {
                    WarnOnce("command 0x2A hook / hiding-spot description point (0x184670)");
                    GetRunAim(p, s4, &e.pos, nullptr);
                }
            }
            else
            {
                GetRunAim(p, s4, &e.pos, nullptr);  // 0x1E4D40
                FDropPointOnGround(&e.pos, 100.0f, -150.0f);  // 0x173610
            }
        }
        if (FPrint())
            std::fprintf(stderr, "scpt: 0x2A target %d (cid 0x%X) at %.0f %.0f %.0f from %.0f %.0f %.0f\n", s4 ? s4->oid : -1,
                         s4 ? s4->Cid() : 0, e.pos.x, e.pos.y, e.pos.z, PosAloCenter(pa->palo).x, PosAloCenter(pa->palo).y,
                         PosAloCenter(pa->palo).z);
        RecoverActorTo(pa, e);
        if (pc->N(0x14) == 1) pa->jump.f28 = 1;
        if (SLY* psly = dynamic_cast<SLY*>(pa)) psly->n18F0 = pc->N(0x18);
        SetRunPending(p, ic, pa->palo);
        break;
    }
    case 0x27:
    {
        // orig: 0x001E1E30: FollowPath pushes the original A8 controller; it does
        // not bind the actor to the runner's direct-input commands.
        ACTOR* actor = PactorFromLo(actorOf(0x34));
        if (!actor) break;
        LO* path = PloFind(p, pc->N(0x10));
        if (!IsA(path, 0xA)) break;
        const CRV* curve = PcrvFromLo(path);
        if (!curve) break;
        if (pc->N(0x24) != -1)
            PlayActorScriptAnim(actor->palo, PloFindSwChild(actor->palo, pc->N(0x24)), pc->N(0x24), 9, 6);
        auto* follow = static_cast<JOBACTORFOLLOW*>(PjobPush(actor, 0xA8));
        follow->fNoAvoid = p->fE0;
        follow->fWalk = pc->N(0x18);
        follow->uSpeedActor = pc->G(0x1C);
        follow->Follow(curve, pc->N(0x14) == 0);
        follow->Enter();
        break;
    }
    case 0x28:
    {
        // orig: 0x001E1E30 / 0x00144E68: C1 owns the goal and creates an A8
        // follower with the authored arrival, walk/run and speed settings.
        ACTOR* actor = PactorFromLo(actorOf(0x34));
        if (!actor) break;
        glm::vec3 pos;
        GetRunAim(p, PloFind(p, pc->N(0x10)), &pos, nullptr);
        if (pc->N(0x24) != -1)
            PlayActorScriptAnim(actor->palo, PloFindSwChild(actor->palo, pc->N(0x24)), pc->N(0x24), 9, 6);
        auto* walk = static_cast<JOBACTORWALK*>(PjobPush(actor, 0xC1));
        walk->fRun = p->fE0;
        walk->sArrive = pc->G(0x14);
        walk->fWalk = pc->N(0x18);
        walk->uSpeed = pc->G(0x1C);
        walk->SetGoal(pos);
        walk->Enter();
        break;
    }
    case 0x24:
    {
        // the actor goes to the object +0x10 at the speed +0x14 (+0x24: walking), within +0x18
        LO* s1 = actorOf(0x34);
        if (!s1) break;
        if (pc->N(0x20) != -1)  // 0x143EE0(actor, +0x20, 9, 6): the walk's own animation (the actor's kind 6 state)
            PlayActorScriptAnim(s1, PloFindSwChild(s1, pc->N(0x20)), pc->N(0x20), 9, 6);
        LO* pt = PloFind(p, pc->N(0x10));
        // to an interaction's object: 0x1938C0 on the actor's +0x4C0 (its mask / target arguments unused there: the
        // first tracked target within 250 starts its interaction); natively only a warning (audit 2026-11 P9). (The
        // state-3 projection into +0x3C (sGoTo) has no reader.)
        if (pt && (IsA(pt, 0x22) || IsA(pt, 0xC) || IsA(pt, 0x26) || IsA(pt, 0x58) || IsA(pt, 0xB) || IsA(pt, 0x13)))
            if (ACTOR* pa = PactorFromLo(s1)) FInteractCircle(pa);
        SetRunPending(p, ic, s1);
        break;
    }
    case 0x10:
        SetClockFactor(&g_clock, 6, pc->G(0x10));  // 0x14D1D0
        ps->fRealTime = pc->G(0x10) == 0.0f;
        // (the script's update entry on the real clock: it runs in the paused update)
        if (LO* pl = p->pscptLo)
        {
            uint32_t& grf = pl->iupd >= 0 ? g_psw->aupd[static_cast<size_t>(pl->iupd)].grf : pl->grfUpd;
            grf = ps->fRealTime ? (grf | GRFUPD_RealTime) : (grf & ~GRFUPD_RealTime);
        }
        break;
    case 0x14:
    case 0x15:
    {
        // a jump to the label (0x13) +0x10 at the same depth (0x15: when 0x210B68 (always 0) equals +0x14)
        if (kind == 0x15 && pc->N(0x14) != 0) break;
        int depth = 0, icFound = -1;
        for (int i = p->icFirst + 1; i < p->icLast; ++i)
        {
            const SCPTCMD& e = ps->acmd[static_cast<size_t>(i)];
            if (e.kind == 1) --depth;
            else if (e.kind == 0) ++depth;
            else if (e.kind == 0x13 && depth == 0 && e.a[0].n == pc->N(0x10))
            {
                icFound = i;
                break;
            }
        }
        if (icFound >= 0) p->icCur = icFound;
        break;
    }
    case 0x16:
    {
        // the object placed at another's position and heading
        LO* s1 = PloFind(p, pc->N(0x10));
        if (s1 && IsA(s1, 0x29))
        {
            TriggerWarp(s1);  // 0x228170: the player put at the warp point
            break;
        }
        LO* po = p->plo24;
        if (!po || !IsA(po, 0x30)) break;
        if (s1 && IsA(s1, 0x1B))
        {
            // onto the hiding rail: a player (cid 0x34) enters interaction kind 8 on it in its state 9 (0x1928F8 /
            // 0x192BD8 on +0x4C0), no pending actor (e.g. world 10's boardroom start: Sly under the pool's edge)
            if (IsA(po, 0x34))
                if (ACTOR* pa = PactorFromLo(po))
                {
                    StartInteract(pa, 8, s1, JTL_Max);
                    SetInteractState(pa, 9);
                    SetRunPending(p, -1, nullptr);
                }
            break;
        }
        glm::vec3 pos;
        float rad;
        GetRunAim(p, s1, &pos, &rad);
        if (std::getenv("SLY2N_SCPTPRINT"))
            std::printf("scpt place: %d onto %d (cid 0x%X) at %.1f %.1f %.1f heading %.3f\n", po->oid, s1 ? s1->oid : -1, s1 ? s1->Cid() : 0,
                        pos.x, pos.y, pos.z, rad);
        ALO* palo = static_cast<ALO*>(po);
        if (STEP* pstep = PstepFromLo(po))
        {
            SetStepPos(pstep, pos - glm::vec3(pstep->vecBase));  // vt slot 29
            SetStepPhysHeading(palo, rad);                         // 0x204660
        }
        else SetAloPos(palo, pos);
        const glm::mat3 m = cmm::BuildRotateMatrixAxis(rad, glm::vec3(0, 0, 1));  // 0x11B500
        if (STEP* pstep = PstepFromLo(po)) SetStepRotation(pstep, m);        // vt slot 30
        else SetAloRotation(palo, m);
        if (ACTOR* pa = PactorFromLo(po))
        {
            pa->posCmTarget = glm::vec4(pos + glm::vec3(0, 0, 1) * pa->sHeight, 1.0f);
            // (0x1E26A8: only when not held for a talk (+0x12F4 0): a held actor stays in its 0x60B pose; audit P3)
            if (pc->N(0x14) == 0 && CActorTalkHold(po) == 0)
            {
                SetJumpKind(&pa->jump, 0xB, 0, 0);  // 0x195A00
                DetachAloContactsAll(pa->palo);     // 0x1F4E28
            }
            SetRunPending(p, -1, nullptr);
        }
        SetAloVelocityTo(palo, glm::vec3(0.0f));     // vt slot 35
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));  // vt slot 36
        break;
    }
    case 0x11:
        // a task's state (0x161F48 with the one entry {+0x10 task, +0x14 state}, from the runner's context)
        ApplyCheckpointTasks(g_psw, p->ploCtx, {{pc->N(0x10), pc->N(0x14)}});
        break;
    case 0x12:
    {
        // orig 0x1E50E0: the runner's pause of the game (the kinds +0x14): on (the old one ended first) / off
        // (TODO(media): 0x20F0C0 / 0x20F0F0's channel 4)
        const int fOn = pc->N(0x10), grf = pc->N(0x14);
        if (fOn)
        {
            if (p->fPause)
            {
                if (grf == p->grfPause) break;
                ResumeGame(p->grfPause);  // 0x20F030
                ResumeTalk();             // 0x20F0F0
            }
            PauseGame(grf);  // 0x20EFC0
            PauseTalk();     // 0x20F0C0 (the pad locked for the scene)
        }
        else
        {
            if (!p->fPause) break;
            ResumeGame(p->grfPause);
            ResumeTalk();
        }
        p->grfPause = grf;
        p->fPause = fOn;
        break;
    }
    case 0x55:
    case 0x56:
    {
        // orig 0x171CF8 / 0x171D58: a guard's reactions (its job 0x9C) told to run the command list b (message 5), or
        // to stop (message 1)
        LO* pa = PloFind(p, pc->N(0x10));
        LO* pb = PloFind(p, pc->N(0x14));
        TellGuardCommands(pa, kind == 0x55 ? pb : nullptr, kind == 0x55);  // (scptai.cpp)
        if (FPrint()) std::fprintf(stderr, "scpt: guard %d: %s %d\n", pc->N(0x10), kind == 0x55 ? "run" : "stop", pc->N(0x14));
        if (kind == 0x55 && pc->b == 0)
        {
            p->ploGuardD8 = pa;
            p->ploCmdsDC = pb;
        }
        break;
    }
    case 0x17: CutCm(&g_cm); break;  // 0x14D8C0
    case 0x23:
        if (!PloFind(p, pc->N(0x14))) break;
        if (!actorOf(0x34)) break;
        SetRunPending(p, ic, p->plo24);
        break;
    case 0x22:
    {
        // orig 0x1E29D8: the actor's prize interaction (kind 12, 0x1424E0) with the object +0x14 (3: facing the camera
        // instead, 0x2046B0), its animation the ASEG +0x10 (0x15E938 kind 1 from the actor), +0x18 its +0x1318; waited
        // on until the interaction ends (0x142530)
        ACTOR* pa = PactorFromLo(actorOf(0x34));
        if (!pa) break;
        ASEG* paseg = dynamic_cast<ASEG*>(PloFindSwChild(pa->palo, pc->N(0x10)));
        if (!paseg) paseg = dynamic_cast<ASEG*>(PloFindSwNearest(pa->palo, pc->N(0x10)));
        LO* ploDir = nullptr;
        if (pc->N(0x14) == 3) SetStepPhysHeadingToward(pa->palo, g_cm.pos - PosLoWorld(pa->palo));
        else ploDir = PloFind(p, pc->N(0x14));
        if (FPrint()) std::fprintf(stderr, "scpt: prize interaction aseg %d (%p parent %d tmax %.2f) object %d\n", pc->N(0x10), (void*)paseg, paseg && paseg->paloParent ? paseg->paloParent->oid : -1, paseg ? paseg->asegd.tMax : -1.f, ploDir ? ploDir->oid : -1);
        StartPrizeInteract(pa, paseg, ploDir, pc->N(0x18));
        SetRunPending(p, ic, pa->palo);
        break;
    }
    case 0x1E:
    {
        // orig 0x00142278 (StepCmdPutDown: e.g. Ep1 Heist's SCPT 1903, Sly hands the truck key 124138 to Bentley):
        // the object +0x10 faded in (0x1F7C68, 0.25 s), held by the actor's throw controller +0x11B0, its target the
        // object +0x14's center (0x1A62E8), the arc (0x21FB70), the jump kind 7 (vt slot 88) and the throw (state 7);
        // waited on until the controller is back in state -1 (0x1424C0)
        LO* s1 = actorOf(0x34);
        ACTOR* pa = PactorFromLo(s1);
        if (!pa) break;
        LO* pa1 = PloFind(p, pc->N(0x10));
        LO* pb = PloFind(p, pc->N(0x14));
        SwitchLoFaded(pa1, true, 0.25f);
        pa->jtthrow.selected = pa1;
        if (pb) pa->jtthrow.targetWorld = IsA(pb, 0x30) ? PosAloCenter(static_cast<ALO*>(pb)) : PosLoWorld(pb);
        UpdateJtThrowArc(pa, false);
        NotifyActorJumpKind(pa, 7);
        SetJtThrowState(pa, 7);
        SetRunPending(p, ic, s1);
        break;
    }
    case 0x1D: case 0x1F: case 0x20: case 0x21:
    {
        // the actor's interaction with the object +0x10 (0x142140: kind 5, the reach; 0x142340: kind 9; 0x1421A0: the
        // reach let go (kind 5 ended), the +0x900 grab; 0x1421F8: kind 5 with the +0x900 reach), waited on (0x1E4EB8)
        ACTOR* pa = PactorFromLo(actorOf(0x34));
        if (!pa) break;
        LO* pt = PloFind(p, pc->N(0x10));
        switch (kind)
        {
        case 0x1D: SetActorInteractObject(pa, 5, pt); break;
        case 0x1F: SetActorInteractObject(pa, 9, pt); break;
        case 0x20:
            // orig 0x1421A0: the reach ended (0x142140(0)), then the object held by the interaction's reach (+0x900:
            // 0x1CD098) or the held one let go (0x1CD2C0); natively only the first part ran (audit 2026-11 P2)
            SetActorInteractObject(pa, 5, nullptr);
            if (pt) AttachReachObject(&pa->inter.reach, pt);
            else ReleaseReachObject(&pa->inter.reach);
            break;
        case 0x21:
            // orig 0x1421F8: kind 5 with the object, then the held object put there (+0x900: 0x1CD308)
            SetActorInteractObject(pa, 5, pt);
            if (pt) PlaceReachObject(&pa->inter.reach, pt);
            break;
        default: break;
        }
        if (kind != 0x20) SetRunPending(p, ic, pa->palo);
        break;
    }
    case 0x25: p->fE0 = 1; break;
    case 0x26: p->fE0 = 0; break;
    case 0x2C:
    case 0x2D:
    {
        // an object switched on / off (faded over +0x14 when an ALO); waited for when b is 0 (TODO: +0x60)
        LO* plo = PloFind(p, pc->N(0x10));
        if (FPrint()) std::fprintf(stderr, "scpt: switch %d (cid 0x%X) %s\n", pc->N(0x10), plo ? plo->Cid() : -1, kind == 0x2C ? "on" : "off");
        SwitchLoFaded(plo, kind == 0x2C, pc->G(0x14));  // 0x210338
        if (plo && pc->b == 0 && IsA(plo, 0x30)) p->ploFade = plo;
        break;
    }
    case 0x2E: case 0x2F: case 0x30: case 0x31:
    {
        // orig: 0x1E1E30 -> 0x210408 / 0x2104A0: mesh-group visibility
        // and collision flags, resolved from the runner's context (no default object).
        LO* plo = PloFindRunEvent(p, pc->N(0x10));
        if (!plo || !IsA(plo, 0x30)) break;
        const bool fOn = kind == 0x2E || kind == 0x30;
        if (kind <= 0x2F) ShowAloGlobs(static_cast<ALO*>(plo), pc->N(0x14), fOn);
        else SetAloSoesByTag(static_cast<ALO*>(plo), pc->N(0x14), fOn);
        break;
    }
    case 0x34:
    {
        // orig: 0x1E1E30 -> 0x210538: the authored burst, optional source/contact and scale.
        LO* plo = PloFindRunEvent(p, pc->N(0x10));
        if (!plo) break;
        BURSTPARAMS bp;
        bp.grf = 1;
        bp.plo = PloFindRunEvent(p, pc->N(0x14));
        if (!bp.plo) bp.plo = plo;
        bp.ploTouch = PloFindRunEvent(p, pc->N(0x18));
        if (bp.ploTouch) bp.grf |= 0x20;
        if (pc->G(0x1C) != 1.0f)
        {
            bp.g8 = pc->G(0x1C);
            bp.grf |= 0x18;
        }
        ExplodeLo(plo, &bp);
        break;
    }
    case 0x32: JoltCm(&g_cm, pc->N(0x10)); break;  // 0x14F750
    case 0x33: TriggerJoyRumbleRumk(pc->N(0x14), pc->G(0x10), "scpt 0x33"); break;  // 0x187E28 (rumk +0x14, dt +0x10)
    case 0x38:
    {
        // a timed camera block: its duration (+0x40; 0: the sum of its commands') to its end (0x39)
        float g = pc->G(0x40);
        const bool fSum = g == 0.0f;
        int icEnd = -1, depth = 0;
        for (int i = ic + 1; i < p->icLast && icEnd < 0; ++i)
        {
            SCPTCMD* pe = &ps->acmd[static_cast<size_t>(i)];
            if (pe->kind == 1) --depth;
            else if (pe->kind == 0) ++depth;
            else if (pe->kind == 0x39) { if (depth == 0) icEnd = i; }
            else if (depth == 0 && fSum) g += GCmdDuration(p, pe);
        }
        if (icEnd >= 0) StartRunCamera(p, pc, &ps->acmd[static_cast<size_t>(icEnd)], g);
        if (FPrint()) std::fprintf(stderr, "scpt: camera block +0x40 %.3f (sum %d) dt %.3f end %d: until %.3f\n", pc->G(0x40), fSum, g, icEnd, p->f80);
        break;
    }
    case 0x39:
        if (!(p->f80 == FLT_MAX)) break;
        if (pc->N(0x40) == 0) p->f9C = 0;
        else EndRunCamera(p, pc->N(0x44));
        break;
    case 0x3A: EndRunCamera(p, pc->N(0x10)); break;
    case 0x3D: g_cmpolicies.cpfree.SetFocusObject(PloFind(p, pc->N(0x10))); break;   // 0x150E20
    case 0x3E: g_cmpolicies.cpfree.SetTargetObject(PloFind(p, pc->N(0x10))); break;  // 0x150E60
    case 0x3F:
        g_cmpolicies.cpfree.SetTargets(glm::vec3(pc->G(0x10), pc->G(0x14), pc->G(0x18)),
                                       glm::vec3(pc->G(0x20), pc->G(0x24), pc->G(0x28)), pc->G(0x30));  // 0x150EA0
        break;
    case 0x40: g_cmpolicies.cpfree.SetZoomTarget(pc->G(0x10)); break;  // 0x150EE8
    case 0x41: g_cmpolicies.cpfree.Snap(); break;                     // 0x150EF0
    // the sound commands (sound.h; media code): a dialog line (+0x68 its command), the subtitles' reset
    // (0x227788: TODO with the subtitles), the music pushed / popped
    case 0x44: ShowTitle(); break;  // 0x1DD820 on 0x3DA530 (then waited for until gone: a new game continues the script)
    case 0x52: break;  // (a wait only: the dialog line, FCmdDone)
    case 0x42: g_dialogsub.grf = static_cast<uint32_t>(pc->N(0x10)) & 0xF0; break;  // (0x4435F8: the prompt's buttons,
                       // string 0x2F0; the wait for the button in FCmdDone)
    case 0x45:
        PsndStartDialog(pc->N(0x10), pc->N(0x14));  // 0x1FE5F0
        p->pcDialog = pc;
        break;
    case 0x46: ResetDialogSubtitle(); break;
    case 0x2B:
    {
        // the actor launched toward the object +0x10's aim (0x141968: +0x14 the elevation's share, +0x18 the air
        // control's weight)
        ACTOR* pa = PactorFromLo(actorOf(0x34));
        if (!pa) break;
        glm::vec3 pos(0.0f);
        GetRunAim(p, PloFind(p, pc->N(0x10)), &pos, nullptr);  // 0x1E4D40
        LaunchActorTo(pa, pos, pc->G(0x14), pc->G(0x18));
        break;
    }
    case 0x35: SetEyesClosedOf(p->ploCtx, pc->N(0x10), pc->N(0x14), pc->G(0x18)); break;  // 0x210678 (the eyes)
    case 0x47: HoldRunEffects(p, p->plo24, pc->N(0x14), pc->N(0x18)); break;     // 0x1E51A0 (the talk's face held)
    case 0x48: ReleaseRunEffects(p, p->plo24, pc->N(0x14), pc->N(0x18)); break;  // 0x1E5280
    case 0x3B:
    {
        // the binocucom's talk (0x19CC50 view 1: the other speaker +0x14, the window screen object +0x18)
        LO* ploSpeaker = PloFind(p, pc->N(0x14));
        LO* ploScreen = pc->N(0x18) != -1 && g_psw ? PloFindSwObjectByOid(g_psw, pc->N(0x18)) : nullptr;  // 0x15E938
        LO* po = PloFind(p, pc->N(0x10));
        ACTOR* pa = po && FCidDerivedFrom(po->Cid(), 0x34) ? PactorFromLo(po) : nullptr;
        if (!pa) break;
        p->paBinoc = pa;
        OpenBinoc(pa, 1, ploSpeaker, ploScreen);
        break;
    }
    case 0x3C:
        // 0x1E54B0: the binocucom closed (0x19CCE8); its window's state change resets the subtitle (0x19D660 ->
        // 0x227788)
        if (p->paBinoc) CloseBinoc(p->paBinoc);
        p->paBinoc = nullptr;
        ResetDialogSubtitle();  // (the media code: the subtitle gone with the talk's end, as the reference shows)
        break;
    case 0x4F:
    {
        // Murray holds an object up (a gate): his state 6 with its state machine +0x1520, his and the object's state
        // machines run to +0x1C (0x1B9C00)
        MURRAY* pm = IsA(p->plo24, 0x36) ? PmurrayFromLo(p->plo24) : nullptr;
        if (!pm) break;
        LO* pt = PloFind(p, pc->N(0x14));  // 0x1E1DB8
        if (!pt) break;
        SMA* psmaObj = nullptr;
        for (SMA* psma : g_apsma)  // 0x1F67E0 (any state machine of the object)
            if (psma->palo == pt) { psmaObj = psma; break; }
        if (!psmaObj) break;
        StartMurrayHold(pm, psmaObj, pc->N(0x1C), pc->N(0x18), pc->N(0x20));  // 0x1B9C00
        break;
    }
    case 0x53: HudPrompt(pc->N(0x10), pc->G(0x14)); break;  // HintBox: 0x1DA4B8 (SetHintBoxText) on the HUD prompt 0x3DA0C0
    case 0x54: HudPromptEnd(pc->N(0x10)); break;            // ClearHintBox: 0x1DA578 (ClearIfSlotId)
    case 0x50: case 0x51:
        // Bentley (cid 0x37) watched until his state is (0x50) / is not (0x51) 6 (0x1E3890)
        if (LO* pb = actorOf(0x37)) SetRunPending(p, ic, pb);
        break;
    case 0x57:
        // Murray (cid 0x36): the Bentley toss (0x1B9880: his state 15)
        if (LO* pm = actorOf(0x36))
            if (MURRAY* pmur = PmurrayFromLo(pm)) SetMurrayState(pmur, 15);
        break;
    case 0x58:
    {
        // orig 0x1E1E30 case 0x58: Bentley (cid 0x37, +0x14) jumps (kind 3 from his state 0x11, else 1) and is driven
        // (0x1E46A0) until Murray (+0x10) holds him: the partner jump into Murray's hands (w32's 2343 at the battery)
        BENTLEY* pb = PbentleyFromLo(actorOf(0x37));
        LO* plo = PloFind(p, pc->N(0x10));
        if (pb && plo && IsA(plo, 0x36))
        {
            SetJumpKind(&pb->jump, pb->jump.state == 0x11 ? 3 : 1, 0, nullptr);  // 0x195A00
            SetRunPending(p, ic, pb->palo);                                      // SetJsgJt
        }
        break;
    }
    case 0x59:
    {
        // orig case 0x59: Murray (+0x14) aiming the toss (+0x1910 in its states 4..6) turns toward the object +0x10
        // (0x220508); the wait (0x1E3890) is on the runner's actor
        MURRAY* pm = PmurrayFromLo(actorOf(0x36));
        if (pm && pm->jtthrow1910.state < 7 && pm->jtthrow1910.state >= 4)
            if (LO* plo = PloFind(p, pc->N(0x10))) SetJtThrowTarget(pm->jtthrow1910, plo);
        break;
    }
    case 0x5A:
    {
        // orig case 0x5A: as 0x59, then Murray is driven (0x1E46A0: the throw pressed once his aim has arrived); the
        // controller's +0 kept for the wait (+0x18 here: armed)
        MURRAY* pm = PmurrayFromLo(actorOf(0x36));
        if (pm && pm->jtthrow1910.state < 7 && pm->jtthrow1910.state >= 4)
        {
            pc->N(0x18) = 1;
            if (LO* plo = PloFind(p, pc->N(0x10)))
            {
                SetJtThrowTarget(pm->jtthrow1910, plo);
                SetRunPending(p, ic, pm->palo);  // SetJsgJt
            }
        }
        break;
    }
    case 0x49:
    {
        // orig: 0x001E3400.. (the dance's state with the prompts of the block's beats: the 0x4A commands after it at
        // depth 0, 6 at most, up to the next 0x49 at depth 0: 0x1531A8)
        if (!FDanceDirector()) break;
        std::vector<std::pair<float, int>> aent;
        int cBlock = 0;
        for (int i = ic + 1; i < p->icLast; ++i)
        {
            SCPTCMD& c = ps->acmd[static_cast<size_t>(i)];
            if (c.kind == 0) ++cBlock;
            else if (c.kind == 1) --cBlock;
            else if (c.kind == 0x4A && cBlock == 0 && aent.size() < 6) aent.emplace_back(c.G(0x10), c.N(0x14));
            else if (c.kind == 0x49 && cBlock == 0) break;
        }
        SetDancePrompts(pc->N(0x10), aent);
        break;
    }
    case 0x4A:
        // a beat of the dancer (cid 0x44) for the director (0x1532D8); the runner waits for its music time
        if (p->plo24 && FDancer(p->plo24) && FDanceDirector()) AddDanceBeat(p->plo24, pc->N(0x14), pc->N(0x18), pc->G(0x10));
        break;
    case 0x4B:
        // PlayAsegDefault: the dancer's move (0x1524A0) from the ASEG (-1: none)
        if (p->plo24 && FDancer(p->plo24))
            PlayDancerDefault(p->plo24, pc->N(0x10) == -1 ? nullptr : PloFindSwNearest(p->plo24, pc->N(0x10)));
        break;
    case 0x4E: break;  // (WaitTimer: a wait only)
    case 0x36:
    {
        // orig: 0x00210740 (PlayAsegSoundEvent, as the ASEG event kind 0): the sound +0x10 at the object +0x14 (-1:
        // none, found from the context: 0x15E938 kind 4), falloff +0x18 .. +0x1C, pitch +0x20, volume +0x28 (+0x24
        // the fade: 0x1FB520 is empty); a looping one (kind 2) kept by the relay
        const int idSnd = pc->N(0x10);
        if (idSnd == -1) break;
        LO* pso = pc->N(0x14) != -1 ? PloFind(p, pc->N(0x14)) : nullptr;
        CSnd* psnd = PsndNewPlain(idSnd);
        if (!psnd) break;
        psnd->SetSource(pso);
        psnd->SetFalloff(static_cast<float>(pc->N(0x18)), static_cast<float>(pc->N(0x1C)));
        psnd->SetVolume(pc->G(0x28));
        psnd->SetPitch(pc->G(0x20) + GSoundPitchOffset(pso));  // (asegevent.cpp: the guard scale term; audit S2)
        if (psnd->Kind() == 2) p->psnd = CSndRef(psnd);
        break;
    }
    case 0x37:
        // orig: 0x002108E8 (StopAsegSoundEvent: the relay's kept sound stopped when it is +0x10 (-1: any))
        if (p->psnd)
        {
            const CSndPlain* pp = dynamic_cast<const CSndPlain*>(p->psnd.get());
            if (pc->N(0x10) != -1 && (!pp || pp->m_sfxid != pc->N(0x10))) break;
            p->psnd->Stop(0.0f);
            p->psnd = CSndRef();
        }
        break;
    case 0x4C: PsndPushMusic(pc->N(0x10)); break;                 // 0x1FE150
    case 0x4D: PopMusic(pc->N(0x10)); break;                      // 0x1FE310
    case 0x03: case 0x04: break;  // (waits only)
    case 0x43: break;  // (a wait only: FCmdDone, the actor's slot 89 kind)
    default:
    {
        char sz[64];
        std::snprintf(sz, sizeof sz, "command 0x%X", kind);
        WarnOnce(sz);
        break;
    }
    }
    p->t34 = TScpt(ps);
}

// orig: 0x001E3890 (the command is done, or needs no waiting)
bool FCmdDone(RUN* p, SCPTCMD* pc)
{
    SCPT* ps = PscptOf(p->pscptLo);
    const float dt = TScpt(ps) - p->t34;
    switch (pc->kind)
    {
    case 0x03:
        for (int i = 0; i < pc->N(0x10) && i < 4; ++i)
            if (PrunFind(ps, pc->a[1 + i].n, p)) return false;
        return true;
    case 0x04:
    {
        const bool f = FHasAlarm(ps, pc->N(0x10));
        if (!f) return false;
        if (pc->N(0x14) != 0) RemoveAlarm(ps, pc->N(0x10));
        return true;
    }
    case 0x43:
        // orig 0x1E3890 case 0x43: an actor (cid 0x34) is done when its slot 89 kind is +0x10 (e.g. 8:2121's SCPT 2126
        // waits for Murray to be standing (0) again after a hit before its 0.5 s wait and the walk 2125); else at once
        if (IsA(p->plo24, 0x34))
        {
            ACTOR* pa = PactorFromLo(p->plo24);
            return !pa || ActorJumpKind(pa) == pc->N(0x10);
        }
        return true;
    case 0x50: case 0x51:
    {
        BENTLEY* pb = PbentleyFromLo(p->ploWatch);
        return !pb || ((pb->state == 6) == (pc->kind == 0x50));
    }
    case 0x55:
        // a command 0x55 without its wait: until the guard's reactions no longer run the list (0x171DA8)
        if (p->ploCmdsDC && !FGuardRunsCommands(p->ploGuardD8, p->ploCmdsDC))
        {
            p->ploGuardD8 = nullptr;
            p->ploCmdsDC = nullptr;
        }
        return p->ploCmdsDC == nullptr;
    case 0x0A:
        if (p->ploAnim)
        {
            if (FActorScriptAnim(p->ploAnim)) return false;  // 0x144038
            p->ploAnim = nullptr;
            return true;
        }
        if (p->pasega && FAsegaRetracted(p->pasega)) p->pasega = nullptr;  // 0x131968
        return p->pasega == nullptr;
    case 0x0E:
        if (p->ploScreen && FScreenObjDone(p->ploScreen))
        {
            if (FPrint()) std::printf("scpt: screen %d gone (real t %.3f)\n", p->ploScreen->oid, g_clock.t3C);
            HideScreenObj(p->ploScreen);  // 0x1E5468
            p->ploScreen = nullptr;
        }
        return p->ploScreen == nullptr;
    case 0x4A: case 0x4E: return pc->G(0x10) <= TMusicTop();  // (the beat's / WaitTimer's music time: 0x1FE3F8)
    case 0x0B:
        if (!p->pscptSub) return true;
        if (!FScptRunning(p->pscptSub)) p->pscptSub = nullptr;
        return p->pscptSub == nullptr;
    case 0x0D:
        if (p->psma && (FSmaRetracted(p->psma) || p->fSmaGoal)) p->ReleaseSmaWatch();
        return p->psma == nullptr;
    case 0x1A:
    {
        // within +0x14 of the point (limited to the pipe's range, 0x19E110 / 0x11D948)
        ACTOR* pa = PactorFromLo(p->ploWatch);
        if (!pa || !pa->pcrvF50) return true;
        // orig: 0x001E3890 calls 0x0019E110 before limiting the goal.
        // Climbing itself stops short of an open pipe's geometric endpoints.
        const cmm::LM lm = LmClimb(pa);
        const float g = p->s7C < lm.gMin ? lm.gMin : (lm.gMax < p->s7C ? lm.gMax : p->s7C);
        return std::fabs(pa->sCrvF50 - g) < pc->G(0x14);
    }
    case 0x2A:
    {
        ACTOR* pa = PactorFromLo(p->ploWatch);
        return !pa || pa->jump.kind == -1;
    }
    case 0x29:
    {
        // orig 0x1E3890: still in kind 8: done within +0x18 (x, y) of the target
        ACTOR* pa = PactorFromLo(p->ploWatch);
        if (!pa || pa->nSub4CC == -1 || pa->n4C8 != 8) return true;
        LO* pt = PloFind(p, pc->N(0x10));
        if (!pt) return true;
        glm::vec3 pos;
        GetRunAim(p, pt, &pos, nullptr);
        const glm::vec3 q = PosLoWorld(pa->palo);
        return std::sqrt((q.x - pos.x) * (q.x - pos.x) + (q.y - pos.y) * (q.y - pos.y)) < pc->G(0x18);
    }
    case 0x27:
    case 0x28:
    {
        // orig: 0x001E3890 / 0x00144EE8: WalkTo also accepts a stopped A8
        // follower. Both commands clear the actor's stack when they finish.
        ACTOR* actor = PactorFromLo(p->plo24);
        if (!actor) return true;
        bool done;
        if (pc->kind == 0x27)
        {
            const auto* follow = static_cast<const JOBACTORFOLLOW*>(PjobFind(actor, 0xA8));
            done = !follow || follow->FArrived();
        }
        else
        {
            const auto* walk = static_cast<const JOBACTORWALK*>(PjobFind(actor, 0xC1));
            done = !walk || walk->FAtGoal();
        }
        if (!done) return false;
        ClearJobs(actor);
        if (pc->N(0x24) != -1) EndActorScriptAnim(actor);
        return true;
    }
    case 0x24:
    {
        LO* po = p->ploWatch;
        if (!po) return true;
        LO* pt = PloFind(p, pc->N(0x10));
        if (pt)
        {
            glm::vec3 pos;
            GetRunAim(p, pt, &pos, nullptr);
            const glm::vec3 q = PosAloCenter(static_cast<ALO*>(po));  // (0x1E3890 GoTo: 0x1A62E8, GLengthXY)
            const float dx = q.x - pos.x, dy = q.y - pos.y;
            if (std::getenv("SLY2N_DRIVEWHY"))
                std::printf("drive 0x24 done? pos %.4f %.4f center %.4f %.4f aim %.4f %.4f d %.5f within %.2f v %.6g %.6g\n", q.x, q.y,
                            PosAloCenter(static_cast<ALO*>(po)).x, PosAloCenter(static_cast<ALO*>(po)).y, pos.x, pos.y,
                            std::sqrt(dx * dx + dy * dy), pc->G(0x18), VelAloPos(static_cast<ALO*>(po)).x, VelAloPos(static_cast<ALO*>(po)).y);
            if (ACTOR* pa = PactorFromLo(po)) SetActorWatchTarget(pa, pos);  // 0x1448F0
            if (!(std::sqrt(dx * dx + dy * dy) < pc->G(0x18))) return false;
        }
        if (pc->N(0x20) != -1)
            if (ACTOR* pa = PactorFromLo(po)) EndActorScriptAnim(pa);  // 0x143FF8 (the walk's animation over)
        return true;
    }
    case 0x58:
    {
        // Bentley's partner jump: done once Murray's toss (+0x1910) holds him (its states 4..6), or after 7 s
        if (!p->ploWatch) return true;
        MURRAY* pm = PmurrayFromLo(PloFind(p, pc->N(0x10)));
        if (dt > 7.0f) return true;  // 0x2C3620
        return !pm || (pm->jtthrow1910.state < 7 && pm->jtthrow1910.state >= 4);
    }
    case 0x59:
    {
        // Murray's aim: done when its point has reached the transition's end (0.1 on each axis), or after 10 s
        MURRAY* pm = PmurrayFromLo(p->ploWatch);
        if (!pm) return true;
        if (dt > 10.0f) return true;  // 0x2C3624
        const JTTHROW& t = pm->jtthrow1910;
        return FFloatsNear(t.transitionEnd.x, t.targetWorld.x, 0.1f) && FFloatsNear(t.transitionEnd.y, t.targetWorld.y, 0.1f) &&
               FFloatsNear(t.transitionEnd.z, t.targetWorld.z, 0.1f);  // 0x2C3628
    }
    case 0x5A:
    {
        // Murray's throw: done when his toss is over (-1, or back at 0: his slot 88 (0) then ends it) and the
        // controller's owner has landed (its jump's state -1), or after 10 s
        MURRAY* pm = PmurrayFromLo(p->ploWatch);
        if (!pm) return true;
        if (dt > 10.0f) return true;  // 0x2C362C
        const int st = pm->jtthrow1910.state;
        if (st != -1 && st != 0) return false;
        if (st == 0) NotifyActorJumpKind(pm, 0);  // slot 88 (0)
        const ACTOR* po = pc->N(0x18) ? pm->jtthrow1910.owner : nullptr;
        return po && po->jump.state == -1;
    }
    case 0x1D: case 0x21:
    {
        ACTOR* pa = PactorFromLo(p->ploWatch);
        return !pa || FActorPoseDone(pa);  // 0x1423A0
    }
    case 0x1F:
    {
        ACTOR* pa = PactorFromLo(p->ploWatch);
        return !pa || pa->n4C8 != 9;  // 0x1424D0
    }
    case 0x1E:
    {
        ACTOR* pa = PactorFromLo(p->ploWatch);
        return !pa || pa->jtthrow.state == -1;  // 0x1424C0
    }
    case 0x22:
    {
        ACTOR* pa = PactorFromLo(p->ploWatch);
        return !pa || pa->n4C8 != 12;  // 0x142530 (the prize interaction over)
    }
    case 0x23:
    case 0x33: return pc->G(0x10) < dt;
    case 0x32: return 0.5f < dt;  // (0x2BD850)
    case 0x53: return pc->G(0x14) < dt;
    case 0x3B: return !p->paBinoc || p->paBinoc->binoc.win.state == 1;   // (+0xF28)
    case 0x3C: return !p->paBinoc || p->paBinoc->binoc.win.state == -1;
    case 0x39:
        // the camera block's end: once its time has come, the move revoked (+0x40) or left on (0x1E5A98)
        if (!(p->f80 <= g_clock.t)) return false;
        if (pc->N(0x40) == 0)
        {
            p->f9C = 0;
            return true;
        }
        EndRunCamera(p, pc->N(0x44));
        return true;
    case 0x2C:
    case 0x2D:
        // the faded object: in the world and fully shown (+0x12C 1) / out of the world
        if (LO* po = p->ploFade)
        {
            if (FPrint())
                std::fprintf(stderr, "scpt: fade %d: inworld %d fade %.2f rate %.2f iupd %d cid 0x%X st %d r %.0f mask 0x%X active %d grfCm 0x%X\n", po->oid, po->fInWorld,
                             static_cast<ALO*>(po)->gFade, static_cast<ALO*>(po)->dgFade, po->iupd, po->Cid(), po->st, po->sRadius, po->bMask, po->fNodeActive, g_actview.grfCm);
            if (FPrint()) { const glm::vec3 a = PosAloCenter(static_cast<ALO*>(po)); const glm::vec3 w = PosLoWorld(po); std::fprintf(stderr, "   at %.0f %.0f %.0f (lo %.0f %.0f %.0f) cm %.0f %.0f %.0f queued %d root %d\n", a.x, a.y, a.z, w.x, w.y, w.z, g_actview.posCm.x, g_actview.posCm.y, g_actview.posCm.z, po->fQueued, po->paloParent == nullptr); }
            if (pc->kind == 0x2C ? (po->fInWorld && static_cast<ALO*>(po)->gFade == 1.0f) : !po->fInWorld) p->ploFade = nullptr;
        }
        return p->ploFade == nullptr;
    case 0x42:
        // a button pressed (+0x10, the game's layout; 0x800 Start: the boot world's skip of the intro): taken from
        // the pad (0x187D80, then 0x187DE0 again) and the prompt word 0x4435F8 cleared. +0x14 != -1 asks a player's
        // pad (0x3E133C's list, 0x1A6290 / 0x1C4700: TODO; single pad here).
    {
        const uint32_t grfTaken = GrfbtnTakePressed(static_cast<uint32_t>(pc->N(0x10)));
        const bool fDone = grfTaken != 0;
        if (fDone)
        {
            g_dialogsub.grf = 0;
            // 0x187DE0 on the pad: the button held stays handled (+0x10BC) until released, so the players'
            // controllers (+0x10B4 & ~+0x10BC) do not see it held afterwards
            SetJoyPs2BtnHandled(&g_joyps2, grfTaken);
        }
        else NoteScptWaitsForButton();   // (skipcut.h: a scene waiting for the player is not skipped)
        return fDone;
    }
    case 0x44: return !FTitleShown();                 // (0x1D9880 on 0x3DA530: the title gone)
    case 0x4F:
    {
        // Murray out of state 6, or his state machine at +0x1C
        MURRAY* pm = IsA(p->plo24, 0x36) ? PmurrayFromLo(p->plo24) : nullptr;
        if (!pm) return false;
        if (pm->state != 6) return true;
        if (pc->N(0x1C) == -1) return false;
        return pm->psma && OidSmaCur(pm->psma) == pc->N(0x1C);  // 0x1F07E8
    }
    case 0x45: return !FDialogPlaying(pc->N(0x10));   // (0x1FE848: the line over)
    case 0x52:
    {
        // the runner's dialog line (+0x68) over, or played for +0x10 s (negative: till its end)
        const int id = p->pcDialog ? p->pcDialog->N(0x10) : -1;
        if (std::getenv("SLY2N_DLGWAIT"))
            std::fprintf(stderr, "scpt %d: 0x52 waits line %d (g %.2f): playing %d (current %d) t %.3f\n", p->pscptLo ? p->pscptLo->oid : -1, id,
                         pc->G(0x10), FDialogPlaying(id) ? 1 : 0, IdDialogCur(), g_clock.t);
        if (!FDialogPlaying(id)) return true;  // (-1: any line, 0x1FE848)
        if (pc->G(0x10) < 0.0f) return false;
        return 0.0f <= TDialog() - pc->G(0x10);
    }
    default: return true;
    }
}

// (native QoL, skipcut.h) The scene a script plays when it pauses the game (0x12): its own commands
// and the scripts it starts (0x0B, transitively, found by oid in this world). A dialogue line (0x45) or a binocucom
// talk (0x3B) anywhere in it: a story scene; camera blocks (0x38) only, three or more: a cinematic of several shots;
// fewer: a gate / door / pickup shown for a moment, a pad lock only (doors, the safehouse, banners): not scenes.
}  // namespace
int SceneSkipKind(LO* pscpt)
{
    std::vector<LO*> aplo{pscpt}, aploSeen{pscpt};
    int cDialog = 0, cCamera = 0, cBinoc = 0;
    while (!aplo.empty())
    {
        LO* plo = aplo.back();
        aplo.pop_back();
        SCPT* ps = PscptOf(plo);
        if (!ps) continue;
        // (the starting script: only its paused part, from its first pause on to its last pause off after that;
        // what it does after it has given the pad back, e.g. 2:1770's talk started after its animation, is not this
        // scene: the talk is its own)
        size_t icFirst = 0, icLast = ps->acmd.size();
        if (plo == pscpt)
        {
            while (icFirst < ps->acmd.size() && !(ps->acmd[icFirst].kind == 0x12 && ps->acmd[icFirst].a[0].n)) ++icFirst;
            for (size_t i = ps->acmd.size(); i-- > icFirst;)
                if (ps->acmd[i].kind == 0x12 && !ps->acmd[i].a[0].n) { icLast = i; break; }
        }
        for (size_t ic = icFirst; ic < icLast; ++ic)
        {
            const SCPTCMD& c = ps->acmd[ic];
            cDialog += c.kind == 0x45;
            cCamera += c.kind == 0x38;
            cBinoc += c.kind == 0x3B;
            if (c.kind != 0x0B) continue;
            const int oid = c.a[0].n;
            for (auto& [ploSub, sSub] : MpScpt())
                if (ploSub->oid == oid && std::find(aploSeen.begin(), aploSeen.end(), ploSub) == aploSeen.end())
                {
                    aploSeen.push_back(ploSub);
                    aplo.push_back(ploSub);
                    break;
                }
        }
    }
    if (cDialog || cBinoc) return SKIPK_Dialog;
    return cCamera >= 3 ? SKIPK_Cinematic : SKIPK_None;
}
namespace
{

// orig: 0x001DFD58
void StartScptImpl(LO* pscpt, LO* ploStarter)
{
    SCPT* ps = PscptOf(pscpt);
    if (!ps || ps->fRunning) return;
    static const bool s_fOff = std::getenv("SLY2N_NOSCPT") != nullptr;  // (debug: no script runs)
    if (s_fOff) return;
    CoopOnScptStart(pscpt, ploStarter);   // (coop.h: the host's cutscenes, failure screens, scripted moves run in both)
    if (std::getenv("SLY2N_SCPTDIALOG"))  // (debug: the scripts with dialog lines as they start)
    {
        int c = 0, idFirst = -1;
        for (SCPTCMD& cmd : ps->acmd)
            if (cmd.kind == 0x45 && c++ == 0) idFirst = cmd.N(0x10);
        if (c) std::printf("scpt %d started (by %d): %d dialog lines, the first %d\n", pscpt->oid, ploStarter ? ploStarter->oid : -1, c, idFirst);
    }
    ps->ploStarter = ploStarter;
    ps->fRunning = 1;
    if (ps->nSkipKind < 0) ps->nSkipKind = SceneSkipKind(pscpt);
    ps->idSkipScene = ps->nSkipKind == SKIPK_Dialog || ps->nSkipKind == SKIPK_Cinematic ? NewSkipCutScene() : 0;
    ps->f78 = 0;
    ++g_mpoidcScptStart[pscpt->oid];   // (debug: the route's waitscpt)
    if (FPrint())
    {
        std::fprintf(stderr, "scpt %d: start (%zu commands) t %.3f\n", pscpt->oid, ps->acmd.size(), g_clock.t);
        for (size_t i = 0; i < ps->acmd.size(); ++i)
            std::fprintf(stderr, "  cmd %zu kind 0x%X b %d [%d %d %d]\n", i, ps->acmd[i].kind, ps->acmd[i].b, ps->acmd[i].N(0x10), ps->acmd[i].N(0x14), ps->acmd[i].N(0x18));
        for (SYMID k : {2u, 3u, 4u, 5u, 6u})
            if (CRef* pref = pscpt->pframe ? pscpt->pframe->PrefFindBinding(k, 0) : nullptr; pref && pref->m_tagk == TAGK_Proc)
            {
                std::string str;
                CRef body;
                body.SetPair(pref->m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
                std::fprintf(stderr, "  event %u: %s\n", k, str.substr(0, 3000).c_str());
            }
    }
    HandleLoSpliceEvent(pscpt, 5, false, 0, nullptr);  // vt slot 3 (0x1A61E0): 5, the tasks' 0x1A
    BroadcastTaskEvent(g_psw, 0x1A, pscpt);
    if (!ps->fRunning) return;
    if (ps->fDirty)
    {
        PcmdNew(ps, 1, 1);  // 0x1E02C0
        ps->fDirty = 0;
    }
    RunRun(PrunNew(pscpt, 0, nullptr));
    if (!ps->fRunning) return;
    if (ps->arun.empty())
    {
        StopScptImpl(pscpt);
        return;
    }
    uint32_t& grf = pscpt->iupd >= 0 ? g_psw->aupd[static_cast<size_t>(pscpt->iupd)].grf : pscpt->grfUpd;
    if (grf & GRFUPD_Update) return;
    SetLoSt(g_psw, pscpt, 1);  // 0x1A6CA8
    uint32_t& grf2 = pscpt->iupd >= 0 ? g_psw->aupd[static_cast<size_t>(pscpt->iupd)].grf : pscpt->grfUpd;
    grf2 |= GRFUPD_Update;
    ps->f78 = 1;
}

// orig: 0x001DFEB8
void StopScptImpl(LO* pscpt)
{
    SCPT* ps = PscptOf(pscpt);
    if (!ps || !ps->fRunning) return;
    ps->ploStarter = nullptr;
    ps->t = 0.0f;
    ps->fRunning = 0;
    if (FPrint())
    {
        CRef* pref = pscpt->pframe ? pscpt->pframe->PrefFindBinding(6, 1) : nullptr;
        std::fprintf(stderr, "scpt %d: stop t %.3f (event 6 %s)\n", pscpt->oid, g_clock.t, pref ? "bound" : "unbound");
        if (pref && pref->m_tagk == TAGK_Proc && pref->m_pproc)
        {
            std::string str;
            CRef body;
            body.SetPair(pref->m_pproc->m_ppairCodeExpr);
            PrintRef(body, &str);
            std::fprintf(stderr, "  %s\n", str.substr(0, 3000).c_str());
        }
    }
    DLI dli{ps, 0};
    Adli().push_back(&dli);
    while (dli.i < ps->arun.size())
    {
        PRUN r = ps->arun[dli.i++];
        RetireRun(r);
    }
    Adli().pop_back();
    if (FAlarmPrint()) std::fprintf(stderr, "alarms: cleared (stop) t %.3f\n", g_clock.t);
    ps->analarm.clear();  // 0x1E0120
    if (ps->f78)
    {
        SetLoSt(g_psw, pscpt, 2);
        uint32_t& grf = pscpt->iupd >= 0 ? g_psw->aupd[static_cast<size_t>(pscpt->iupd)].grf : pscpt->grfUpd;
        grf &= ~GRFUPD_Update;
    }
    HandleLoSpliceEvent(pscpt, 6, false, 0, nullptr);  // vt slot 3: 6, the tasks' 0x1B
    BroadcastTaskEvent(g_psw, 0x1B, pscpt);
}

// orig: 0x001DFC50 (vt slot 16)
void UpdateScpt(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1A5E28)) pfn(plo);
    SCPT* ps = PscptOf(plo);
    if (!ps || !ps->fRunning) return;
    ps->t += ps->fRealTime ? g_clock.dt40 : g_clock.dt;
    DLI dli{ps, 0};
    Adli().push_back(&dli);
    while (dli.i < ps->arun.size())
    {
        PRUN r = ps->arun[dli.i++];
        RunRun(r);
    }
    Adli().pop_back();
    if (ps->arun.empty()) StopScptImpl(plo);
}

// The builders' arguments (RefSetArgsFromSplice 0x201360): by their OTYP (1 a float, 2 a vector, 0x8006 an int list,
// else an int), split as the original's calls pass them: the ints (and vectors) in the integer registers, the
// floats in the float ones, in the wrapper's call order.
struct ARGS
{
    std::vector<SLOT> ai;
    std::vector<glm::vec4> aq;  // (the vectors, also counted in ai's order)
    std::vector<float> ag;
    std::vector<int> an;        // the int list
};
using PFNBUILD = void (*)(SCPT* ps, const ARGS& a);

struct THUNK
{
    uint32_t pfn;
    PFNBUILD pfnBuild;
    std::vector<int> aotyp;
    std::vector<int> aorder;  // the wrapper's call order (indices of its arguments)
};

int I(const ARGS& a, size_t i) { return i < a.ai.size() ? a.ai[i].n : 0; }
float F(const ARGS& a, size_t i) { return i < a.ag.size() ? a.ag[i] : 0.0f; }
glm::vec4 Q(const ARGS& a, size_t i) { return i < a.aq.size() ? a.aq[i] : glm::vec4(0.0f); }

void B1(SCPT* ps, int kind, int b, std::initializer_list<std::pair<uint32_t, int>> an)
{
    SCPTCMD* p = PcmdNew(ps, kind, b);
    for (const auto& [ib, n] : an) p->N(ib) = n;
    static const bool s_fRefs = std::getenv("SLY2N_TASKREFS") != nullptr;  // (debug: who sets which task's state)
    if (s_fRefs && kind == 0x11)
        std::fprintf(stderr, "taskref scpt %d: task %d state %d\n", PloLoading() ? PloLoading()->oid : -1, p->N(0x10), p->N(0x14));
}
// kind with b 1 and +0x10 = int 0
template <int K> void BI1(SCPT* ps, const ARGS& a) { B1(ps, K, 1, {{0x10, I(a, 0)}}); }
template <int K> void BII1(SCPT* ps, const ARGS& a) { B1(ps, K, 1, {{0x10, I(a, 0)}, {0x14, I(a, 1)}}); }
template <int K> void BII0(SCPT* ps, const ARGS& a) { B1(ps, K, 0, {{0x10, I(a, 0)}, {0x14, I(a, 1)}}); }
template <int K> void BIIb(SCPT* ps, const ARGS& a) { B1(ps, K, I(a, 2), {{0x10, I(a, 0)}, {0x14, I(a, 1)}}); }
template <int K> void BIb(SCPT* ps, const ARGS& a) { B1(ps, K, I(a, 1), {{0x10, I(a, 0)}}); }
template <int K, int Bv> void BNone(SCPT* ps, const ARGS&) { B1(ps, K, Bv, {}); }
template <int K> void BIFb(SCPT* ps, const ARGS& a)  // 0x1E0ED8 / 0x1E0F18 / 0x1E1970
{
    SCPTCMD* p = PcmdNew(ps, K, I(a, 1));
    p->N(0x10) = I(a, 0);
    p->G(0x14) = F(a, 0);
}
template <int K> void BIII1(SCPT* ps, const ARGS& a) { B1(ps, K, 1, {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 2)}}); }
void BVecCmd(SCPT* ps, int kind, const ARGS& a, SCPTCMD** pp)
{
    SCPTCMD* p = PcmdNew(ps, kind, 1);
    const glm::vec4 q0 = Q(a, 0), q1 = Q(a, 1);
    for (int i = 0; i < 4; ++i) p->G(0x10 + 4 * i) = q0[i];
    p->G(0x20) = q1.x * c_degToRad;
    p->G(0x24) = q1.y * c_degToRad;
    p->G(0x28) = q1.z * c_degToRad;
    *pp = p;
}

const std::vector<THUNK>& Athunk()
{
    static const std::vector<THUNK> s_a = {
        {0x106A80, BI1<0>, {6}, {0}},                                    // 0x1E0290
        {0x106B08, BNone<1, 1>, {}, {}},                                // 0x1E02C0
        {0x106B80, BI1<2>, {6}, {0}},                                    // 0x1E02E0
        {0x106C08, [](SCPT* ps, const ARGS& a) {                         // 0x1E0310
             SCPTCMD* p = PcmdNew(ps, 3, 0);
             const int c = std::min<int>(4, static_cast<int>(a.an.size()));
             p->N(0x10) = c;
             for (int i = 0; i < c; ++i) p->a[1 + i].n = a.an[static_cast<size_t>(i)];
         }, {0x8006}, {0}},
        {0x106CA8, BII0<4>, {6, 0}, {0, 1}},                             // 0x1E03A0
        {0x106D50, BII1<5>, {6, 6}, {0, 1}},                             // 0x1E03E0
        {0x106DF8, BI1<6>, {6}, {0}},
        {0x106E80, BI1<7>, {6}, {0}},
        {0x106F08, BI1<8>, {6}, {0}},
        {0x106F90, BI1<9>, {6}, {0}},
        {0x107018, [](SCPT* ps, const ARGS& a) { B1(ps, 0xA, I(a, 2), {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 3)}}); },
         {6, 6, 0, 0x100B}, {0, 1, 2, 3}},                               // 0x1E04E0
        {0x1070E0, BIIb<0xB>, {6, 6, 0}, {0, 1, 2}},                     // 0x1E0530
        {0x107198, BI1<0xC>, {6}, {0}},
        {0x107220, [](SCPT* ps, const ARGS& a) {                         // 0x1E05A0
             SCPTCMD* p = PcmdNew(ps, 0xD, I(a, 3));
             p->N(0x10) = I(a, 0);
             p->N(0x14) = I(a, 1);
             p->N(0x18) = I(a, 2);
             p->N(0x1C) = I(a, 4);
             if (I(a, 4)) p->b = 1;
         }, {6, 6, 6, 0, 0}, {0, 1, 2, 3, 4}},
        {0x107300, BIIb<0xE>, {6, 6, 0}, {0, 1, 2}},                     // 0x1E0610
        {0x1073B8, BI1<0xF>, {6}, {0}},
        {0x107440, [](SCPT* ps, const ARGS& a) { PcmdNew(ps, 0x10, 1)->G(0x10) = F(a, 0); }, {1}, {0}},  // 0x1E0680
        {0x1074C8, BII1<0x11>, {6, 0x1050}, {0, 1}},
        {0x107570, BII1<0x12>, {0, 0x1058}, {0, 1}},
        {0x107618, BI1<0x13>, {6}, {0}},
        {0x1076A0, BI1<0x14>, {6}, {0}},
        {0x107728, BII1<0x15>, {6, 0x1000}, {0, 1}},
        {0x1077D0, BIII1<0x16>, {6, 0, 6}, {0, 1, 2}},                   // 0x1E07D0
        {0x107888, BNone<0x17, 1>, {}, {}},
        {0x107900, BII1<0x18>, {6, 6}, {0, 1}},
        {0x1079A8, BII1<0x19>, {6, 6}, {0, 1}},
        {0x107A50, [](SCPT* ps, const ARGS& a) {                         // 0x1E08C0
             SCPTCMD* p = PcmdNew(ps, 0x1A, I(a, 1));
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->N(0x18) = I(a, 2);
         }, {6, 1, 0, 6}, {0, 2, 3, 1}},
        {0x107B18, [](SCPT* ps, const ARGS& a) { B1(ps, 0x1B, 0, {{0x10, I(a, 0)}}); }, {6}, {0}},
        {0x107BA0, [](SCPT* ps, const ARGS& a) { B1(ps, 0x1C, I(a, 2), {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 3)}}); },
         {6, 0x1011, 0, 6}, {0, 1, 2, 3}},                               // 0x1E0940
        {0x107C68, BII0<0x1D>, {6, 6}, {0, 1}},
        {0x107D10, [](SCPT* ps, const ARGS& a) { B1(ps, 0x1E, 0, {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 2)}}); },
         {6, 6, 6}, {0, 1, 2}},
        {0x107DC8, BII0<0x1F>, {6, 6}, {0, 1}},
        {0x107E70, BII0<0x20>, {6, 6}, {0, 1}},
        {0x107F18, BII0<0x21>, {6, 6}, {0, 1}},
        {0x107FC0, [](SCPT* ps, const ARGS& a) {
             B1(ps, 0x22, 0, {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 2)}, {0x1C, I(a, 3)}});
         }, {6, 6, 16, 6}, {0, 1, 2, 3}},
        {0x108088, [](SCPT* ps, const ARGS& a) {                         // 0x1E0B40
             SCPTCMD* p = PcmdNew(ps, 0x23, 0);
             p->G(0x10) = F(a, 0);
             p->N(0x14) = I(a, 0);
             p->N(0x18) = I(a, 1);
         }, {1, 6, 6}, {1, 2, 0}},
        {0x108140, [](SCPT* ps, const ARGS& a) {                         // 0x1E0B90
             SCPTCMD* p = PcmdNew(ps, 0x24, I(a, 1));
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->G(0x18) = F(a, 1);
             p->N(0x1C) = I(a, 2);
             p->N(0x20) = I(a, 3);
             p->N(0x24) = 1;
         }, {6, 1, 1, 0, 6, 6}, {0, 3, 4, 1, 2, 5}},
        {0x108230, [](SCPT* ps, const ARGS& a) {                         // 0x1E0C08
             SCPTCMD* p = PcmdNew(ps, 0x24, I(a, 1));
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->G(0x18) = F(a, 1);
             p->N(0x1C) = I(a, 2);
             p->N(0x20) = I(a, 3);
             p->N(0x24) = 0;
         }, {6, 1, 1, 0, 6, 6}, {0, 3, 4, 1, 2, 5}},
        {0x108320, BNone<0x25, 1>, {}, {}},
        {0x108398, BNone<0x26, 1>, {}, {}},
        {0x108410, [](SCPT* ps, const ARGS& a) {                         // 0x1E0CB8
             SCPTCMD* p = PcmdNew(ps, 0x27, 0);
             p->N(0x10) = I(a, 0);
             p->N(0x14) = I(a, 1);
             p->N(0x18) = I(a, 2);
             p->G(0x1C) = F(a, 0);
             p->N(0x20) = I(a, 3);
             p->N(0x24) = I(a, 4);
         }, {6, 0, 0, 1, 6, 6}, {0, 1, 2, 3, 4, 5}},
        {0x108500, [](SCPT* ps, const ARGS& a) {                         // 0x1E0D38
             SCPTCMD* p = PcmdNew(ps, 0x28, 0);
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->N(0x18) = I(a, 1);
             p->G(0x1C) = F(a, 1);
             p->N(0x20) = I(a, 2);
             p->N(0x24) = I(a, 3);
         }, {6, 1, 0, 1, 6, 6}, {0, 2, 4, 1, 3, 5}},
        {0x1085F0, [](SCPT* ps, const ARGS& a) {                         // 0x1E0DB8
             SCPTCMD* p = PcmdNew(ps, 0x29, I(a, 1));
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->G(0x18) = F(a, 1);
             p->N(0x1C) = I(a, 2);
         }, {6, 1, 1, 0, 6}, {0, 1, 2, 3, 4}},
        {0x1086D0, [](SCPT* ps, const ARGS& a) {                         // 0x1E0E18
             B1(ps, 0x2A, I(a, 3), {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 2)}, {0x1C, I(a, 4)}});
         }, {6, 0x102E, 0x102F, 0, 6}, {0, 1, 2, 3, 4}},
        {0x1087B0, [](SCPT* ps, const ARGS& a) {                         // 0x1E0E78
             SCPTCMD* p = PcmdNew(ps, 0x2B, I(a, 1));
             p->N(0x10) = I(a, 0);
             p->G(0x14) = F(a, 0);
             p->G(0x18) = F(a, 1);
             p->N(0x1C) = I(a, 2);
         }, {6, 1, 1, 0, 6}, {0, 1, 2, 3, 4}},
        {0x108890, BIFb<0x2C>, {6, 1, 0}, {0, 2, 1}},                    // 0x1E0ED8
        {0x108948, BIFb<0x2D>, {6, 1, 0}, {0, 2, 1}},                    // 0x1E0F18
        {0x108A00, BII1<0x2E>, {6, 6}, {0, 1}},
        {0x108AA8, BII1<0x2F>, {6, 6}, {0, 1}},
        {0x108B50, BII1<0x30>, {6, 6}, {0, 1}},
        {0x108BF8, BII1<0x31>, {6, 6}, {0, 1}},
        {0x108CA0, BIb<0x32>, {0x1004, 0}, {0, 1}},                      // 0x1E1058
        {0x108D48, [](SCPT* ps, const ARGS& a) {                         // 0x1E1088
             SCPTCMD* p = PcmdNew(ps, 0x33, I(a, 1));
             p->G(0x10) = F(a, 0);
             p->N(0x14) = I(a, 0);
         }, {1, 0x1003, 0}, {1, 2, 0}},
        {0x108E00, [](SCPT* ps, const ARGS& a) {                         // 0x1E10C8
             SCPTCMD* p = PcmdNew(ps, 0x34, 1);
             p->N(0x10) = I(a, 0);
             p->N(0x14) = I(a, 1);
             p->N(0x18) = I(a, 2);
             p->G(0x1C) = F(a, 0);
         }, {6, 6, 6, 1}, {0, 1, 2, 3}},
        {0x108EC8, [](SCPT* ps, const ARGS& a) {                         // 0x1E1128
             SCPTCMD* p = PcmdNew(ps, 0x35, 1);
             p->N(0x10) = I(a, 0);
             p->N(0x14) = I(a, 1);
             p->G(0x18) = F(a, 0);
         }, {6, 6, 1}, {0, 1, 2}},
        {0x108F80, [](SCPT* ps, const ARGS& a) {                         // 0x1E1178
             SCPTCMD* p = PcmdNew(ps, 0x36, 1);
             for (int i = 0; i < 4; ++i) p->N(0x10 + 4 * static_cast<uint32_t>(i)) = I(a, static_cast<size_t>(i));
             for (int i = 0; i < 3; ++i) p->G(0x20 + 4 * static_cast<uint32_t>(i)) = F(a, static_cast<size_t>(i));
         }, {0xE, 6, 5, 5, 1, 1, 1}, {0, 1, 2, 3, 4, 5, 6}},
        {0x109088, BI1<0x37>, {0}, {0}},
        {0x109110, [](SCPT* ps, const ARGS& a) {                         // 0x1E1238
             SCPTCMD* p;
             BVecCmd(ps, 0x38, a, &p);
             p->G(0x30) = F(a, 0);
             p->G(0x40) = F(a, 1);
             p->N(0x44) = I(a, 2);
         }, {2, 2, 1, 1, 0x106D}, {0, 1, 2, 3, 4}},
        {0x1091F0, [](SCPT* ps, const ARGS& a) {                         // 0x1E12E0
             SCPTCMD* p;
             BVecCmd(ps, 0x39, a, &p);
             p->G(0x30) = F(a, 0);
             p->N(0x40) = I(a, 2);
             p->N(0x44) = I(a, 3);
         }, {2, 2, 1, 0, 0}, {0, 1, 2, 3, 4}},
        {0x1092D0, BI1<0x3A>, {0}, {0}},
        {0x109350, [](SCPT* ps, const ARGS& a) { B1(ps, 0x3B, I(a, 3), {{0x10, I(a, 0)}, {0x14, I(a, 1)}, {0x18, I(a, 2)}}); },
         {6, 6, 6, 0}, {0, 1, 2, 3}},
        {0x109418, [](SCPT* ps, const ARGS& a) { B1(ps, 0x3C, I(a, 0), {}); }, {0}, {0}},
        {0x109498, BIb<0x3D>, {6, 0}, {0, 1}},
        {0x109540, BIb<0x3E>, {6, 0}, {0, 1}},
        {0x1095E8, [](SCPT* ps, const ARGS& a) {                         // 0x1E1488
             SCPTCMD* p;
             BVecCmd(ps, 0x3F, a, &p);
             p->G(0x30) = F(a, 0);
         }, {2, 2, 1}, {0, 1, 2}},
        {0x1096A0, [](SCPT* ps, const ARGS& a) { PcmdNew(ps, 0x40, I(a, 0))->G(0x10) = F(a, 0); }, {1, 0}, {1, 0}},
        {0x109748, BNone<0x41, 1>, {}, {}},
        {0x1097C0, [](SCPT* ps, const ARGS& a) {                         // 0x1E1560
             const int n = I(a, 0);
             B1(ps, 0x42, 0, {{0x10, n == -1 ? int(BtnMenuConfirm()) : n}, {0x14, I(a, 1)}});   // (Japan: Circle)
         }, {0x103C, 6}, {0, 1}},
        {0x109868, BII0<0x43>, {0x1057, 6}, {0, 1}},
        {0x109910, BNone<0x44, 0>, {}, {}},
        {0x109988, BIIb<0x45>, {16, 0x105D, 0}, {0, 1, 2}},              // 0x1E1610
        {0x109A40, BNone<0x46, 1>, {}, {}},
        {0x109AB8, BIII1<0x47>, {6, 0x1066, 0}, {0, 1, 2}},
        {0x109B70, BIII1<0x48>, {6, 0x1066, 0}, {0, 1, 2}},
        {0x109C28, BI1<0x49>, {0x1059}, {0}},                            // 0x1E1710 (the dance's state)
        {0x109CB0, [](SCPT* ps, const ARGS& a) {                         // 0x1E1740 (a dance beat: time, button, move, dancer)
             SCPTCMD* p = PcmdNew(ps, 0x4A, 0);
             p->G(0x10) = F(a, 0);
             p->N(0x14) = I(a, 0);
             p->N(0x18) = I(a, 1);
             p->N(0x1C) = I(a, 2);
         }, {1, 0x103C, 6, 6}, {0, 1, 2, 3}},
        {0x109D78, BII1<0x4B>, {6, 6}, {0, 1}},                          // 0x1E17A0 (PlayAsegDefault: ASEG, dancer)
        {0x109E20, BI1<0x4C>, {0x13}, {0}},                              // 0x1E17E0 (StartMusic)
        {0x109EA8, BI1<0x4D>, {0x13}, {0}},                              // 0x1E1810 (StopMusic)
        {0x109F30, [](SCPT* ps, const ARGS& a) { PcmdNew(ps, 0x4E, 0)->G(0x10) = F(a, 0); }, {1}, {0}},  // 0x1E1840 (WaitTimer)
        {0x109FB8, [](SCPT* ps, const ARGS& a) {                         // 0x1E1870
             SCPTCMD* p = PcmdNew(ps, 0x4F, I(a, 5));
             p->N(0x14) = I(a, 0);
             p->N(0x1C) = I(a, 1);
             p->N(0x18) = I(a, 2);
             p->N(0x20) = I(a, 3);
             p->N(0x10) = I(a, 4);
         }, {6, 6, 0x1061, 0, 6, 0}, {0, 1, 2, 3, 4, 5}},
        // (audit 2026-11 P1: the builders 0x1E18E0 / 0x1E1910 / 0x1E19B0 / 0x1E1A60 were missing, so these commands
        // were dropped from the authored scripts: w12's 2151 / 97639 (0x50), 2244 / 2250 / 2256 (0x54), w32's 2343 (0x57))
        {0x10A0A8, [](SCPT* ps, const ARGS& a) { B1(ps, 0x50, 0, {{0x10, I(a, 0)}}); }, {6}, {0}},  // 0x1E18E0
        {0x10A130, [](SCPT* ps, const ARGS& a) { B1(ps, 0x51, 0, {{0x10, I(a, 0)}}); }, {6}, {0}},  // 0x1E1910
        {0x10A1B8, [](SCPT* ps, const ARGS& a) { PcmdNew(ps, 0x52, 0)->G(0x10) = F(a, 0); }, {1}, {0}},  // 0x1E1940
        {0x10A240, BIFb<0x53>, {16, 1, 0}, {0, 2, 1}},                   // 0x1E1970
        {0x10A2F8, [](SCPT* ps, const ARGS& a) { B1(ps, 0x54, 1, {{0x10, I(a, 0)}}); }, {0x10}, {0}},  // 0x1E19B0
        {0x10A380, BIIb<0x55>, {6, 6, 0}, {0, 1, 2}},                    // 0x1E19E0
        {0x10A438, BII1<0x56>, {6, 6}, {0, 1}},                          // 0x1E1A20
        {0x10A4E0, [](SCPT* ps, const ARGS& a) { B1(ps, 0x57, 1, {{0x10, I(a, 0)}}); }, {6}, {0}},  // 0x1E1A60
        // (w32's 2343 only, at the battery: Bentley's partner jump onto Murray, Murray's aim and throw (R54))
        {0x10A568, BIIb<0x58>, {6, 6, 0}, {0, 1, 2}},                    // 0x1E1A90
        {0x10A620, BIIb<0x59>, {6, 6, 0}, {0, 1, 2}},                    // 0x1E1AD0
        {0x10A6D8, BIIb<0x5A>, {6, 6, 0}, {0, 1, 2}},                    // 0x1E1B10 (+0x18 0)
    };
    return s_a;
}

constexpr size_t c_cthunk = 92;

// The wrapper of a builder: the arguments converted by OTYP, passed in the call's order.
CRef RefThunk(const THUNK& t, LO* plo, const std::vector<CRef>& aref)
{
    SCPT* ps = PscptOf(plo);
    if (!ps) return CRef(TAGK_Void);
    ARGS a;
    for (int i : t.aorder)
    {
        const size_t j = static_cast<size_t>(i);
        const int otyp = t.aotyp[j];
        if (otyp == 1) a.ag.push_back(GArg(aref, j));
        else if (otyp == 2)
        {
            glm::vec4 v(0.0f);
            if (j < aref.size() && aref[j].m_tagk != TAGK_Nil) v = glm::vec4(aref[j].GetVector(), 0.0f);
            a.aq.push_back(v);
            a.ai.push_back(SLOT{0});
        }
        else if (otyp == 0x8006) a.an = AnArgList(aref, j);
        else a.ai.push_back(SLOT{NArg(aref, j)});
    }
    t.pfnBuild(ps, a);
    return CRef(TAGK_Void);
}

template <size_t I_>
CRef RefThunkN(LO* plo, const std::vector<CRef>& aref)
{
    return RefThunk(Athunk()[I_], plo, aref);
}

template <size_t... Is>
void RegisterThunks(std::index_sequence<Is...>)
{
    (RegisterObjectMethod(Athunk()[Is].pfn, RefThunkN<Is>), ...);
}

struct INITSCPT
{
    INITSCPT()
    {
        RegisterThunks(std::make_index_sequence<c_cthunk>{});
        if (Athunk().size() != c_cthunk) std::abort();
        RegisterLoUpdate(0x1DFC50, UpdateScpt);
        g_pfnStartScpt = [](LO* plo, int) { StartScpt(plo, nullptr); };
        // the script's own methods (EOPIDs 613..623)
        RegisterObjectMethod(0x106688, [](LO* plo, const std::vector<CRef>& a) {  // 0x1DFD58 (start with an object)
            StartScpt(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106710, [](LO* plo, const std::vector<CRef>&) {  // 0x1DFE80 RefreshWorldReference
            StartScpt(plo, PloPoCurrent(g_psw));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106788, [](LO* plo, const std::vector<CRef>&) {  // 0x1DFEB8
            StopScpt(plo);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106800, [](LO* plo, const std::vector<CRef>& a) {  // 0x1E0020
            if (SCPT* ps = PscptOf(plo)) EnsureAlarm(ps, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106888, [](LO* plo, const std::vector<CRef>& a) {  // 0x1E0070
            if (SCPT* ps = PscptOf(plo)) RemoveAlarm(ps, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106910, [](LO* plo, const std::vector<CRef>& a) {  // 0x1E00E8
            CRef r;
            SCPT* ps = PscptOf(plo);
            r.SetBool(ps && FHasAlarm(ps, NArg(a, 0)));
            return r;
        });
        RegisterObjectMethod(0x106990, [](LO* plo, const std::vector<CRef>&) {  // 0x1E0120
            if (SCPT* ps = PscptOf(plo)) ps->analarm.clear();
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x106A08, [](LO* plo, const std::vector<CRef>&) {  // 0x1DFFE0 (the commands cleared)
            SCPT* ps = PscptOf(plo);
            if (!ps) return CRef(TAGK_Void);
            StopScpt(plo);
            ps->acmd.clear();
            PcmdNew(ps, 0, 1)->N(0x10) = -1;
            ps->fDirty = 1;
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter(620, [](LO* plo) {  // +0x6C (the object that started it)
            CRef r;
            SCPT* ps = PscptOf(plo);
            if (ps && ps->ploStarter) r.SetBasic(ps->ploStarter);
            return r;
        });
        RegisterObjectGetter(616, [](LO* plo) {  // 0x1DFFD8
            CRef r;
            r.SetBool(FScptRunning(plo));
            return r;
        });
        RegisterObjectSetter(622, [](LO* plo, const CRef& ref) {  // 0x1E01C8 (the commands' capacity)
            SCPT* ps = PscptOf(plo);
            const int n = ref.m_tagk == TAGK_S32 ? ref.m_n : static_cast<int>(ref.m_g);
            if (ps && n > 0 && n < 0x200 && ps->acmd.size() < 2)
            {
                ps->cMax = n;
                ps->f80 = 1;
            }
        });
        RegisterObjectGetter(624, [](LO* plo) {  // 0x1E0128: the event runner's lookup context (+0x18)
            CRef r;
            SCPT* ps = PscptOf(plo);
            RUN* p = ps ? ps->prun68 : nullptr;
            if (p && p->icCur >= 0 && ps->acmd[static_cast<size_t>(p->icCur)].kind == 9 && p->ploCtx)
                r.SetBasic(p->ploCtx);
            return r;
        });
        RegisterObjectSetter(624, [](LO* plo, const CRef& ref) {  // 0x1E0158
            SCPT* ps = PscptOf(plo);
            RUN* p = ps ? ps->prun68 : nullptr;
            if (p && p->icCur >= 0 && ps->acmd[static_cast<size_t>(p->icCur)].kind == 9)
                p->ploCtx = dynamic_cast<LO*>(ref.m_pbasic);
        });
    }
} s_initscpt;
}  // namespace

// orig: 0x001DFB10 (and 0x001DFAD8's -1 capacity): the buffer, the opening command (-1: no object)
void BeginScptLoad(LO* plo)
{
    SCPT& s = MpScpt()[plo];
    s = SCPT{};
    PcmdNew(&s, 0, 1)->N(0x10) = -1;  // 0x1E0290
    s.fDirty = 1;
}

// orig: 0x001DFBF8 (vt slot 41, a clone's: the copied object (0x1A6730) owns a copy of the commands (0x1AE238 of +0x54 x 0x50))
void CloneScptData(LO* ploSrc, LO* ploDst)
{
    SCPT* ps = PscptOf(ploSrc);
    if (!ps || ploSrc == ploDst) return;
    SCPT& d = MpScpt()[ploDst];
    d = SCPT{};
    d.acmd = ps->acmd;
    d.cMax = ps->cMax;
    d.fDirty = ps->fDirty;
    d.f80 = ps->f80;
    d.analarm = ps->analarm;
    d.fRealTime = ps->fRealTime;
}

void EndScptLoad(LO* plo)
{
    SCPT* ps = PscptOf(plo);
    if (!ps) return;
    if (std::getenv("SLY2N_SCPTDIALOG"))
    {
        int c = 0, idFirst = -1;
        for (SCPTCMD& cmd : ps->acmd)
            if (cmd.kind == 0x45 && c++ == 0) idFirst = cmd.N(0x10);
        if (c) std::printf("scpt %d: %d dialog lines, the first %d\n", plo->oid, c, idFirst);
        for (SCPTCMD& cmd : ps->acmd)
            if (cmd.kind == 0x0B) std::printf("scpt %d: starts script %d\n", plo->oid, cmd.N(0x10));
    }
    // (debug: SLY2N_SCPTAUDIT=1 prints every script's commands at load)
    if (std::getenv("SLY2N_SCPTAUDIT"))
        for (size_t i = 0; i < ps->acmd.size(); ++i)
        {
            std::printf("scptaudit %d %zu %X %d", plo->oid, i, ps->acmd[i].kind, ps->acmd[i].b);
            for (const SLOT& a : ps->acmd[i].a) std::printf(" %d", a.n);
            std::printf("\n");
        }
    // debug: SLY2N_SCPTFIND=n prints the scripts whose commands carry the word n
    // (debug: SLY2N_SCPTKIND=K lists the scripts carrying commands of kind K)
    if (const char* sz = std::getenv("SLY2N_SCPTKIND"))
        for (size_t i = 0; i < ps->acmd.size(); ++i)
            if (ps->acmd[i].kind == static_cast<int>(std::strtol(sz, nullptr, 0)))
                std::printf("scpt %d: command %zu kind 0x%X [%d %d %d %d %d]\n", plo->oid, i, ps->acmd[i].kind, ps->acmd[i].a[0].n,
                            ps->acmd[i].a[1].n, ps->acmd[i].a[2].n, ps->acmd[i].a[3].n, ps->acmd[i].a[4].n);
    if (const char* sz = std::getenv("SLY2N_SCPTFIND"))
        for (size_t i = 0; i < ps->acmd.size(); ++i)
            for (int k = 0; k < 16; ++k)
                if (ps->acmd[i].a[k].n == std::atoi(sz))
                    std::printf("scpt %d: command %zu kind 0x%X carries %s\n", plo->oid, i, ps->acmd[i].kind, sz);
    ps->cMax = std::max(static_cast<int>(ps->acmd.size()), ps->cMax) + 1;
    ps->acmd.reserve(static_cast<size_t>(ps->cMax));
}

int CScptSmaCommands(LO* pscpt, int (*an)[3], int cMax)
{
    SCPT* ps = pscpt ? PscptOf(pscpt) : nullptr;
    if (!ps) return 0;
    int c = 0;
    for (SCPTCMD& cmd : ps->acmd)
        if (cmd.kind == 0x0D && c < cMax)
        {
            an[c][0] = cmd.N(0x10), an[c][1] = cmd.N(0x14), an[c][2] = cmd.N(0x18);
            ++c;
        }
    return c;
}

int CScptWalkTargets(LO* pscpt, int* aoid, int cMax)
{
    SCPT* ps = pscpt ? PscptOf(pscpt) : nullptr;
    if (!ps) return 0;
    int c = 0;
    for (SCPTCMD& cmd : ps->acmd)
        if (cmd.kind == 0x24 && c < cMax) aoid[c++] = cmd.N(0x10);
    return c;
}

void StartScpt(LO* pscpt, LO* ploStarter)
{
    LO* plo = pscpt;
    if (SCPT* ps = PscptOf(plo))
    {
        // debug: SLY2N_SCPTDUMP=oid prints the script's commands
        if (const char* sz = std::getenv("SLY2N_SCPTDUMP"); sz && std::atoi(sz) == plo->oid)
            for (size_t i = 0; i < ps->acmd.size(); ++i)
                std::printf("scpt %d: %zu kind 0x%X b %d [%d %d %d %d %d]\n", plo->oid, i, ps->acmd[i].kind, ps->acmd[i].b,
                            ps->acmd[i].a[0].n, ps->acmd[i].a[1].n, ps->acmd[i].a[2].n, ps->acmd[i].a[3].n, ps->acmd[i].a[4].n);
    }
    StartScptImpl(pscpt, ploStarter);
}
void StopScpt(LO* pscpt) { StopScptImpl(pscpt); }
// orig: 0x001E0020 (an alarm set on the script, e.g. the vault's 0x655)
void EnsureScptAlarm(LO* pscpt, int n)
{
    if (SCPT* ps = PscptOf(pscpt)) EnsureAlarm(ps, n);
}

bool FScptRunning(LO* pscpt)
{
    SCPT* ps = PscptOf(pscpt);
    return ps && ps->fRunning;
}

void ResetScpts() { MpScpt().clear(); }

uint64_t IdScptCutscene()
{
    uint64_t id = 0;
    for (const auto& [plo, s] : MpScpt())
        if (s.fRunning && s.idSkipScene && (!id || s.idSkipScene < id) &&
            std::any_of(s.arun.begin(), s.arun.end(), [](const PRUN& r) { return !r->fRetired && r->fPause; }))
            id = s.idSkipScene;
    return id;
}

bool FScptActorGoingTo(const void* pv)
{
    if (!pv || !MpRunLive().count(pv)) return false;
    const auto* p = static_cast<const RUN*>(pv);
    if (p->fRetired || p->icPending < 0) return false;
    const SCPT* ps = PscptOf(p->pscptLo);
    const auto i = static_cast<size_t>(p->icPending);
    return ps && i < ps->acmd.size() && ps->acmd[i].kind == 0x24;
}

// orig: 0x001E46A0
void DriveScptActor(void* pv, PADCTL* out)
{
    if (!pv || !MpRunLive().count(pv)) return;
    RUN* p = static_cast<RUN*>(pv);
    SCPT* ps = PscptOf(p->pscptLo);
    if (std::getenv("SLY2N_DRIVEWHY"))
        std::printf("drive run: ps %p retired %d pending %d watch %d\n", (void*)ps, ps ? p->fRetired : -1, p->icPending, p->ploWatch ? p->ploWatch->oid : -1);
    if (!ps || !out || p->fRetired || p->icPending < 0) return;
    SCPTCMD* pc = &ps->acmd[static_cast<size_t>(p->icPending)];
    ACTOR* po = PactorFromLo(p->ploWatch);
    if (!po) return;
    switch (pc->kind)
    {
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22:
        out->uJoy = 0.0f;
        if (po->nSub4CC != -1) out->grfbtn |= 0x20;
        break;
    case 0x23:
        if (LO* pt = PloFind(p, pc->N(0x14)))
        {
            float rad;
            GetRunAim(p, pt, nullptr, &rad);
            SetStepPhysHeading(po->palo, rad);  // 0x204660
            out->radJoy = rad;
        }
        out->uJoy = 0.0f;
        if (po->nSub4CC != -1) out->grfbtn |= 0x20;
        break;
    case 0x1A:
        // along the pipe toward +0x7C: its tangent at the actor (the points 50 either way) in the stick's frame
        // (0x1C4768: the controller's camera matrix), turned round when the point lies behind; full deflection
        if (const CRV* pcrv = po->pcrvF50)
        {
            glm::vec3 a, b;
            EvalCrvS(pcrv, SWrapCrv(pcrv, po->sCrvF50 + 50.0f), &a, nullptr);  // (0x1E6728: wrapped / clamped)
            EvalCrvS(pcrv, SWrapCrv(pcrv, po->sCrvF50 - 50.0f), &b, nullptr);
            // 0x1C4768: the vector in the camera frame (forward, left, up) as the stick's (-left, up), normalised
            const glm::vec3 cc = glm::transpose(out->matCam) * (a - b);
            const float l = std::sqrt(cc.y * cc.y + cc.z * cc.z);
            float x = l > 0.0f ? -cc.y / l : 0.0f, y = l > 0.0f ? cc.z / l : 0.0f;
            bool fBehind = false;
            const float gCur = po->sCrvF50, gTarget = p->s7C;
            if (pcrv->fClosed)
            {
                const float len = SLengthCrv(pcrv);
                const float da = gTarget < gCur ? gCur - gTarget : (len + gCur) - gTarget;
                const float db = gTarget < gCur ? (len + gTarget) - gCur : gTarget - gCur;
                fBehind = da < db;
            }
            else fBehind = gTarget < gCur;
            if (fBehind) { x = -x; y = -y; }
            out->x = x;
            out->y = y;
            out->radJoy = std::atan2(y, x);
            out->uJoy = 1.0f;
        }
        break;
    case 0x24:
    {
        po->palo->tScptRtz = g_clock.t;  // (native, UE6: its position integrates rounding toward zero, xf.cpp)
        out->uJoy = std::fabs(pc->G(0x14));
        if (pc->N(0x24)) out->grfbtn |= 8;
        if (LO* pt = PloFind(p, pc->N(0x10)))
        {
            if (po->nSub4CC != -1 && po->n4C8 == 3) WarnOnce("the drive along a rail (0x1E67B8)");
            glm::vec3 pos;
            GetRunAim(p, pt, &pos, nullptr);
            const glm::vec3 q = PosAloCenter(po->palo);
            const float dx = pos.x - q.x, dy = pos.y - q.y;
            out->radJoy = std::atan2(dy, dx);  // 0x2458F0
            static const bool s_fDbg = std::getenv("SLY2N_DRIVEPRINT") != nullptr;
            if (s_fDbg)
                std::printf("drive 0x24 %d -> %d: aim %.1f %.1f %.1f from %.1f %.1f %.1f heading %.2f bearing %.2f (jump kind of its state %d)\n",
                            po->palo->oid, pt->oid, pos.x, pos.y, pos.z, q.x, q.y, q.z, po->radHeading, out->radJoy, ActorJumpKind(po));
            const float d = 0.0f + std::sqrt(dx * dx + dy * dy);
            float sc = 0.25f + d * (0.01f + d * 0.0f);  // 0x2C3630, clamped by 0x2BBB60
            sc = sc < 0.0f ? 0.0f : (1.0f < sc ? 1.0f : sc);
            out->uJoy *= sc;
        }
        if (po->nSub4CC != -1 && (po->n4C8 == 0 || po->n4C8 == 0xD)) out->grfbtn |= 0x20;
        break;
    }
    case 0x58:
        // Bentley's partner jump: the stick at rest, 0x20 pressed now (0x1C45A8)
        out->uJoy = 0.0f;
        out->grfbtn |= 0x20;
        StampPadctl(out);
        break;
    case 0x5A:
    {
        // Murray's throw: 0x40 pressed once his aim point (+0x19F0) is at its end (+0x1A20), 0.1 on each axis
        if (MURRAY* pm = PmurrayFromLo(po->palo))
        {
            const JTTHROW& t = pm->jtthrow1910;
            if (FFloatsNear(t.transitionEnd.x, t.targetWorld.x, 0.1f) && FFloatsNear(t.transitionEnd.y, t.targetWorld.y, 0.1f) &&
                FFloatsNear(t.transitionEnd.z, t.targetWorld.z, 0.1f))
                out->grfbtn |= 0x40;
        }
        out->uJoy = 0.0f;
        StampPadctl(out);
        break;
    }
    case 0x2A: out->uJoy = 0.0f; break;
    // Bentley's bomb (e.g. w12's 97639 at the Rip-off the Ruby gate, cmd 7): TRIANGLE held from here, so his ability
    // starts (state 6, the bomb's summon and hold) and the command's wait (0x1E3890: his state 6) completes; 0x51 lets go
    case 0x50: p->fBlocked = 1; break;
    case 0x51: p->fBlocked = 0; break;
    case 0x29:
    {
        // the speed +0x14; while in kind 8, toward the target in the actor's own frame (0x1A6FC0 from
        // the world): the stick (-y, x) of the unit direction
        out->uJoy = std::fabs(pc->G(0x14));
        LO* pt = PloFind(p, pc->N(0x10));
        if (!pt || po->nSub4CC == -1 || po->n4C8 != 8) break;
        glm::vec3 pos;
        GetRunAim(p, pt, &pos, nullptr);  // 0x1E4D40
        glm::vec3 d = pos - PosLoWorld(po->palo);
        d.z = 0.0f;
        const glm::mat3 m(MatJtLo(po->palo));
        glm::vec3 l = glm::transpose(m) * d;
        const float len = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
        l = len < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : l / len;
        out->y = l.x * out->uJoy;
        out->x = -l.y * out->uJoy;
        break;
    }
    default:
    {
        char sz[64];
        std::snprintf(sz, sizeof sz, "the drive of command 0x%X (0x1E46A0)", pc->kind);
        WarnOnce(sz);
        break;
    }
    }
    if (p->fBlocked)
    {
        out->grfbtn |= 0x10;  // TRIANGLE
        if (pc->kind == 0x50) StampPadctl(out);  // 0x1C45A8: its press time
    }
}

// (for the binocucom's slots 83 / 84, binoc.cpp: 0x143EE0 / 0x143FF8)
void PlayActorScriptAnimLo(LO* ploActor, LO* plo, int oid, int grfapl) { PlayActorScriptAnim(ploActor, plo, oid, grfapl, 5); }

// (co-op, coop.h) the other game's script started here (fPlayer: started by that game's player: this game's starts it)
bool FCoopApplyScptStart(SW* psw, int oid, int iclone, int oidStarter, bool fPlayer)
{
    if (!psw) return false;
    LO* pscpt = nullptr;
    LO* ploStarter = fPlayer ? PloPoCurrent(psw) : nullptr;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo) continue;
        if (!pscpt && plo->oid == oid && plo->iclone == iclone && PscptOf(plo)) pscpt = plo;
        if (!fPlayer && !ploStarter && oidStarter >= 0 && plo->oid == oidStarter) ploStarter = plo;
    }
    SCPT* ps = pscpt ? PscptOf(pscpt) : nullptr;
    if (!ps || ps->fRunning) return false;
    StartScptImpl(pscpt, ploStarter);
    return true;
}
