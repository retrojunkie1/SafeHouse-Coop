#include <execinfo.h>
// po.cpp: the player objects (po.h): the controller states (ported from the reference's m_1C3D70.cpp, oracle-checked
// ports of the original's 0x1C3D70..0x1C4B00), the enable / current player (the world code) and the safe house's
// slot 79 (0x1D7418 / 0x1D7480).
#include "po.h"
#include "actioninput.h"
#include "rope.h"
#include "turret.h"
#include "vehiclehooks.h"
#include "actor.h"
#include "aipursuit.h"
#include "ai.h"
#include "booty.h"
void StartPlayerCounters(ACTOR* pa, bool fReset);  // sly.cpp: orig 0x1440D0
#include "so.h"
#include <cstring>
#include <cstdlib>
#include "proc.h"
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
#include "frame.h"

#include "activate.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cmworld.h"
#include "joy.h"
#include "jt.h"
#include "lo.h"
#include "objmethod.h"
#include "ref.h"
#include "savedata.h"
#include "scpt.h"
#include "spliceobj.h"
#include "cplcy.h"
#include "smrun.h"
#include "asega.h"
#include "gang.h"
#include "sly.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "frontend.h"
#include "menu.h"
#include "shop.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>

using cmm::F32;

namespace
{
std::map<LO*, PODATA> s_mppodata;
std::list<std::unique_ptr<PADCTL>> s_lpadctl;  // SW +0x3EAC (appended: 0x11A070)
std::vector<LO*> s_apo;                        // the game state's +0x3EBC (count +0x3EB8, at most 16)
LO* s_ploCur = nullptr;                        // +0x3EFC
LO* s_ploOverride = nullptr;                   // +0x3F00

constexpr float HALF_PI = F32(0x3FC90FDB);
const float G_NEG_FLT_MAX = -FLT_MAX;          // 0x2A76F0

// orig: 0x001C4330 (a stick (x, y) to the camera-relative heading and deflection; the deflection held decays by 0.2 a
// frame, the old heading kept while the stick is inside it)
void TurnPadctlStick(float radCam, float* px, float* py, float* prad, float* pr, float* prHeld)
{
    const float x = *px, y = *py;
    *pr = std::sqrt(x * x + y * y);
    const float r2 = *prHeld - 0.2f;
    *prHeld = r2 > 0.0f ? r2 : 0.0f;
    if (*pr < *prHeld || *pr == 0.0f)
    {
        float s, c;
        cmm::CalculateSinCos(*prad - radCam + HALF_PI, &s, &c);
        *pr = *prHeld;
        *px = *prHeld * c;
        *py = *pr * s;
    }
    else
    {
        *prad = cmm::RadNormalize((std::atan2(*py, *px) + radCam) - HALF_PI);
        *prHeld = *pr;
    }
}

// orig: 0x001C45A8 (the press times of the new buttons, the sticks' last rest / movement)
void StampPadctlState(PADCTL* ppc)
{
    const float t = g_clock.t;
    uint32_t grf = ppc->grfbtn & ~ppc->grfbtnPrev;
    ppc->grfbtnPrev = ppc->grfbtn;
    for (int i = 0; grf; ++i, grf >>= 1)
        if (grf & 1) ppc->atPressed[i] = t;
    ppc->atStick[0.0f < ppc->uJoy ? 1 : 0] = t;
    ppc->atStick[2 + (0.0f < ppc->uJoy2 ? 1 : 0)] = t;
}
}  // namespace

void StampPadctl(PADCTL* ppc) { StampPadctlState(ppc); }  // orig: 0x001C45A8

namespace
{
// The safe house (cid 0x46): +0x180 its state, +0x184 its state machine (option 1337), +0x188 the handle,
// +0x220 / +0x224 the states it may show (TODO: their option)
struct SAFEHOUSE
{
    int nState = 0;
    SMA* psma = nullptr;
    bool fAllowedSet = false;   // +0x220 set by the method 1339 (else the option's list)
    std::vector<OID> aoidAllowed;   // +0x220 / +0x224 (at most 6)
    bool fMurrayQol = false;        // (QoL U33: Murray added in Episode 4, see RefSetSafeHouseStates)
};
std::unordered_map<const LO*, SAFEHOUSE> s_mpsh;

// QoL (SafeHouse-Coop): the walkable safe house. On entering, the character the safe house shows takes the pad and
// walks the hideout; standing by a gang member or the ThiefNet laptop sets the safe house's choice to it (its banner
// shows "Press X to play as Bentley" / ThiefNet) and its button does the original action. SLY2_CLASSIC_SAFEHOUSE:
// the original menu.
bool FWalkableSafeHouse() { static const bool s_f = std::getenv("SLY2_CLASSIC_SAFEHOUSE") == nullptr; return s_f; }
struct WALKSH
{
    LO* plosh = nullptr;      // the safe house shown
    LO* ploActor = nullptr;   // the character walking it
    int phase = 0;            // 1: entered (the switch to the character next), 2: walking
    float tEnter = 0.0f;
    OID oidNear = -1;         // the choice stood by (-1 none)
};
WALKSH s_walk;
bool s_fWalkSwitch = false;   // (the character taking the pad: the safe house is not left)

// orig 0x001D77D0: the safe house's state machine to the next allowed state (+0x220 / +0x224: option 1339) from
// its current one, d steps along the machine's states (0: the current one if allowed, else the next)
std::vector<OID> AoidOptionStates(const LO* plo);
std::vector<OID> AoidAllowed(const LO* plo)
{
    if (auto it = s_mpsh.find(plo); it != s_mpsh.end() && it->second.fAllowedSet) return it->second.aoidAllowed;
    return AoidOptionStates(plo);
}
void StepSafeHouse(LO* plo, SAFEHOUSE& sh, int d)
{
    if (!sh.psma || !sh.psma->psm) return;
    SM* psm = sh.psma->psm;
    const int c = int(psm->asms.size());
    if (c <= 0) return;
    const std::vector<OID> aoid = AoidAllowed(plo);
    if (std::getenv("SLY2N_POPRINT")) { std::printf("safe house: step %d from %d, states", d, OidSmaCur(sh.psma)); for (const auto& s : psm->asms) std::printf(" %d", s.oid); std::printf("\n"); }
    int i = IsmsFindSm(psm, OidSmaCur(sh.psma));
    if (i < 0) i = 0;
    const int i0 = i;
    for (int guard = 0; guard < 2 * c + 2; ++guard)
    {
        i += d;
        if (i < 0) i = c - 1;
        else if (i >= c) i = 0;
        const OID oid = OidFromSmIsms(psm, i);
        if (std::find(aoid.begin(), aoid.end(), oid) != aoid.end())
        {
            SetSmaGoal(sh.psma, oid);
            return;
        }
        if (d == 0) d = 1;
        else if (i == i0) break;
    }
    ClearSmaHandle(&sh.psma);   // (0x1F1448(0, p, -1, 0, +0x188))
}

// orig 0x001D7780 (EOPID 1339's method 0x10F648; the loader's option and the scripts): the states the select may
// show (a list of at most 6 oids into +0x220 / +0x224), then 0x1D77D0(p, 0). This is how the original makes a
// character available: the Paris scripts give the list (10 7) (ThiefNet, Sly) until Bentley's / Murray's jobs open.
// (Paris: the hub's script gives (10 7) and conses 9 (Murray) when task 1878's state is 1+, 8 (Bentley) when task
// 1586's is; every hub has its own such script.)
bool s_fAllCharacters = false;   // debug (F6): every state of the option's list stays allowed
std::vector<OID> AoidOptionStates(const LO* plo)
{
    std::vector<OID> a;
    for (const OPT& o : plo->aopt)
        if (o.eopid == 1339)
            for (const OPTVAL& v : o.aval)
                for (const OPTVAL& w : v.alist) a.push_back(OID(w.n));
    return a;
}
CRef RefSetSafeHouseStates(LO* plo, const std::vector<CRef>& aref)
{
    SAFEHOUSE& sh = s_mpsh[plo];
    std::vector<int> an = AnArgList(aref, 0);
    if (an.size() > 6) an.resize(6);
    sh.aoidAllowed.assign(an.begin(), an.end());
    sh.fAllowedSet = true;
    // QoL DEVIATION FROM THE ORIGINAL (user request U33): Episode 4's hub script (object 2382, sidebag 1267) gives
    // (10 8) and conses Sly (7) once t4_chalktalk2 (2443) is done, but never adds Murray (9), although he is back
    // with the gang after the episode's final job (the heist 2534). Once its last task t4_heist7_complete (2601) is
    // done, Murray is allowed too. The original game leaves him out.
    if (IepisodeCur() == 4 && std::find(sh.aoidAllowed.begin(), sh.aoidAllowed.end(), 9) == sh.aoidAllowed.end() &&
        sh.aoidAllowed.size() < 6)
    {
        bool fDone = false;
        if (NTaskStateOid(2601, &fDone) != -2 && fDone)
        {
            sh.aoidAllowed.push_back(9);
            sh.fMurrayQol = true;
            // Murray's actor is loaded removed in this hub (the world file): put him in the world so the safe
            // house's SM seats him with the others.
            if (LO* plo9 = PloFindSwObjectByOid(g_psw, 9); plo9 && plo9->fRemoved) AddLo(g_psw, plo9);
        }
    }
    if (s_fAllCharacters)
        for (OID oid : AoidOptionStates(plo))
            if (std::find(sh.aoidAllowed.begin(), sh.aoidAllowed.end(), oid) == sh.aoidAllowed.end()) sh.aoidAllowed.push_back(oid);
    if (std::getenv("SLY2N_POPRINT"))
    {
        std::printf("safe house %d: states allowed", plo->oid);
        for (OID oid : sh.aoidAllowed) std::printf(" %d", oid);
        std::printf("\n");
    }
    StepSafeHouse(plo, sh, 0);
    return CRef(TAGK_Void);
}

void Event(LO* plo, int n)
{
    if (std::getenv("SLY2N_POPRINT"))
    {
        std::printf("safe house %d: event %d t %.3f\n", plo->oid, n, g_clock.t);
        if (CRef* pref = plo->pframe ? plo->pframe->PrefFindBinding(static_cast<SYMID>(n), 1) : nullptr; pref && pref->m_tagk == TAGK_Proc)
        {
            std::string str;
            CRef body;
            body.SetPair(pref->m_pproc->m_ppairCodeExpr);
            PrintRef(body, &str);
            std::printf("  %s\n", str.substr(0, 2000).c_str());
        }
    }
    HandleLoSpliceEvent(plo, n, false, 0, nullptr);
    if (n == 5 || n == 6) BroadcastTaskEvent(g_psw, n == 5 ? 0x1A : 0x1B, plo);  // 0x1A61E0
}

// orig 0x001D7480: the safe house shown (1) or left (0)
void SetSafeHouseState(SW* psw, LO* plo, int n)
{
    SAFEHOUSE& sh = s_mpsh[plo];
    if (sh.nState == n) return;
    if (sh.nState == 1)
    {
        ClearSmaHandle(&sh.psma);  // 0x1F1448(0, ...)
        Event(plo, 6);
    }
    sh.nState = n;
    if (n != 1)
    {
        if (s_walk.plosh == plo) s_walk = WALKSH{};
        return;
    }
    // a job running (and not +0x74) ends
    if (JOB* pjob = psw->tasks.pjobCur; pjob && !pjob->f74) EndJob(psw, pjob);  // 0x162E38
    // the players (0x15E9C8(G, 5, 0x34, 0, 3): the first three of class 0x34): a carried booty put back (0x1433C8:
    // the save's slot, the booty out, the HUD timer stopped), their health / power refilled (0x1440D0(p, 1))
    {
        std::vector<LO*> aplo = AploOfClass(psw, 0x34);
        if (aplo.size() > 3) aplo.resize(3);
        for (LO* p : aplo)
            if (ACTOR* pa = PactorFromLo(p))
            {
                ReturnActorBooty(pa);
                StartPlayerCounters(pa, true);
            }
    }
    // the cid 0x64 objects: vt slot 76 (4) (TODO); the cid 0x4F objects removed
    for (CID cid = 0x4F; cid < g_acls[0x4F].cidLast; ++cid)
        for (LO* p : std::vector<LO*>(psw->mpcidaplo[cid])) RemoveLo(psw, p);
    // its state machine at the state of the progress "oidPoCurrent" (else its first)
    OID oidSm = static_cast<OID>(plo->U32Field(0, 0x184, static_cast<uint32_t>(-1)));
    LO* plosm = oidSm != -1 ? PloFindSwObjectByOid(psw, oidSm) : nullptr;
    SM* psm = dynamic_cast<SM*>(plosm);
    OID oidState = -1;
    if (psm)
    {
        const CRef ref = RefGetProgress({"oidPoCurrent"});
        const int v = ref.m_tagk == TAGK_S32 ? ref.m_n : -1;
        oidState = IsmsFindSm(psm, v) == -1 ? OidFromSmIsms(psm, 0) : v;
    }
    if (std::getenv("SLY2N_POPRINT"))
        std::printf("safe house %d: sm %d (%s) state %d\n", plo->oid, oidSm, psm ? "found" : "none", oidState);
    if (FCidDerivedFrom(plo->Cid(), 0x30)) ReplaceSmaHandle(psm, static_cast<ALO*>(plo), oidState, 0, &sh.psma);
    StepSafeHouse(plo, sh, 0);   // orig 0x1D77D0(p, 0)
    ClearJoySteps();             // orig 0x187E78
    Event(plo, 5);
    if (FWalkableSafeHouse()) s_walk = WALKSH{plo, nullptr, 1, g_clock.t, -1};
}
}

PODATA* PpodataFromLo(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x32)) return nullptr;
    auto [it, fNew] = s_mppodata.try_emplace(plo);
    if (fNew) it->second.grfbtnForced = plo->U32Field(0, 0x154, 0);
    return &it->second;
}

// orig: 0x001C42E8 (the sticks, the buttons and their times cleared)
void ClearPadctl(PADCTL* ppc)
{
    ppc->grfbtn = 0;
    ppc->uJoy2 = ppc->radJoy2 = ppc->y2 = ppc->x2 = 0.0f;
    ppc->uJoy = ppc->radJoy = ppc->y = ppc->x = 0.0f;
    ForgetPadctlPresses(ppc, 0xFFFF);
}

// orig: 0x001C4468 (off: cleared; current: the pad's sticks and buttons (less the handled ones, plus the object's
// own +0x154), turned by the camera's pan; then the press times)
void UpdatePadctl(PADCTL* ppc)
{
    float radPan = 0.0f, radTilt = 0.0f;
    if (ppc->nControl == 0) ClearPadctl(ppc);
    else if (ppc->nControl == 1)
    {
        // (G_GRFJOYT & 2 ? the first pad : the second: the lock of 0x1870D0)
        const JOYPS2& j = JoyFor(2);
        ppc->x = j.x;
        ppc->y = j.y;
        ppc->x2 = j.x2;
        ppc->y2 = j.y2;
        const PODATA* ppd = PpodataFromLo(ppc->plo);
        // (+0x154, EOPID 889: the field itself, as the original reads it: the PODATA may be made before the loader
        // sets the option, e.g. Bentley's R1 (8) in Ep7's boss world 37, which keeps him from the target stance)
        if (ppd) const_cast<PODATA*>(ppd)->grfbtnForced = ppc->plo->U32Field(0, 0x154, ppd->grfbtnForced);
        ppc->grfbtn = (j.grfbtn & ~j.grfbtnHandled) | (ppd ? ppd->grfbtnForced : 0);
        ppc->grfbtn = sly2actions::HeldButtons(ppc, ppc->grfbtn, ppd ? ppd->grfbtnForced : 0);
        cmm::DecomposeRotateMatrixPanTilt(g_cm.mat, &radPan, &radTilt);
    }
    ppc->matCam = cmm::BuildPanTiltMatrix(radPan, radTilt);
    TurnPadctlStick(radPan, &ppc->x, &ppc->y, &ppc->radJoy, &ppc->uJoy, &ppc->uJoyDecay);
    TurnPadctlStick(radPan, &ppc->x2, &ppc->y2, &ppc->radJoy2, &ppc->uJoy2, &ppc->uJoyDecay2);
    // (debug: SLY2N_AUTOSTEER="X Y@tA-tB[,U];...": between the game times A and B the controlled player's left stick
    // points at the world point (X, Y) with deflection U (default 1): a player's stick in any camera)
    if (static const char* s_sz = std::getenv("SLY2N_AUTOSTEER"); s_sz && ppc->nControl == 1 && ppc->plo &&
        FCidDerivedFrom(ppc->plo->Cid(), 0x30))
        for (const char* q = s_sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            float x = 0, y = 0, a = 0, b = 0, u = 1.0f;
            if (std::sscanf(q, "%f %f@t%f-%f,%f", &x, &y, &a, &b, &u) < 4 || g_clock.t < a || b < g_clock.t) continue;
            const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(ppc->plo));
            ppc->radJoy = std::atan2(y - pos.y, x - pos.x);
            ppc->uJoy = u;
            ppc->uJoyDecay = u;
            break;
        }
    StampPadctl(ppc);
}

// orig: 0x001C4640
void SetPadctlControl(PADCTL* ppc, int n)
{
    ppc->nControl = n;
    if (n == 1) ppc->grfbtnPrev = 0xFFFF;
}

// orig: 0x001C4660 (the buttons of grf pressed within dt)
uint32_t GrfPadctlPressedWithin(const PADCTL* ppc, uint32_t grf, float dt)
{
    if (sly2actions::Applies(ppc)) return sly2actions::PressedButtons(ppc, grf, dt);
    uint32_t grfRet = 0;
    const float t = g_clock.t;
    for (int i = 0; grf; ++i, grf >>= 1)
        if ((grf & 1) && t - ppc->atPressed[i] <= dt) grfRet |= 1u << (i & 31);
    return grfRet;
}

// orig: 0x001C46C0 (and consumed)
uint32_t GrfPadctlTakePressed(PADCTL* ppc, uint32_t grf, float dt)
{
    const uint32_t r = GrfPadctlPressedWithin(ppc, grf, dt);
    ForgetPadctlPresses(ppc, r);
    return r;
}

// orig: 0x001C4738
void ForgetPadctlPresses(PADCTL* ppc, uint32_t grf)
{
    if (sly2actions::Applies(ppc)) sly2actions::ForgetButtons(ppc, grf);
    for (int i = 0; grf; ++i, grf >>= 1)
        if (grf & 1) ppc->atPressed[i] = G_NEG_FLT_MAX;
}

void UpdateAllPadctls()
{
    // (debug: SLY2N_SETPO="OID@T[;OID@T...]" makes the player object OID the current player at game time T, as the
    // safe house's choice does: added to the world (vt slot 8) and enabled (slot 79); test harness only)
    if (static const char* s_sz = std::getenv("SLY2N_SETPO"); s_sz && g_psw)
    {
        static std::vector<const char*> s_aqDone;
        for (const char* q = s_sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            int oid = 0;
            float t = 0.0f;
            if (std::sscanf(q, "%d@%f", &oid, &t) != 2 || g_clock.t < t) continue;
            if (std::find(s_aqDone.begin(), s_aqDone.end(), q) != s_aqDone.end()) continue;
            s_aqDone.push_back(q);
            if (LO* plo = PloFindSwObjectByOid(g_psw, oid))
            {
                if (!plo->fInWorld) AddLo(g_psw, plo);
                EnablePo(g_psw, plo, 1);
                std::printf("setpo: %d the player at t %.2f\n", oid, g_clock.t);
            }
        }
    }
    for (auto& ppc : s_lpadctl) UpdatePadctl(ppc.get());
}

LO* PloPoCurrent(SW*) { return s_ploOverride ? s_ploOverride : s_ploCur; }

void SetPoCurrent(SW* psw, LO* plo)
{
    if (std::getenv("SLY2N_POPRINT")) std::printf("po current: %d\n", plo ? plo->oid : -1);
    LO* pcur = PloPoCurrent(psw);
    LO* pnew = s_ploOverride ? s_ploOverride : plo;
    if (pcur != pnew && pcur) SetCmFocusObject(pcur, false);  // vt slot 78 (0)
    s_ploCur = plo;
    g_transition.ploPlayer = plo;
    if (pcur != pnew && pnew) SetCmFocusObject(pnew, true);   // vt slot 78 (1)
    if (plo && FCidDerivedFrom(plo->Cid(), 0x34))
    {
        CRef ref;
        ref.SetS32(plo->oid);
        FSetProgress({"oidPoCurrent"}, ref);  // 0x1D8B20
    }
    // 0x194F70(psw, 0, 0, 0): the tracked interaction targets dropped (R58: the safe house's door kept the previous
    // player's aura, e.g. Murray's emblem with Bentley at the door, because it stayed tracked across the switch)
    DropJtTrack();
    // TODO: 0x1C48C0 (SW +0x4778's followers, put on the new player: registered by the splice method 0x105AF0 ->
    // 0x1594D8 -> 0x1C4848, not bound; no script was seen calling it)
}

// orig: 0x001C4A60 (the forced player object SW +0x3F00: the world's EOPID 928 (id 0x22), its getter 0x1C4A48
// EOPID 927; cleared by 0x2102A0): the old and the new effective ones told (vt slot 78)
void SetPoForced(SW* psw, LO* plo)
{
    LO* pold = PloPoCurrent(psw);
    LO* pnew = plo ? plo : s_ploCur;
    if (pold != pnew)
    {
        if (pold) SetCmFocusObject(pold, false);  // vt slot 78 (0)
        s_ploOverride = plo;
        if (pnew) SetCmFocusObject(pnew, true);   // vt slot 78 (1)
        return;
    }
    s_ploOverride = plo;
}

// orig: 0x001C4080 (the control state; -1 without a controller state)
int NPoEnabled(const LO* plo)
{
    auto it = s_mppodata.find(const_cast<LO*>(plo));
    return it == s_mppodata.end() || !it->second.ppadctl ? -1 : it->second.ppadctl->nControl;
}

// orig: 0x001C3ED8 (vt slot 79 of the POs; the controller state made on first use, up to 16 objects)
void EnablePo(SW* psw, LO* plo, int n)
{
    if (std::getenv("SLY2N_POPRINT")) std::printf("po enable: %d %d (was %d)\n", plo ? plo->oid : -1, n, NPoEnabled(plo));
    if (std::getenv("SLY2N_POBT") && n == 1)  // (debug: who enables a player)
    {
        void* a[12];
        const int c = backtrace(a, 12);
        std::fflush(stdout);
        backtrace_symbols_fd(a, c, 1);
    }
    if (n != NPoEnabled(plo))
    {
        PODATA& pd = s_mppodata[plo];
        if (!pd.ppadctl)
        {
            if (s_apo.size() >= 16) return;
            s_apo.push_back(plo);
            auto ppc = std::make_unique<PADCTL>();  // 0x1C4280: cleared, no state, on the list
            ClearPadctl(ppc.get());
            ppc->plo = plo;
            ppc->nControl = -1;
            pd.ppadctl = ppc.get();
            s_lpadctl.push_back(std::move(ppc));
        }
        if (n == 1)
        {
            if (plo != s_ploCur)
            {
                if (s_ploCur) EnablePo(psw, s_ploCur, 0);  // (vt slot 79 (0))
                SetPoCurrent(psw, plo);
            }
        }
        else if (n == 0 || n == -1)
        {
            if (plo == s_ploCur) SetPoCurrent(psw, nullptr);
        }
        SetPadctlControl(pd.ppadctl, n);
    }
    // the safe house's slot 79 (0x1D7418): its state with it
    if (plo->pcls && plo->pcls->PfnOrig(79) == 0x1D7418 && (n == 0 || n == 1) && !(s_fWalkSwitch && n == 0))
        SetSafeHouseState(psw, plo, n);
    // the actors' slot 79 (0x1406E8, every caller here is the original's vt slot 79): after 0x1C3ED8 the pursued
    // record's mode (0x1F82B8: 0 = the guards place themselves by the camera's heading while it is the player, else
    // -1) and, when it takes the pad, its job list cleared (0x1EECD0)
    if (plo->pcls && plo->pcls->PfnOrig(79) == 0x1406E8)
    {
        PtargetrecOf(plo)->nMode = n == 1 ? 0 : -1;
        if (n == 1)
            if (ACTOR* pa = PactorFromLo(plo)) ClearJobs(pa);
    }
}

int CpoRegistered() { return static_cast<int>(s_apo.size()); }
const std::vector<LO*>& AploRegistered() { return s_apo; }

namespace
{
// orig 0x001D72A8 (the safe house's vt slot 16): left / right through its allowed states; a state's button
// (option 1338: {state, buttons, target}) opens the shop (0x378: 0x1DC260(0x3DA150, 0)) or starts the target's
// script (0x1DFD58, scpt.cpp)
void UpdateSafeHouse(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    auto it = s_mpsh.find(plo);
    if (it == s_mpsh.end()) return;
    SAFEHOUSE& sh = it->second;
    if (std::getenv("SLY2N_WALKDBG")) { static int s_c = 0; if (s_c++ % 60 == 0) std::printf("safe house update: state %d psma %p shop %d menu %d walk %d phase %d\n", sh.nState, (void*)sh.psma, FShopShown(), FMenuShown(), s_walk.plosh == plo, s_walk.phase); }
    if (sh.nState != 1 || !sh.psma) return;
    if (FShopShown() || FMenuShown()) return;   // (0x14FD50: the camera's own; the shop / menus take the pad)
    const bool fWalk = FWalkableSafeHouse() && s_walk.plosh == plo;
    if (fWalk && s_walk.phase == 1)
    {
        // (once the safe house's camera has settled: the character it shows takes the pad, the safe house stays shown)
        if (g_clock.t - s_walk.tEnter < 0.5f) return;
        const OID oidCur = OidSmaCur(sh.psma);
        const OID oidActor = (oidCur == 8 || oidCur == 9) ? oidCur : 7;
        LO* pa = PloFindSwObjectByOid(g_psw, oidActor);
        if (!pa) { s_walk = WALKSH{}; return; }
        if (!pa->fInWorld) AddLo(g_psw, pa);
        s_fWalkSwitch = true;
        EnablePo(g_psw, pa, 1);
        s_fWalkSwitch = false;
        // (the safe house's scene holds the character at the table: its sequences on it retracted, its machine
        // handle cleared, so its own movement (idle, walk) takes over)
        if (FCidDerivedFrom(pa->Cid(), 0x30))
        {
            for (ASEGA* pasega : std::vector<ASEGA*>(g_apasega))
                if (pasega && !pasega->fRetracted && pasega->palo == static_cast<ALO*>(pa)) RetractAsega(pasega);
            if (ACTOR* pact = PactorFromLo(pa); pact && pact->psma) ClearSmaHandle(&pact->psma);
            // (its scripted state (18: held by the scene) to standing)
            if (SLY* psly = PslyFromLo(pa)) SetSlyState(psly, 0, 0);
            else if (BENTLEY* pb = dynamic_cast<BENTLEY*>(PactorFromLo(pa))) SetBentleyState(pb, 0);
            else if (MURRAY* pm = dynamic_cast<MURRAY*>(PactorFromLo(pa))) SetMurrayState(pm, 0);
        }
        s_walk.ploActor = pa;
        s_walk.phase = 2;
        std::printf("safe house: walking as %d\n", oidActor);
        return;
    }
    if (fWalk)
    {
        if (s_walk.phase != 2) return;   // (3: a choice made, the original's action runs)
        // the choice stood by: the other gang members (their places), ThiefNet (the table, the safe house's place)
        if (!s_walk.ploActor || PloPoCurrent(g_psw) != s_walk.ploActor) return;
        // (the safe house's fixed look-at camera (priority 10, its states' shots) taken off: the character's follow
        // camera under it shows the walk; the states re-push it, so each step)
        if (g_cm.ccpr > 1 && g_cm.acpr[0].cpp == 10 && dynamic_cast<CPLOOKAT*>(g_cm.acpr[0].pcplcy))
            RevokeCmPolicy(&g_cm, 3, 10, g_cm.acpr[0].pcplcy, nullptr, nullptr);
        const glm::vec3 posMe = PosLoWorld(s_walk.ploActor);
        // (walked out of the hideout (it opens onto the street): the safe house left, as its exit does, the walk goes on
        // in the hub)
        if (glm::length(glm::vec2(posMe - PosLoWorld(plo))) > 520.0f)
        {
            std::printf("safe house: walked out\n");
            s_walk = WALKSH{};
            SetSafeHouseState(g_psw, plo, 0);
            return;
        }
        const std::vector<OID> aoid = AoidAllowed(plo);
        OID oidNear = -1;
        float dBest = 170.0f;
        for (OID oid : aoid)
        {
            if (oid == s_walk.ploActor->oid) continue;
            glm::vec3 pos;
            if (oid == 10) pos = PosLoWorld(plo);
            else if (LO* pg = PloFindSwObjectByOid(g_psw, oid)) pos = PosLoWorld(pg);
            else continue;
            const float d = glm::length(glm::vec2(pos - posMe));
            if (d < dBest) dBest = d, oidNear = oid;
        }
        if (oidNear != s_walk.oidNear)
        {
            s_walk.oidNear = oidNear;
            if (oidNear >= 0) SetSmaGoal(sh.psma, oidNear);
            std::printf("safe house: by %d\n", oidNear);
        }
        if (oidNear < 0) return;   // (nothing to choose: the button is the character's own)
    }
    else if (const int n = NStepLeftRight())
    {
        StepSafeHouse(plo, sh, n);
        return;
    }
    const OID oidCur = OidSmaCur(sh.psma);
    static OID s_oidLast = -2;
    if (std::getenv("SLY2_HUDDBG") && oidCur != s_oidLast)
    {
        s_oidLast = oidCur;
        std::printf("safe house: state %d (goal %d) dt %.4f factors", oidCur, OidSmaGoal(sh.psma), g_clock.dt);
        for (float g : g_clock.af) std::printf(" %g", g);
        std::printf("\n");
    }
    for (const OPT& o : plo->aopt)
    {
        if (o.eopid != 1338 || o.aval.size() < 3) continue;
        if (o.aval[0].n != oidCur) continue;
        if (fWalk && oidCur != s_walk.oidNear) continue;
        if (fWalk && s_walk.ploActor)
            if (PODATA* ppd = PpodataFromLo(s_walk.ploActor); ppd && ppd->ppadctl) ForgetPadctlPresses(ppd->ppadctl, uint32_t(o.aval[1].n));   // (no jump)
        if (!GrfbtnTakePressed(uint32_t(o.aval[1].n))) continue;
        if (fWalk && o.aval[2].n != 0x378) s_walk.phase = 3;   // (ThiefNet: the walk goes on after it)
        if (fWalk) std::printf("safe house: chose %d\n", oidCur);
        const OID oidTarget = o.aval[2].n;
        if (oidTarget == -1) continue;
        if (oidTarget == 0x378) SetShopPage(0);
        else
        {
            // the target found from the safe house (0x15E938 kind 4), a script started (0x1DFD58: scpt.cpp)
            LO* pscpt = g_splicegame.pfnFindObject ? dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, static_cast<SYMID>(oidTarget), plo, nullptr)) : nullptr;
            if (!pscpt) continue;
            if (std::getenv("SLY2N_POPRINT")) std::printf("safe house %d: action %d (state %d)\n", plo->oid, oidTarget, oidCur);
            StartScpt(pscpt, nullptr);
        }
        return;
    }
    // QoL DEVIATION (U33, see RefSetSafeHouseStates): Episode 4's safe house has no action (option 1338) for Murray
    // and no script 27262 (the other hubs' {9, CROSS, 27262}, which starts the exit script 27254 with Murray as in
    // 27264 / 27266 for Sly / Bentley). Do the same here.
    if (sh.fMurrayQol && oidCur == 9 && GrfbtnTakePressed(64))
    {
        LO* pscpt = PloFindSwObjectByOid(g_psw, 27254);
        LO* plo9 = PloFindSwObjectByOid(g_psw, 9);
        if (pscpt && plo9 && FCidDerivedFrom(pscpt->Cid(), 0x72)) StartScpt(pscpt, plo9);
    }
}
struct INITSAFEHOUSE
{
    INITSAFEHOUSE()
    {
        RegisterLoUpdate(0x1D72A8, UpdateSafeHouse);
        RegisterObjectMethod(0x10F648, RefSetSafeHouseStates);
    }
} s_initsafehouse;
}  // namespace

void ResetPos(SW*)
{
    s_mppodata.clear();
    s_lpadctl.clear();
    s_apo.clear();
    s_ploCur = nullptr;
    s_ploOverride = nullptr;
    s_mpsh.clear();
    ResetTurrets();
    ResetVehicles();
    ResetRopes();
}

namespace
{
struct INITPOSETTERS
{
    INITPOSETTERS()
    {
        // EOPID 884 (id 806): vt slot 79 (the enable), its getter 0x1C4080
        RegisterObjectSetter(884, [](LO* plo, const CRef& ref) {
            EnablePo(g_psw, plo, ref.m_tagk == TAGK_Bool ? ref.m_bool : ref.m_n);
        });
        RegisterObjectGetter(884, [](LO* plo) {
            CRef r;
            r.SetS32(NPoEnabled(plo));
            return r;
        });
        // EOPID 888 (option 810, field +0x150): the controller state (a class 0x89 object; null before control)
        RegisterObjectGetter(888, [](LO* plo) {
            CRef r;
            const PODATA* ppd = PpodataFromLo(plo);
            r.SetBasic(ppd ? ppd->ppadctl : nullptr);
            return r;
        });
        // orig: 0x00200598 / 0x001FFD98 (set/get of EOPID 889, option 811: the PO's +0x154 forced buttons).
        // SCPT 1690's sym6 forces R1 (8); the 152651 event in 1697 / 1712 clears it again.
        RegisterObjectSetter(889, [](LO* plo, const CRef& ref) {
            if (PODATA* ppd = PpodataFromLo(plo))
            {
                ppd->grfbtnForced = static_cast<uint32_t>(ref.m_tagk == TAGK_Bool ? ref.m_bool : ref.m_n);
                plo->SetField(0, 0x154, ppd->grfbtnForced);
            }
        });
        RegisterObjectGetter(889, [](LO* plo) {
            CRef r;
            const PODATA* ppd = PpodataFromLo(plo);
            r.SetS32(ppd ? static_cast<int32_t>(ppd->grfbtnForced) : 0);
            return r;
        });
    }
} s_initposetters;
}

namespace
{
// orig 0x001C3ED8 as the POs' vt slot 79 (e.g. SCPT command 0x07: the boot world's title makes SO 150643 the player,
// so the title's camera follows it and the gauge has no actor)
void EnablePoVt(LO* plo, int n) { EnablePo(g_psw, plo, n); }
NATIVEFN(0x1C3ED8, EnablePoVt);
}

// Debug (F6, temporary): every character selectable at the safe house from now on (the states of its option 1339
// stay allowed whatever the hub's script gives). Returns the safe houses changed in this world.
int UnlockSafeHouseCharacters(SW* psw)
{
    s_fAllCharacters = true;
    int c = 0;
    if (!psw) return 0;
    for (LO* plo : AploOfClass(psw, 0x46))
    {
        SAFEHOUSE& sh = s_mpsh[plo];
        if (!sh.fAllowedSet) continue;   // (still the option's full list)
        for (OID oid : AoidOptionStates(plo))
            if (std::find(sh.aoidAllowed.begin(), sh.aoidAllowed.end(), oid) == sh.aoidAllowed.end()) sh.aoidAllowed.push_back(oid);
        ++c;
    }
    return c;
}

// (debug harness, autoplay.cpp: the safe house's shown state (its machine's current state oid: 7 Sly, 8 Bentley,
// 9 Murray, 10 ThiefNet) and whether it may show that one; -1 when the object is no safe house)
int OidSafeHouseShown(const LO* plo, std::vector<OID>* paoidAllowed)
{
    auto it = s_mpsh.find(plo);
    if (it == s_mpsh.end() || !it->second.psma) return -1;
    if (paoidAllowed) *paoidAllowed = AoidAllowed(plo);
    return OidSmaCur(it->second.psma);
}
