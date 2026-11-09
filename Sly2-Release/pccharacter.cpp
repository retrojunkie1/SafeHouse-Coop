// PC addition, not in the original game: the debug menu's guest characters and outfits (pccharacter.h).
// Nothing here runs unless the debug menu (Debug keys) asks for it.
#include "pccharacter.h"
#include "coop.h"
#include "pcguard.h"
#include "pcrc.h"
#include "pcloader.h"

#include "activate.h"
#include "actor.h"
#include "cplcy.h"
#include "damage.h"
#include "display.h"
#include "hud.h"
#include "actanim.h"
#include "anim.h"
#include "asega.h"
#include "bone.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "glob.h"
#include "joy.h"
#include "lo.h"
#include "pcinput.h"
#include "po.h"
#include "shd.h"
#include "sm.h"
#include "so.h"
#include "sound.h"
#include "projectile.h"
#include "stepphys.h"
#include "guard.h"
#include "spliceglue.h"
#include "step.h"
#include "sw.h"
#include "transition.h"
#include "worldaccess.h"
#include "xf.h"
#include "frame.h"
#include "geom.h"
#include "proc.h"
#include <string>
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

extern std::unordered_map<int, int> g_mpoidiwrecTask;
void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);  // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                       // launcher.cpp (method 508, 0x1D6B00)
extern int g_iframeMain;   // game.cpp (debug: the animation survey's frames)   // autoplay.cpp / brx.cpp (the loader adds to it)

namespace
{
// ------------------------------------------------------------ the table
// Carmelita (cid 0x3F guard 12712) has the same object, animations and kind table (EOPID 243) in all three worlds; only
// her meshes differ. Her animations (looked at one by one): the idle SM 418 (her kind table's state bits 1 / 2: a
// relaxed stand, the pistol down), 416 a relaxed walk (204 / s: her walk, scaled down for the slow walk), 13171 a
// stride with the pistol held forward (250 / s: the table's moving entry, bits 9 / 10), 13187 her run (600 / s: the
// table's chase entries {27 9} / {28 9} / {31 9}, as she runs after Sly in the prologue and A Friend in Need: R1),
// 590 her jumping dive (the stepped kind-2 move of bit 15) and 13177 its wide landing stance (held
// while she falls), 13212 a pistol strike. Not drawn: her dizzy birds (13144: 13145 / 13153's 13148, a KO's), the
// pickpocket loot she carries (12787, cid 0x63), the comic "THWACK!" (13061 / 13063) and the shock effects (13118's
// groups, 13108), the closed eyes (12982) and a hidden effect (13054): her AI and scripts show those.
const int s_an0CarmHide[] = {13061, 13063, 12982, 13054, -1};
const int s_aoidCarmHide[] = {13144, 12787, 13118, 13108, -1};
// her ballroom dress (8610: the ballroom's scripts show it when she comes in; loaded hidden)
const int s_an0CarmTangoShow[] = {8610, -1};
// Sly's ballroom model (record 11) has his own groups and the tuxedo's, all shown as loaded; the dances (jobs 2059 /
// 1593's scripts) show the tuxedo's (3818 its body, 4125, 4148, 4201, 4254, 4256, 4273) and hide the ones it replaces
// (313 his body, 320, 3905, 4167, 4203, 4246). The rest (his hands' and face's groups) stay his animations' business.
const int s_an0TangoShow[] = {3818, 4125, 4148, 4201, 4254, 4256, 4273, -1};
const int s_an0TangoHide[] = {313, 320, 3905, 4167, 4203, 4246, -1};
const int s_anNone[] = {-1};
// Her moveset's parts (the same in the three files): the high jump 13167 (her crouching launch: the leap of her chase),
// her turn in place (her set has no turn: her AI turns her body; her side steps 13181 left / 13183 right, 300 / s),
// her shot (13212's script event 17903 in her world: the launcher 13048 throws an instance of proxy 12734, the
// straight projectile 12736 (cid 0x51, 1300 / s, the shock's emitters and burst), 0.25 s into the animation, from her
// world's point 12979 on her arm's record; that record is aimed by her AI's arm controller, which the puppet does not
// run, so the muzzle here is where the animation's pose holds the pistol: 80 ahead, 15 right, 105 up).
// Her camera (R37): her collision body (94 x 143) is Sly's size, so the computed fit framed her as close as Sly, but
// she stands upright (her head about 230 above her feet, her ears 250; Sly crouches, his head about 140): her figure
// filled the picture. The camera is pulled back and its focus raised for her real height (her model is drawn at its
// own size: the same file, no scale on her records; in the tango, job 1593, her head and Sly's are level).
constexpr float c_uCarmCamDist = 1.25f, c_sCarmCamHeight = 150.0f;
// Her run cycle 13187's own ground speed (measured: her feet's speed while on the ground in her frame,
// SLY2N_PCCHARFEET, 712 / s and more: about 760; the table's chase entries move her at 600, the cycle was made faster):
// her run is played at the speed's share of it, so at Sly's 830 / s her feet keep to the ground (x 1.09; it was 600:
// x 1.38, too fast). Her walk 416 measured 195 / s (204 kept).
constexpr float c_sCarmRunCycle = 760.0f;
const PCCHARDEF s_adef[] = {
    {"Carmelita (Prologue)", PCCHARK_Puppet, 0, 12712, 418, 416, 13187, 590, 13177, 13177, 13212, 13167, 13181, 13183, 12736, 13048, 0.25f, {80.0f, 15.0f, 105.0f}, 204.0f, c_sCarmRunCycle, s_an0CarmHide, s_aoidCarmHide, s_anNone, -1, nullptr, 0.0f, 0.0f, c_uCarmCamDist, c_sCarmCamHeight},
    {"Carmelita (Snow, Episode 6)", PCCHARK_Puppet, 27, 12712, 418, 416, 13187, 590, 13177, 13177, 13212, 13167, 13181, 13183, 12736, 13048, 0.25f, {80.0f, 15.0f, 105.0f}, 204.0f, c_sCarmRunCycle, s_an0CarmHide, s_aoidCarmHide, s_anNone, -1, nullptr, 0.0f, 0.0f, c_uCarmCamDist, c_sCarmCamHeight},
    {"Carmelita (Tango, Episode 2)", PCCHARK_Puppet, 11, 12712, 418, 416, 13187, 590, 13177, 13177, 13212, 13167, 13181, 13183, 12736, 13048, 0.25f, {80.0f, 15.0f, 105.0f}, 204.0f, c_sCarmRunCycle, s_an0CarmHide, s_aoidCarmHide, s_an0CarmTangoShow, -1, nullptr, 0.0f, 0.0f, c_uCarmCamDist, c_sCarmCamHeight},
    {"Tango (Episode 2)", PCCHARK_SlyOutfit, 11, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, s_an0TangoHide, s_anNone, s_an0TangoShow},
};
// The whole table: the entries above, then the guards and bosses (pcguard.cpp's table, kinds Guard / Boss; their
// animations are filled from their own kind table when first used, PcGuardBegin)
std::vector<PCCHARDEF>& Adef()
{
    static std::vector<PCCHARDEF> s_a = [] {
        std::vector<PCCHARDEF> a(std::begin(s_adef), std::end(s_adef));
        AppendPcGuardCharDefs(a);
        return a;
    }();
    return s_a;
}
int CDef() { return int(Adef().size()); }
// drawn on the player as a puppet: Carmelita, the guards and the bosses (their moves: pcguard.cpp)
bool FPuppetKind(PCCHARK k) { return k == PCCHARK_Puppet || k == PCCHARK_Guard || k == PCCHARK_Boss; }
bool FGuardDef(const PCCHARDEF& def) { return def.kind == PCCHARK_Guard || def.kind == PCCHARK_Boss; }

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_PCCHARPRINT") != nullptr;
    return s_f;
}

// ------------------------------------------------------------ the kept worlds (pcloader.h)
SW* PswKeptOf(int idef) { return PswKeptWorld(Adef()[idef].iwrec); }
bool FKeptReady(int idef) { return PckwsKeptWorld(Adef()[idef].iwrec) == PCKWS_Ready; }

ALO* PaloFindModel(int iwrec, int oid) { return static_cast<ALO*>(PloKeptFind(iwrec, oid, 0x30)); }

// An object of the model's tree, else the first of its oid in the kept world.
LO* PloFindNear(SW* pswKept, ALO* proot, int oid)
{
    if (oid < 0 || !pswKept) return nullptr;
    LO* ploAny = nullptr;
    for (const auto& plo : pswKept->aploAll)
        if (plo->oid == oid)
        {
            if (FLoInParentChain(plo.get(), proot)) return plo.get();
            if (!ploAny) ploAny = plo.get();
        }
    return ploAny;
}

// ------------------------------------------------------------ state
int s_idefPuppet = -1, s_idefSly = -1;
int s_idefPuppetWant = -2, s_idefSlyWant = -2;   // (-2: nothing waiting for a load)
bool s_fSceneRebuild = false;

// The puppet's motion (from the player's: StepPuppet)
enum PUPM { PUPM_None, PUPM_Idle, PUPM_Walk, PUPM_Run, PUPM_Air, PUPM_Attack, PUPM_Land, PUPM_Turn, PUPM_Aim };

struct PUPPET
{
    int idef = -1;
    SW* pswKept = nullptr;         // its kept world (pcloader.h)
    ALO* proot = nullptr;
    std::vector<ALO*> apaloTree;   // its ALOs, parents first
    std::vector<ALO*> apaloDraw;   // the ones drawn (with meshes, not hidden)
    ASEGA* pasega = nullptr;
    ASEG* pasegCur = nullptr;
    bool fLoop = true;
    PUPM pupm = PUPM_None;
    LO* ploPlayer = nullptr;       // the hidden player
    bool fPlayerDrawn = false;     // it would be drawn (she is drawn when it would be: cutscenes, binocucom, fades)
    uint32_t serial = 0;           // its world's
    glm::vec3 posPrev{0.0f};
    bool fPrev = false;
    float sSpeed = 0.0f;           // smoothed ground speed
    glm::vec2 vGround{0.0f};       // smoothed ground velocity (her aim mode's strafe direction)
    float dtAir = 0.0f;            // how long off the floor
    float dtAttack = 0.0f;         // the attack's time left
    float dtLand = 0.0f;           // the landing's time left (her moveset)
    float dtAirHold = 0.0f;        // her jump's start: in the air whatever the floor says (her moveset)
    bool fFlip = false;            // her double jump's flip (590's roll) playing
    // her shot over her walk / run / strafe: the shot's animation on her upper body only (records under
    // c_ixfCarmUpper), the legs keep the locomotion's cycle
    ASEGA* pasegaUpper = nullptr;
    ASEG* pasegUpper = nullptr;
    float radPrev = 0.0f;          // the heading of the step before
    float wTurn = 0.0f;            // the smoothed turn rate (radians / s)
    // its parts added / removed by its animations (events 7 / 8, a guard's: the flashlight guard's muzzle flash): their
    // fade (0 hidden) and its rate; the ones its world removes at its start begin hidden
    struct PARTFADE { float g = 1.0f, dg = 0.0f; };
    std::unordered_map<const ALO*, PARTFADE> mppartfade;
} s_pup;

// ------------------------------------------------------------ Carmelita's moveset (state)
// Tuning (her moves on Sly's object): her walk by the stick (her walk cycle's speed at a full push, slower below), her
// run with R1 held (her chase's run cycle and speed), her jump 170 units high (Sly's 215, no double jump), her high
// jump 520; her shot aims at the nearest guard within 3000 ahead (40 degrees either side), else straight ahead.
constexpr float c_uSlowWalk = 0.3f;    // her slowest walk (a slight push): this much of her walk's speed
// her jump 200 units high (round 3: a bit higher than 170; Sly's 215), her double jump 180 more from where she is (Sly's
// goes to 315), her high jump 520
// (her double jump's second jump 250 (180 before) and her high jump 580 (520 before): "her
// mega jump a bit higher")
constexpr float c_hJump = 200.0f, c_hJumpDouble = 250.0f, c_hJumpHigh = 580.0f;
// her double jump's flip: her leap 590 ends in a forward roll out of the dive (0.78 .. 1.25 s: upside down at 0.85 ..
// 1.05, on her feet at 1.25); the double jump plays that part (her set has no other flip / somersault: every clip
// of her three files looked at)
constexpr float c_tFlipStart = 0.78f, c_tFlipEnd = 1.25f;
// her upper body (the shot over her walk): record 13 (the spine above the hips' branch: her chest, arms, head) and
// everything under it; the hips, legs, tail and coat stay the locomotion's
constexpr size_t c_ixfCarmUpper = 13;
constexpr float c_sShotRange = 3000.0f;
// her run (R1): Sly's own R1 run (his state 1: 830 / s on open ground, measured with SLY2N_FRAMETICK in world 27; his
// run without R1, state 12, 510 / s)
constexpr float c_sRunSly = 830.0f;
// Her aim mode (Triangle held; Sly 3's strafe with her shock pistol): the crosshair this far above the picture's centre
// (NDC; her head is near the centre), the camera's focus raised to it and the camera a bit nearer while aiming; the
// shot goes to the crosshair's point c_sAimDist beyond her, or to the guard locked on (within c_radAimLock (20 degrees)
// of her aim across and c_sShotRange of her, on the picture: the one nearest her aim); she strafes at c_sStrafe at a full push
// (R1: c_sStrafeFast), with her chase's directional run cycles (13187 forward, 13185 back, 13181 left, 13183 right, 600 /
// s) and, standing, her pose 17989 (upright, the pistol forward at her chest; 13175, used before, is her
// alert stance: the pistol held low behind her, bouncing on her feet, which made her look turned round).
constexpr float c_yAimNdc = 0.22f, c_sAimDist = 2500.0f, c_radAimLock = 0.35f;
constexpr float c_sAimCamHeight = 215.0f, c_uAimCamNear = 0.85f;
constexpr float c_sStrafe = 420.0f, c_sStrafeFast = 600.0f;
constexpr int c_oidAimIdle = 17989, c_oidStrafeF = 13187, c_oidStrafeB = 13185, c_oidStrafeL = 13181, c_oidStrafeR = 13183;
constexpr int c_cShot = 5;              // her shots in the world at once (three before the aim mode's faster fire)
bool s_fMoveset = true;              // the menu's "Carmelita's moves"
std::vector<ALO*> s_apaloShot;       // her shot's projectiles in the current world (clones made at its load)
uint32_t s_serialShot = 0;           // (their world's)
bool s_fShotPending = false;         // her attack animation runs, the shot not thrown yet
LO* s_ploShotTarget = nullptr;       // the guard aimed at (its world's)
float s_radShot = 0.0f;              // the heading at the shot's start
bool s_fJumpNew = false, s_fJumpHigh = false, s_fJumpDouble = false;  // a jump of hers started (her jump / high jump animation from its start)
// her aim mode (PcCarmAimInput, each step of the player's input)
bool s_fAim = false;                 // aiming this step (the toggle on, in a state she aims in)
bool s_fAimOn = false;               // the aim mode's toggle (Triangle: on / off; it was held)
bool s_fTrianglePrev = false;
int s_iframeAim = -1;                // (the step it was set in: stale when the player's input did not run)
float s_radAim = 0.0f;               // the camera's heading: hers while aiming
glm::vec3 s_posAim{0.0f};            // the crosshair's point (or the locked guard's centre)
LO* s_ploAimLock = nullptr;          // the guard locked on (its world's)
glm::vec2 s_ndcCross{0.0f};          // the crosshair on the picture (NDC)
float s_uAimCam = 0.0f;              // the aim camera's blend (0 .. 1)
bool s_fShotAimed = false;           // the shot in its animation goes to s_posShotAim (thrown in the aim mode)
glm::vec3 s_posShotAim{0.0f};

bool FMovesetLive() { return s_fMoveset && s_idefPuppet >= 0 && s_pup.proot && g_psw && s_pup.serial == g_psw->serial; }
bool FAimLive() { return s_fAim && s_iframeAim == g_iframeMain && FMovesetLive(); }

// ------------------------------------------------------------ the puppet's animations
bool FHiddenPart(const LO* plo)
{
    const PCCHARDEF& def = Adef()[s_pup.idef];
    for (; plo; plo = plo->paloParent)
        for (const int* p = def.aoidHide; *p != -1; ++p)
            if (plo->oid == *p) return true;
    return false;
}

bool FMovesetPlayer(const LO* plo)
{
    // (Sly, Murray or Bentley: her moves on any of them)
    return FMovesetLive() && plo && plo == s_pup.ploPlayer &&
           (FCidDerivedFrom(plo->Cid(), 0x35) || FCidDerivedFrom(plo->Cid(), 0x36) || FCidDerivedFrom(plo->Cid(), 0x37));
}

// Her animation's mesh-group events (kinds 13 / 14: a group shown / hidden, 0x210408 / 0x2104A0) on her own objects;
// with her moveset also its sounds (kind 0: her steps, jumps and shot, 0x210740's parameters, at the player); the other
// events (scripts, messages, effects) belong to her world and are not fired.
void FirePuppetGlobEvents(const std::vector<ASEGEV>& aev, float t0, float t1, bool fAll)
{
    const PCCHARDEF& def = Adef()[s_pup.idef];
    for (const ASEGEV& ev : aev)
    {
        if (ev.kind == 0 && ev.an[0] != -1 && (fAll || (ev.t > t0 && ev.t <= t1)) && FMovesetPlayer(s_pup.ploPlayer))
        {
            CSnd* psnd = PsndNewPcKept(Adef()[s_pup.idef].iwrec, ev.an[0]);   // (her world's sound table and bank)
            if (psnd->Kind() == 2) { psnd->Stop(0.0f); continue; }   // (no loopers: nothing would stop them)
            psnd->SetSource(s_pup.ploPlayer);
            psnd->SetFalloff(static_cast<float>(ev.an[2]), static_cast<float>(ev.an[3]));
            psnd->SetVolume(ev.ag[2]);
            psnd->SetPitch(ev.ag[0]);
            continue;
        }
        if (ev.kind != 13 && ev.kind != 14)
        {
            // (a guard's other events: its effects, pcguard.cpp)
            if (ev.kind != 0 && (fAll || (ev.t > t0 && ev.t <= t1)) && FGuardDef(def) && FMovesetPlayer(s_pup.ploPlayer)) PcGuardPuppetEvent(ev);
            continue;
        }
        if (!fAll && !(ev.t > t0 && ev.t <= t1)) continue;
        bool fHidden = false;
        for (const int* p = def.an0Hide; *p != -1; ++p) fHidden = fHidden || ev.an[1] == *p;
        if (fHidden) continue;
        for (ALO* palo : s_pup.apaloDraw)
            if (ev.an[0] == -1 ? palo == s_pup.proot : palo->oid == ev.an[0]) ShowAloGlobs(palo, ev.an[1], ev.kind == 13);
    }
}

void StopPuppetAnim()
{
    // (a guard's end events: its effects turned off, as the AI's animation's retract fires them)
    if (s_pup.pasegCur && s_pup.idef >= 0 && FGuardDef(Adef()[s_pup.idef]))
        FirePuppetGlobEvents(PasegdOf(s_pup.pasegCur)->aaev[2], 0.0f, 0.0f, true);
    if (ASEGA* p = s_pup.pasega)
    {
        p->ieaEnd = int(PasegdOf(p->paseg)->aaev[2].size());   // (no end events: they are the kept world's)
        g_fAsegaEventsMuted = true;   // (nor its retract script)
        RetractAsega(p);
        g_fAsegaEventsMuted = false;
    }
    s_pup.pasega = nullptr;
    s_pup.pasegCur = nullptr;
}

void PlayPuppetAnim(ASEG* paseg, bool fLoop, bool fRestart = false, float tStart = 0.0f)
{
    if (!paseg || (paseg == s_pup.pasegCur && !fRestart)) return;
    StopPuppetAnim();
    SW* pswSaved = g_psw;
    g_psw = s_pup.pswKept;   // (the channels' objects are found in the kept world)
    ASEGA* pasega = nullptr;
    // its events not fired by the apply (R36): its start events ran in the kept world, e.g. Carmelita's landing 13177's
    // explosion (her dive's hit burst) went off at the player and re-initialised the running world's emitters; the
    // ones the puppet wants are fired below (FirePuppetGlobEvents)
    g_fAsegaEventsMuted = true;
    ApplyAseg(paseg, s_pup.proot, tStart, 1.0f, fLoop ? 8 : 0x20, &pasega);
    g_fAsegaEventsMuted = false;
    g_psw = pswSaved;
    // driven here, not by the world's ASEGA pass (its events are its world's)
    if (auto it = std::find(g_apasega.begin(), g_apasega.end(), pasega); it != g_apasega.end()) g_apasega.erase(it);
    s_pup.pasega = pasega;
    s_pup.pasegCur = paseg;
    s_pup.fLoop = fLoop;
    FirePuppetGlobEvents(PasegdOf(paseg)->aaev[0], 0.0f, 0.0f, true);
    if (pasega) pasega->t = tStart;
    if (FPrint())
    {
        glm::vec4 v(0.0f), v2(0.0f);
        if (const ASEGCHN* pchn = PchnFindAseg(paseg, s_pup.proot, 1, true); pchn && pchn->ptrkp)
        {
            pchn->ptrkp->Eval(nullptr, &v, PasegdOf(paseg)->tMax * 0.25f, 1.0f);
            pchn->ptrkp->Eval(nullptr, &v2, PasegdOf(paseg)->tMax * 0.75f, 1.0f);
        }
        std::printf("pc character: animation %d (%.2f s) root vel %.0f %.0f %.0f / %.0f %.0f %.0f\n", paseg->oid, PasegdOf(paseg)->tMax, v.x, v.y, v.z, v2.x, v2.y, v2.z);
    }
}

// The animation's time moved by dt at rate svt (looped or held at its end).
void AdvancePuppetAnim(float dt, float svt)
{
    ASEGA* p = s_pup.pasega;
    if (!p) return;
    ASEGD* pd = PasegdOf(p->paseg);
    const float tMax = pd->tMax;
    const float t0 = p->t;
    float t = t0 + dt * svt;
    if (tMax > 0.0f)
    {
        if (s_pup.fLoop && t > tMax)
        {
            FirePuppetGlobEvents(pd->aaev[1], t0, tMax, false);
            t = std::fmod(t, tMax);
            FirePuppetGlobEvents(pd->aaev[1], -1.0f, t, false);
        }
        else
        {
            t = std::min(t, tMax);
            FirePuppetGlobEvents(pd->aaev[1], t0, t, false);
        }
    }
    p->t = t;
    for (ASEGAL& l : p->alink)   // (its scalar links: the kept world's objects' parameters, as UpdateAsega)
        if (l.ptrkg && l.pg) l.ptrkg->Eval(l.pg, nullptr, p->t, p->svt);
}

// Her shot on her upper body: the clip applied as the whole body's, then its controllers on the records
// outside c_ixfCarmUpper's branch taken off (their records stay the locomotion's) and the others raised one priority
// above it (a new cycle started meanwhile does not take them back); its own time, retracted at its end.
bool FCarmUpperRecord(const XF* pxf)
{
    ALO* proot = s_pup.proot;
    if (!proot || proot->axf.size() <= c_ixfCarmUpper) return false;
    const XF* pxfTop = proot->axf[c_ixfCarmUpper].get();
    for (int i = 0; pxf && i < 64; ++i, pxf = pxf->pxfRef)
        if (pxf == pxfTop) return true;
    return false;
}

void StopPuppetUpper()
{
    if (ASEGA* p = s_pup.pasegaUpper)
    {
        p->ieaEnd = int(PasegdOf(p->paseg)->aaev[2].size());
        g_fAsegaEventsMuted = true;
        RetractAsega(p);
        g_fAsegaEventsMuted = false;
    }
    s_pup.pasegaUpper = nullptr;
    s_pup.pasegUpper = nullptr;
}

void PlayPuppetUpper(ASEG* paseg)
{
    StopPuppetUpper();
    if (!paseg || !s_pup.proot || s_pup.proot->axf.size() <= c_ixfCarmUpper) return;
    SW* pswSaved = g_psw;
    g_psw = s_pup.pswKept;
    ASEGA* pasega = nullptr;
    g_fAsegaEventsMuted = true;
    ApplyAseg(paseg, s_pup.proot, 0.0f, 1.0f, 0x20, &pasega);
    g_fAsegaEventsMuted = false;
    g_psw = pswSaved;
    if (!pasega) return;
    if (auto it = std::find(g_apasega.begin(), g_apasega.end(), pasega); it != g_apasega.end()) g_apasega.erase(it);
    int cKept = 0, cOff = 0;
    for (CTRLASEG* pc = pasega->pctrlFirst; pc; pc = pc->pnextAsega)
    {
        RemoveXfCtrl(pc->palo, pc->pxf, pc);
        if (pc->palo == s_pup.proot && FCarmUpperRecord(pc->pxf))
        {
            pc->prio += 1;
            AddXfCtrl(pc->palo, pc->pxf, pc);
            ++cKept;
        }
        else ++cOff;
    }
    s_pup.pasegaUpper = pasega;
    s_pup.pasegUpper = paseg;
    FirePuppetGlobEvents(PasegdOf(paseg)->aaev[0], 0.0f, 0.0f, true);
    if (FPrint()) std::printf("pc character: upper-body animation %d (%d channels on, %d off)\n", paseg->oid, cKept, cOff);
}

void AdvancePuppetUpper(float dt)
{
    ASEGA* p = s_pup.pasegaUpper;
    if (!p) return;
    ASEGD* pd = PasegdOf(p->paseg);
    const float t0 = p->t, t = std::min(t0 + dt, pd->tMax);
    FirePuppetGlobEvents(pd->aaev[1], t0, t, false);
    p->t = t;
    for (ASEGAL& l : p->alink)
        if (l.ptrkg && l.pg) l.ptrkg->Eval(l.pg, nullptr, p->t, p->svt);
}

ASEG* PasegOf(int oid)
{
    LO* plo = PloFindNear(s_pup.pswKept, s_pup.proot, oid);
    if (plo && FCidDerivedFrom(plo->Cid(), 0x6D)) return static_cast<ASEG*>(plo);
    if (plo && plo->Cid() == 0x6F)   // an SM: its first transition's animation
    {
        SM* psm = static_cast<SM*>(plo);
        for (const SM::SMT& t : psm->asmt)
            if (t.paseg) return t.paseg;
    }
    return nullptr;
}

void DumpPuppet()
{
    ALO* p = s_pup.proot;
    std::printf("pc character: model %d cid 0x%X: %zu records, %zu tree objects, %zu drawn\n", p->oid, p->Cid(), p->axf.size(),
                s_pup.apaloTree.size(), s_pup.apaloDraw.size());
    for (size_t i = 0; i < p->axf.size(); ++i)
    {
        int iRef = -1;
        for (size_t j = 0; j < p->axf.size(); ++j)
            if (p->axf[i]->pxfRef == p->axf[j].get()) iRef = int(j);
        std::printf("  record %zu parent %d kind %d tag %d\n", i, iRef, p->axf[i]->kind, p->axf[i]->tag);
    }
    for (ALO* pa : s_pup.apaloTree)
    {
        std::printf("  alo %d cid 0x%X parent %d globs %zu%s\n", pa->oid, pa->Cid(), pa->paloParent ? pa->paloParent->oid : -1,
                    pa->pglobset ? pa->pglobset->aglob.size() : 0, FHiddenPart(pa) ? " (hidden)" : "");
        if (pa->pglobset)
            for (const GLOB& g : pa->pglobset->aglob)
                std::printf("    glob n0 %d shown %d bones %zu vtx %zu owner %d center %.0f %.0f %.0f r %.0f\n", g.n0, FAloGlobShown(pa, g.n0, !(g.grfGroup & 0x800)),
                            g.abone.size(), g.avtx.size(), g.oidOwner, g.posCenter.x, g.posCenter.y, g.posCenter.z, g.sRadius);
    }
    for (const auto& plo : s_pup.pswKept->aploAll)
        if (FCidDerivedFrom(plo->Cid(), 0x6D) && FLoInParentChain(plo.get(), p))
        {
            const ASEGD& d = static_cast<ASEG*>(plo.get())->asegd;
            std::printf("  aseg %d t %.2f channels %zu\n", plo->oid, d.tMax, d.achn.size());
            if (ASEG* pa = static_cast<ASEG*>(plo.get()); pa->pframe)
                for (int sym : {2, 3, 4})
                    if (CRef* pref = pa->pframe->PrefFindBinding(SYMID(sym), 1); pref && pref->m_tagk == TAGK_Proc && pref->m_pproc)
                    {
                        std::string str;
                        CRef body;
                        body.SetPair(pref->m_pproc->m_ppairCodeExpr);
                        PrintRef(body, &str);
                        std::printf("    script %d: %s\n", sym, str.substr(0, 3000).c_str());
                    }
            for (int i = 0; i < 3; ++i)
                for (const ASEGEV& ev : d.aaev[size_t(i)])
                    std::printf("    ev%d t %.2f kind %d an %d %d %d %d ag %.1f %.1f %.1f\n", i, ev.t, ev.kind, ev.an[0], ev.an[1], ev.an[2],
                                ev.an[3], ev.ag[0], ev.ag[1], ev.ag[2]);
        }
    for (const auto& plo : s_pup.pswKept->aploAll)
        if (!FCidDerivedFrom(plo->Cid(), 0x6D) && !FCidDerivedFrom(plo->Cid(), 0x30) && FLoInParentChain(plo.get(), p))
            std::printf("  lo %d cid 0x%X parent %d\n", plo->oid, plo->Cid(), plo->paloParent ? plo->paloParent->oid : -1);
}

// ------------------------------------------------------------ the guest's collision size and camera
// With a puppet's moveset on Sly, Sly's object is the body:
// its collision shapes (the step meshes its states morph the custom entry toward: its entries that are off, no contact
// id, not the hit spheres) are scaled to the guest's body (its own step entry, the AI data's +0x264 tag, in its kept
// world), about their bottom so the feet stay on the floor; the follow camera's distance and its focus height (the
// actor's +0x1048, Sly's 125) scale with it. Everything is put back as it was when the puppet ends, the moveset goes
// off, the player changes or Debug keys go off (a world change frees the objects: nothing to put back).
struct FITSOE
{
    int isoe = -1;
    GEOM geomLocal, geomWorld;
    float sRadius = 0.0f;
};
struct FITSTATE
{
    ALO* palo = nullptr;          // the Sly it is on (null: not applied)
    uint32_t serial = 0;
    float sHeightOrig = 125.0f;
    std::vector<FITSOE> asoe;     // the scaled entries' own geometry
    PCCHARFIT fit;                // the current puppet's (computed at its start)
} s_fit;

// a model's body box (its object frame): its step entry (the tag in +0x264), else its largest entry without a
// contact id; false: none
bool FBodyBox(const ALO* palo, int tag, glm::vec3* pmn, glm::vec3* pmx)
{
    const SOE* pbest = nullptr;
    float gBest = -1.0f;
    for (const SOE& e : palo->asoe)
    {
        if (e.geomLocal.apos.size() < 4) continue;
        glm::vec3 mn(3e38f), mx(-3e38f);
        for (const glm::vec3& v : e.geomLocal.apos) mn = glm::min(mn, v), mx = glm::max(mx, v);
        const glm::vec3 d = mx - mn;
        const float g = e.tag == tag ? 1e30f : e.id == 0 && !e.f48 ? d.x * d.y * d.z : -1.0f;
        if (g > gBest) { gBest = g; pbest = &e; *pmn = mn; *pmx = mx; }
    }
    return pbest != nullptr && gBest > 0.0f;
}

constexpr float c_kCollXYMin = 0.8f, c_kCollXYMax = 1.5f;
constexpr float c_kCollZMin = 0.6f, c_kCollZMax = 1.5f;
constexpr float c_uCamDistMin = 0.85f, c_uCamDistMax = 2.2f;

bool FFitSoe(const ALO* palo, int isoe)
{
    const STEP* pstep = PstepFromLo(const_cast<ALO*>(palo));
    const SOE& e = palo->asoe[size_t(isoe)];
    return pstep && isoe != pstep->isoeCustom && e.f50 && !e.f48 && e.id == 0 && !e.geomLocal.apos.empty();
}

// The fit of the puppet's guest: its body box against Sly's standing shape (Sly's 199: 130 x 113 x 145); a row's
// override wins. Sly's sizes are his entry's own when a Sly is at hand (the shape is the same in every world).
void ComputeFit(const PCCHARDEF& def, ALO* proot, ALO* psly)
{
    PCCHARFIT f;
    glm::vec3 mn(0.0f), mx(0.0f);
    const int tag = int(proot->U32Field(0, 0x264, 0xFFFFFFFFu));
    if (FBodyBox(proot, tag, &mn, &mx))
    {
        f.sBodyW = 0.5f * ((mx.x - mn.x) + (mx.y - mn.y));
        f.sBodyH = mx.z - mn.z;
    }
    float sSlyW = 0.5f * (129.8f + 113.2f), sSlyH = 145.0f;
    if (psly)
        if (const STEP* pstep = PstepFromLo(psly); pstep && pstep->isoeSource >= 0 && size_t(pstep->isoeSource) < psly->asoe.size())
        {
            // (his own entry as loaded: the saved copy when the fit is on him)
            const GEOM* pg = &psly->asoe[size_t(pstep->isoeSource)].geomLocal;
            for (const FITSOE& w : s_fit.asoe)
                if (s_fit.palo == psly && w.isoe == pstep->isoeSource) pg = &w.geomLocal;
            glm::vec3 a(3e38f), b(-3e38f);
            for (const glm::vec3& v : pg->apos) a = glm::min(a, v), b = glm::max(b, v);
            if (b.z > a.z) { sSlyW = 0.5f * ((b.x - a.x) + (b.y - a.y)); sSlyH = b.z - a.z; }
        }
    // the collision: the body's own size, within what the levels' doorways, vents and arches take (they are made for
    // Sly, and the tallest guards walk under the same porches: across and up at most 1.5 x his, about 180 x 220 (a
    // 2 x hull, 290 tall, did not fit under world 27's porch roofs: pushed out into the roof)); the camera: the distance with the body's
    // real size (its height, or its width a bit less: a wide body needs less room than a tall one), never much closer
    // than Sly's; the focus at the same share of its height as Sly's 125 of his 145
    float kxy = 1.0f, kz = 1.0f;
    if (f.sBodyH > 20.0f && f.sBodyW > 20.0f)
    {
        kxy = f.sBodyW / sSlyW;
        kz = f.sBodyH / sSlyH;
    }
    f.kCollXY = std::clamp(kxy, c_kCollXYMin, c_kCollXYMax);
    f.kCollZ = std::clamp(kz, c_kCollZMin, c_kCollZMax);
    f.uCamDist = std::clamp(std::max(kz, 0.85f * kxy), c_uCamDistMin, c_uCamDistMax);
    f.sCamHeight = std::clamp(125.0f * kz, 80.0f, 300.0f);
    if (def.kCollXY > 0.0f) f.kCollXY = def.kCollXY;
    if (def.kCollZ > 0.0f) f.kCollZ = def.kCollZ;
    if (def.uCamDist > 0.0f) f.uCamDist = def.uCamDist;
    if (def.sCamHeight > 0.0f) f.sCamHeight = def.sCamHeight;
    // (debug: SLY2N_PCCAMFIT=distance,height: the camera's values for any guest, to try them)
    if (const char* sz = std::getenv("SLY2N_PCCAMFIT")) std::sscanf(sz, "%f,%f", &f.uCamDist, &f.sCamHeight);
    s_fit.fit = f;
    if (std::getenv("SLY2N_PCFITPRINT"))
        for (const SOE& e : proot->asoe)
        {
            glm::vec3 a(3e38f), b(-3e38f);
            for (const glm::vec3& v : e.geomLocal.apos) a = glm::min(a, v), b = glm::max(b, v);
            std::printf("  entry tag %d id 0x%X f %d%d%d pin %d n %zu r %.0f xf %d box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", e.tag, e.id, int(e.f48), int(e.f49),
                        int(e.f50), int(e.pinned), e.geomLocal.apos.size(), e.sRadius, e.pxf == PxfObj(proot) ? 1 : e.pxf == PxfPos(proot) ? 2 : 0, a.x, a.y, a.z, b.x, b.y, b.z);
        }
    if (FPrint() || std::getenv("SLY2N_PCFITPRINT"))
        std::printf("pc character: %s fit: body %.0f x %.0f (entry tag %d, Sly %.0f x %.0f) -> collision x %.2f / z %.2f, camera "
                    "distance x %.2f, focus %.0f\n",
                    def.pchzName, f.sBodyW, f.sBodyH, tag, sSlyW, sSlyH, f.kCollXY, f.kCollZ, f.uCamDist, f.sCamHeight);
}

// (debug: SLY2N_PCCHARPRINT: a hash of every entry's local geometry but the custom one's, the radius and the camera height:
// the same before the fit and after it is put back)
uint32_t HashFitState(const ALO* palo)
{
    uint32_t h = 2166136261u;
    auto mix = [&](const void* pv, size_t cb) {
        for (size_t i = 0; i < cb; ++i) h = (h ^ static_cast<const uint8_t*>(pv)[i]) * 16777619u;
    };
    const STEP* pstep = PstepFromLo(const_cast<ALO*>(palo));
    for (size_t i = 0; i < palo->asoe.size(); ++i)
    {
        if (pstep && int(i) == pstep->isoeCustom) continue;
        const SOE& e = palo->asoe[i];
        mix(e.geomLocal.apos.data(), e.geomLocal.apos.size() * sizeof(glm::vec3));
        for (const SURF& sf : e.geomLocal.asurf) { mix(&sf.normal, sizeof(sf.normal)); mix(&sf.gDot, sizeof(sf.gDot)); }
        mix(&e.sRadius, sizeof(e.sRadius));
    }
    if (const ACTOR* pa = PactorFromLo(const_cast<ALO*>(palo))) mix(&pa->sHeight, sizeof(pa->sHeight));
    return h;
}

void RestoreFit()
{
    g_fPcCmHoldPan = false;
    if (g_ploPcCmDist == s_fit.palo) { g_ploPcCmDist = nullptr; g_uPcCmDist = 1.0f; }
    if (!s_fit.palo) return;
    ALO* palo = s_fit.palo;
    s_fit.palo = nullptr;
    // (a world change freed it: nothing to put back)
    if (!g_psw || g_psw->serial != s_fit.serial) { s_fit.asoe.clear(); return; }
    for (const FITSOE& w : s_fit.asoe)
        if (size_t(w.isoe) < palo->asoe.size())
        {
            SOE& e = palo->asoe[size_t(w.isoe)];
            e.geomLocal = w.geomLocal;
            e.geomWorld = w.geomWorld;
            e.sRadius = w.sRadius;
            e.nStampWorld = -1;
        }
    s_fit.asoe.clear();
    if (ACTOR* pa = PactorFromLo(palo)) pa->sHeight = s_fit.sHeightOrig;
    if (STEP* pstep = PstepFromLo(palo))
    {
        const int isoe = pstep->isoeOther >= 0 ? pstep->isoeOther : pstep->isoeSource;
        if (isoe >= 0) SetStepMesh(palo, isoe, true);   // (the custom entry on his own shape at once)
    }
    MarkAloDirty(palo);
    if (FPrint()) std::printf("pc character: fit off (player %d's own shape and camera, hash %08X)\n", palo->oid, HashFitState(palo));
}

void ApplyFit(SW* psw, ALO* palo)
{
    RestoreFit();
    STEP* pstep = PstepFromLo(palo);
    ACTOR* pa = PactorFromLo(palo);
    if (!pstep || !pa || pstep->isoeCustom < 0) return;
    // (her size against this player's own standing shape: Sly's, Murray's or Bentley's)
    if (s_pup.idef >= 0 && s_pup.proot) ComputeFit(Adef()[s_pup.idef], s_pup.proot, palo);
    const PCCHARFIT& f = s_fit.fit;
    if (FPrint()) std::printf("pc character: fit: player %d's own state hash %08X\n", palo->oid, HashFitState(palo));
    s_fit.palo = palo;
    s_fit.serial = psw->serial;
    s_fit.sHeightOrig = pa->sHeight;
    // (about the standing shape's bottom, the floor contact's height: +0x210)
    const float zBottom = pstep->sFloorTarget;
    glm::mat4 m(1.0f);
    m[0][0] = m[1][1] = f.kCollXY;
    m[2][2] = f.kCollZ;
    m[3][2] = zBottom * (1.0f - f.kCollZ);
    const bool fScale = f.kCollXY != 1.0f || f.kCollZ != 1.0f;
    for (int i = 0; fScale && i < int(palo->asoe.size()); ++i)
    {
        if (!FFitSoe(palo, i)) continue;
        SOE& e = palo->asoe[size_t(i)];
        FITSOE w;
        w.isoe = i;
        w.geomLocal = e.geomLocal;
        w.geomWorld = e.geomWorld;
        w.sRadius = e.sRadius;
        TransformGeom(m, w.geomLocal, &e.geomLocal);   // (points and planes)
        float g2 = 0.0f;
        for (const glm::vec3& v : e.geomLocal.apos) g2 = std::max(g2, glm::dot(v, v));
        e.sRadius = std::sqrt(g2);
        e.nStampWorld = -1;
        s_fit.asoe.push_back(std::move(w));
    }
    if (fScale)
    {
        const int isoe = pstep->isoeOther >= 0 ? pstep->isoeOther : pstep->isoeSource;
        if (isoe >= 0) SetStepMesh(palo, isoe, true);
        MarkAloDirty(palo);
    }
    pa->sHeight = f.sCamHeight;
    g_ploPcCmDist = palo;
    g_uPcCmDist = f.uCamDist;
    if (FPrint()) std::printf("pc character: fit on player %d: %zu shapes scaled\n", palo->oid, s_fit.asoe.size());
}

// each step of the puppet: the fit on the player whose moves are the guest's, off otherwise
void UpdateFit(SW* psw, LO* ploPlayer, bool fMoveset)
{
    // (test aid: SLY2N_PCFIT=0: no fit, Sly's own shape and camera, as before)
    static const bool s_fOff = std::getenv("SLY2N_PCFIT") && std::atoi(std::getenv("SLY2N_PCFIT")) == 0;
    ALO* pwant = fMoveset && ploPlayer && !s_fOff ? static_cast<ALO*>(ploPlayer) : nullptr;
    if (s_fit.palo && (s_fit.palo != pwant || s_fit.serial != psw->serial)) RestoreFit();
    if (pwant && !s_fit.palo) ApplyFit(psw, pwant);
}

void EndPuppet()
{
    RestoreFit();
    StopPuppetUpper();
    StopPuppetAnim();
    s_pup = PUPPET{};
}

bool FBeginPuppet(int idef)
{
    EndPuppet();
    SW* pswKept = PswKeptOf(idef);
    ALO* proot = PaloFindModel(Adef()[idef].iwrec, Adef()[idef].oid);
    if (!proot || !pswKept) return false;
    if (FGuardDef(Adef()[idef])) PcGuardBegin(&Adef()[idef], pswKept, proot);   // (a guard: its own kind table's animations)
    const PCCHARDEF& def = Adef()[idef];
    s_pup.idef = idef;
    s_pup.pswKept = pswKept;
    s_pup.proot = proot;
    if (FPrint() && !proot->axf.empty())
    {
        const glm::mat4& m = proot->axf[0]->mat;
        std::printf("pc character: root record scale %.3f %.3f %.3f (as loaded / last placed)\n", glm::length(glm::vec3(m[0])),
                    glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
        for (size_t i = 0; i < proot->axf.size(); ++i)
        {
            const glm::mat4& mi = proot->axf[i]->mat;
            const float sx = glm::length(glm::vec3(mi[0])), sy = glm::length(glm::vec3(mi[1])), sz = glm::length(glm::vec3(mi[2]));
            if (proot->axf[i]->kind == 12 || std::fabs(sx - 1.0f) > 0.01f || std::fabs(sz - 1.0f) > 0.01f)
                std::printf("pc character: record %zu kind %d tag %d scale %.3f %.3f %.3f\n", i, proot->axf[i]->kind, proot->axf[i]->tag, sx, sy, sz);
        }
    }
    for (const auto& plo : pswKept->aploAll)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30) || !FLoInParentChain(plo.get(), proot)) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        s_pup.apaloTree.push_back(palo);
        if (palo->pglobset && !palo->pglobset->aglob.empty() && !FHiddenPart(palo)) s_pup.apaloDraw.push_back(palo);
    }
    // (a guard's parts its world removes at its start (a muzzle flash): hidden until its animations add them)
    if (FGuardDef(def))
        for (ALO* palo : s_pup.apaloDraw)
            for (const LO* p = palo; p && p != proot; p = p->paloParent)
                if (p->fRemoveAtStart)
                {
                    s_pup.mppartfade[palo] = PUPPET::PARTFADE{0.0f, 0.0f};
                    if (FPrint()) std::printf("pc character: part %d hidden (removed at its world's start)\n", palo->oid);
                    break;
                }
    // the groups her AI shows only for a moment hidden (and the loaded flags of the rest: she may have been used before)
    for (ALO* palo : s_pup.apaloDraw)
    {
        palo->aglobShown.clear();
        for (const int* p = def.an0Hide; *p != -1; ++p) ShowAloGlobs(palo, *p, false);
        for (const int* p = def.an0Show; *p != -1; ++p) ShowAloGlobs(palo, *p, true);
    }
    if (FGuardDef(def)) PcGuardFixGroups(proot);   // (a guard loaded with its body hidden: its instances show it)
    {
        // its collision size and camera (applied on Sly while its moves are on: UpdateFit)
        LO* ploCur = g_psw ? PloPoCurrent(g_psw) : nullptr;
        ComputeFit(def, proot, ploCur && FCidDerivedFrom(ploCur->Cid(), 0x35) ? static_cast<ALO*>(ploCur) : nullptr);
    }
    // (debug: SLY2N_PCCHARHIDE=n0,n0,..: those mesh groups hidden too)
    if (const char* sz = std::getenv("SLY2N_PCCHARHIDE"))
        for (const char* p = sz; *p;)
        {
            const int n0 = std::atoi(p);
            for (ALO* palo : s_pup.apaloDraw) ShowAloGlobs(palo, n0, false);
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    if (FPrint()) DumpPuppet();
    return true;
}

// The bindings of the kept worlds' drawn objects (a world's start clears the table).
void BindPuppet()
{
    int c = 0;
    for (ALO* palo : s_pup.apaloDraw)
        if (const auto* paabind = PaabindKept(palo)) { SetAloBoneBindings(palo, *paabind); ++c; }
    if (FPrint()) std::printf("pc character: %d of %zu drawn objects bound\n", c, s_pup.apaloDraw.size());
    if (FPrint())
    {
        auto dumpSoe = [](const char* name, ALO* palo) {
            if (!palo) return;
            for (const SOE& e : palo->asoe)
            {
                glm::vec3 lo(3e38f), hi(-3e38f);
                for (const glm::vec3& v : e.geomLocal.apos) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
                std::printf("pc character: %s %d soe tag %d r %.1f pts %zu box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", name, palo->oid, e.tag, e.sRadius,
                            e.geomLocal.apos.size(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
            }
        };
        dumpSoe("carmelita", s_pup.proot);
        if (s_pup.ploPlayer && FCidDerivedFrom(s_pup.ploPlayer->Cid(), 0x30)) dumpSoe("player", static_cast<ALO*>(s_pup.ploPlayer));
    }
}

// ------------------------------------------------------------ Carmelita's moveset (her shot)
// The guard to aim at: the nearest within c_sShotRange ahead of rad (40 degrees either side, 1200 up or down).
LO* PloShotTarget(SW* psw, ALO* palo, float rad)
{
    const glm::vec3 pos = PosAloCenter(palo);
    const glm::vec2 dir(std::cos(rad), std::sin(rad));
    LO* ploBest = nullptr;
    float gBest = 1e30f;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo)) continue;
            const glm::vec3 d = PosAloCenter(static_cast<ALO*>(plo)) - pos;
            const float s = glm::length(glm::vec2(d));
            if (s < 1.0f || s > c_sShotRange || std::fabs(d.z) > 1200.0f) continue;
            const float c = glm::dot(glm::vec2(d) / s, dir);
            if (c < 0.766f) continue;   // (cos 40 degrees)
            const float g = s * (2.0f - c);
            if (g < gBest) { gBest = g; ploBest = plo; }
        }
    return ploBest;
}

// Her aim mode, each step she aims (PcCarmAimInput): her heading the camera's; the crosshair c_yAimNdc above the
// picture's centre, its ray from the camera to c_sAimDist beyond her; the guard nearest her aim (within c_radAimLock
// across, c_sShotRange of her, on the picture) locked on: the crosshair on its centre.
void UpdateAim(SW* psw, ALO* palo)
{
    const glm::vec3 fwd = g_cm.mat[0], up = g_cm.mat[2];
    s_radAim = std::atan2(fwd.y, fwd.x);
    const glm::vec3 dir = glm::normalize(fwd + up * (c_yAimNdc * g_cm.yScreenRange));
    const glm::vec3 posPlayer = PosAloCenter(palo);
    s_posAim = g_cm.pos + dir * (glm::length(posPlayer - g_cm.pos) + c_sAimDist);
    s_ndcCross = glm::vec2(0.0f, c_yAimNdc);
    s_ploAimLock = nullptr;
    const float rAspect = g_cm.yScreenRange > 0.0f ? g_cm.xScreenRange / g_cm.yScreenRange : 1.0f;
    const glm::vec2 dirAim(std::cos(s_radAim), std::sin(s_radAim));
    float gBest = 1e30f;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo)) continue;
            const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(plo));
            const glm::vec3 d = pos - posPlayer;
            if (glm::length(d) > c_sShotRange || glm::dot(glm::vec2(d), dirAim) <= 0.0f) continue;
            const glm::vec3 sc = ConvertCmWorldToScreen(&g_cm, pos);
            if (sc.z <= 1.0f || std::fabs(sc.x) > 0.95f || std::fabs(sc.y) > 0.95f) continue;   // (on the picture)
            // (the angle off her aim, across: within c_radAimLock; then the nearest to the crosshair on the picture)
            const float rad = std::acos(std::clamp(glm::dot(glm::normalize(glm::vec2(d) + glm::vec2(1e-3f, 0.0f)), dirAim), -1.0f, 1.0f));
            if (rad > c_radAimLock) continue;
            const float g = rad + 0.25f * glm::length(glm::vec2(sc.x * rAspect, sc.y - c_yAimNdc));
            if (g < gBest) { gBest = g; s_ploAimLock = plo; s_posAim = pos + glm::vec3(0.0f, 0.0f, 30.0f); s_ndcCross = glm::vec2(sc.x, sc.y); }
        }
}

// The shot: a free projectile of hers (a clone of her world's 12736) thrown by her launcher 13048 from her muzzle
// point, as her world's script does it (13212's event: proxy instance, method 508 0x1D6B00), at the guard aimed at (its
// centre, led by its velocity) or straight ahead; then the player as its thrower (no contact with the
// player's own objects: 0x13BBF8; the noise at the impact is the player's), its flight over within 2.5 s.
void ThrowShot(SW* psw, ALO* paloPlayer)
{
    const PCCHARDEF& def = Adef()[s_pup.idef];
    if (s_serialShot != psw->serial) return;
    ALO* po = nullptr;
    for (ALO* p : s_apaloShot)
        if (!p->fInWorld) { po = p; break; }
    LO* plaunch = PloFindNear(s_pup.pswKept, s_pup.proot, def.oidLaunch);
    if (!po || !plaunch)
    {
        if (FPrint()) std::printf("pc character: no shot (projectile %p launcher %p)\n", (void*)po, (void*)plaunch);
        return;
    }
    // the muzzle: in her root record's frame (her feet, facing the player's heading)
    glm::vec3 posMuzzle = PosAloCenter(paloPlayer);
    if (!s_pup.proot->axf.empty())
        posMuzzle = glm::vec3(s_pup.proot->axf[0]->matWorld *
                              glm::vec4(def.aposMuzzle[0], -def.aposMuzzle[1], def.aposMuzzle[2], 1.0f));
    SetAloPos(po, posMuzzle);
    LO* ptarget = s_ploShotTarget && s_ploShotTarget->fInWorld ? s_ploShotTarget : nullptr;
    if (ptarget)
    {
        // its centre, led by its velocity over the flight (1300 / s; her world's scripts set the launcher's lead curve
        // at run time, which the kept world never runs: here the lead is computed, twice for the flight's length)
        // (her aim mode: a bit above its centre, as the crosshair: low guards' centres are near the ground)
        const glm::vec3 posT = PosAloCenter(static_cast<ALO*>(ptarget)) + glm::vec3(0.0f, 0.0f, s_fShotAimed ? 30.0f : 0.0f);
        const glm::vec3 vT = VelAloPos(static_cast<ALO*>(ptarget));
        glm::vec3 posAim = posT;
        for (int i = 0; i < 2; ++i) posAim = posT + vT * (glm::length(posAim - posMuzzle) / 1300.0f);
        SetLauncherAim(plaunch, nullptr, posAim);
    }
    else if (s_fShotAimed)   // (her aim mode: the crosshair's point)
        SetLauncherAim(plaunch, nullptr, s_posShotAim);
    else
        SetLauncherAim(plaunch, nullptr, posMuzzle + glm::vec3(std::cos(s_radShot), std::sin(s_radShot), 0.0f) * c_sShotRange);
    LaunchFrom(plaunch, po);
    SetProjectileLauncher(po, paloPlayer);
    ExpireProjectileIn(po, 2.5f);
    if (FPrint())
        std::printf("pc character: shot %p from %.0f %.0f %.0f at %d, velocity %.0f %.0f %.0f\n", (void*)po, posMuzzle.x, posMuzzle.y, posMuzzle.z,
                    ptarget ? ptarget->oid : -1, VelAloPos(po).x, VelAloPos(po).y, VelAloPos(po).z);
}

// The player's motion picks her animation; she is posed by it and put where the player stands, facing its way.
void StepPuppet(SW* psw)
{
    LO* ploPlayer = PloPoCurrent(psw);
    if (!ploPlayer || !FCidDerivedFrom(ploPlayer->Cid(), 0x30) || FIsLoDerivedFrom(ploPlayer, 0x46) || FPcRcObject(ploPlayer))
    {
        s_pup.ploPlayer = nullptr;   // (the safe house, the RC chopper (pcrc.h): nobody to stand in for)
        return;
    }
    if (s_pup.ploPlayer != ploPlayer || s_pup.serial != psw->serial)
    {
        s_pup.ploPlayer = ploPlayer;
        s_pup.serial = psw->serial;
        s_pup.fPrev = false;
        s_pup.pupm = PUPM_None;
        StopPuppetUpper();
        BindPuppet();
    }
    ALO* paloPlayer = static_cast<ALO*>(ploPlayer);
    s_pup.fPlayerDrawn = FAloDrawn(psw, paloPlayer);
    for (ALO* palo : s_pup.apaloDraw) palo->gFade = paloPlayer->gFade;
    for (auto& [palo, f] : s_pup.mppartfade)
    {
        f.g = std::clamp(f.g + f.dg * g_clock.dt, 0.0f, 1.0f);
        if (f.g <= 0.0f || f.g >= 1.0f) f.dg = 0.0f;
        const_cast<ALO*>(palo)->gFade *= f.g;
    }
    STEP* pstep = PstepFromLo(ploPlayer);
    const XF* pxf = PxfPos(paloPlayer);
    const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(paloPlayer);
    const glm::vec3 posFeet = glm::vec3(m * glm::vec4(pstep ? glm::vec3(pstep->vecBase) : glm::vec3(0.0f), 1.0f));
    const float rad = pstep ? RadStepHeading(pstep) : std::atan2(m[0].y, m[0].x);
    const float dt = g_clock.dt > 0.0f ? g_clock.dt : 0.0f;
    // the motion: the ground speed (smoothed; a jump of more than 30 units in a step is a teleport), the floor
    float sGround = 0.0f;
    glm::vec2 vGround(0.0f);
    if (s_pup.fPrev && dt > 0.0f && glm::length(posFeet - s_pup.posPrev) < 30.0f + 3000.0f * dt)
    {
        vGround = glm::vec2(posFeet - s_pup.posPrev) / dt;
        sGround = glm::length(vGround);
    }
    s_pup.posPrev = posFeet;
    s_pup.fPrev = true;
    s_pup.sSpeed += (sGround - s_pup.sSpeed) * std::min(1.0f, dt * 12.0f);
    s_pup.vGround += (vGround - s_pup.vGround) * std::min(1.0f, dt * 12.0f);
    const bool fFloor = !pstep || pstep->pxpFloor != nullptr;
    s_pup.dtAir = fFloor ? 0.0f : s_pup.dtAir + dt;
    const PCCHARDEF& def = Adef()[s_pup.idef];
    const bool fMoveset = FMovesetPlayer(ploPlayer);
    UpdateFit(psw, ploPlayer, fMoveset);
    // (test aid: SLY2N_PCCARMNEAR=frame,distance: at that frame Carmelita put that far from the nearest guard, facing it,
    // for shots of her aim mode)
    if (static const char* s_szNear = std::getenv("SLY2N_PCCARMNEAR"); s_szNear && fMoveset)
    {
        int f = -1;
        float d = 900.0f;
        std::sscanf(s_szNear, "%d,%f", &f, &d);
        STEP* pst = PstepFromLo(paloPlayer);
        LO* pg = nullptr;
        float gBest = 1e30f;
        for (CID cid = 0x38; g_iframeMain == f && pst && cid <= 0x43; ++cid)
            for (LO* plo : psw->mpcidaplo[cid])
                if (plo->fInWorld && FCidDerivedFrom(plo->Cid(), 0x30) && PguardFromLo(plo))
                    if (const float g = glm::length(PosAloCenter(static_cast<ALO*>(plo)) - PosAloCenter(paloPlayer)); g < gBest) { gBest = g; pg = plo; }
        if (pg)
        {
            const glm::vec3 posG = PosLoWorld(pg);
            const glm::vec3 dir = glm::normalize(glm::vec3(glm::vec2(PosAloCenter(paloPlayer) - posG), 0.0f) + glm::vec3(1e-3f, 0.0f, 0.0f));
            const float radG = std::atan2(-dir.y, -dir.x);
            SetStepPos(pst, posG + dir * d + glm::vec3(0.0f, 0.0f, 20.0f));
            SetStepRotation(pst, glm::mat3(glm::vec3(std::cos(radG), std::sin(radG), 0.0f), glm::vec3(-std::sin(radG), std::cos(radG), 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
            SetAloVelocityTo(paloPlayer, glm::vec3(0.0f));
            // (and the follow camera turned behind her, as its own return to remembered angles does)
            CPFOLLOW& cpf = g_cmpolicies.cpfollow;
            cpf.fManual = 1;
            cpf.radPanReturn = std::atan2(dir.y, dir.x);
            cpf.radTiltReturn = cpf.radTilt;
            std::printf("pc character: (test) put %.0f from guard %d\n", d, pg->oid);
        }
    }
    // her aim mode's camera: the focus raised to the crosshair's height and the camera a bit nearer, blended in and out
    const bool fAim = fMoveset && !FGuardDef(Adef()[s_pup.idef]) && FAimLive();
    s_uAimCam = std::clamp(s_uAimCam + (fAim ? 1.0f : -1.0f) * dt * 5.0f, 0.0f, 1.0f);
    g_fPcCmHoldPan = fAim;   // (cplcy.h: the follow camera's pan held while she aims)
    if (s_fit.palo == paloPlayer)
        if (ACTOR* pa = PactorFromLo(paloPlayer))
        {
            const PCCHARFIT& f = s_fit.fit;
            pa->sHeight = f.sCamHeight + (c_sAimCamHeight - f.sCamHeight) * s_uAimCam;
            g_uPcCmDist = f.uCamDist * (1.0f + (c_uAimCamNear - 1.0f) * s_uAimCam);
        }
    {
        float dRad = rad - s_pup.radPrev;
        while (dRad > 3.14159265f) dRad -= 6.2831853f;
        while (dRad < -3.14159265f) dRad += 6.2831853f;
        s_pup.radPrev = rad;
        const float w = dt > 0.0f && std::fabs(dRad) < 1.5f ? dRad / dt : 0.0f;
        s_pup.wTurn += (w - s_pup.wTurn) * std::min(1.0f, dt * 15.0f);
    }
    // Square: her pistol shot. Puppet only: the animation (the player's own attack does the hitting); with her moveset
    // Sly's input calls PcCarmFire (the shot thrown below).
    const JOYPS2& joy = JoyFor(2);
    if (!fMoveset && (joy.grfbtnPressed & GrfbtnPs2FromLibpad(BTN_Square)) && def.oidAttack >= 0)
        if (ASEG* p = PasegOf(def.oidAttack))
        {
            s_pup.dtAttack = PasegdOf(p)->tMax;
            s_pup.pupm = PUPM_Attack;
            PlayPuppetAnim(p, false, true);
        }
    if (s_pup.dtAttack > 0.0f) s_pup.dtAttack -= dt;
    if (s_pup.dtAttack <= 0.0f && !s_pup.pasegaUpper) s_fShotPending = false;
    // her moveset: a jump of hers (from its start: the high jump's crouching launch, else her leap), her landing
    const bool fAirPrev = s_pup.pupm == PUPM_Air;
    if (fMoveset && s_fJumpNew && s_pup.dtAttack <= 0.0f)
    {
        s_fJumpNew = false;
        s_pup.dtAirHold = 0.2f;
        // (her double jump: the flip at the end of her leap)
        s_pup.fFlip = s_fJumpDouble && !FGuardDef(def);
        PlayPuppetAnim(PasegOf(s_fJumpHigh && def.oidHigh >= 0 ? def.oidHigh : def.oidJump), false, true, s_pup.fFlip ? c_tFlipStart : 0.0f);
        s_pup.pupm = PUPM_Air;
    }
    if (fMoveset && fAirPrev && fFloor && s_pup.dtAirHold <= 0.0f && s_pup.dtAttack <= 0.0f && def.oidLand >= 0)
        if (ASEG* p = PasegOf(def.oidLand))
        {
            s_pup.dtLand = std::min(0.3f, PasegdOf(p)->tMax);
            PlayPuppetAnim(p, false, true);
        }
    if (s_pup.dtLand > 0.0f) s_pup.dtLand -= dt;
    if (fFloor && s_pup.dtAirHold <= 0.0f) s_pup.fFlip = false;
    if (s_pup.dtAirHold > 0.0f) s_pup.dtAirHold -= dt;
    PUPM pupm;
    if (s_pup.dtAttack > 0.0f) pupm = PUPM_Attack;
    else if (s_pup.dtAir > 0.12f || s_pup.dtAirHold > 0.0f) pupm = PUPM_Air;
    else if (fMoveset && s_pup.dtLand > 0.0f && s_pup.sSpeed < 150.0f) pupm = PUPM_Land;
    else if (fAim) pupm = PUPM_Aim;
    else if (fMoveset && s_pup.sSpeed < 40.0f && std::fabs(s_pup.wTurn) > 2.5f && def.oidTurnL >= 0) pupm = PUPM_Turn;
    else if (s_pup.sSpeed < 40.0f) pupm = PUPM_Idle;
    else if (s_pup.sSpeed < 0.5f * (def.sWalk + def.sRun)) pupm = PUPM_Walk;
    else pupm = PUPM_Run;
    float svt = 1.0f;
    if (FPrint() && int(g_clock.t * 60.0f) % 60 == 0)
    {
        // (and the records' heights above the feet: hers, the hidden player's)
        auto zTop = [&](const ALO* palo) { float z = -1e9f; for (const auto& pxf : palo->axf) z = std::max(z, pxf->matWorld[3].z - posFeet.z); return z; };
        std::printf("pc character: t %.2f feet %.0f %.0f %.0f heading %.1f speed %.0f floor %d motion %d top %.0f player top %.0f fade %.2f drawn %d camera %.1f aim %d\n", g_clock.t, posFeet.x, posFeet.y,
                    posFeet.z, glm::degrees(rad), s_pup.sSpeed, int(fFloor), int(pupm), zTop(s_pup.proot), zTop(paloPlayer), paloPlayer->gFade, int(s_pup.fPlayerDrawn),
                    glm::degrees(std::atan2(g_cm.mat[0].y, g_cm.mat[0].x)), int(fAim));
    }
    static const int s_oidForce = std::getenv("SLY2N_PCCHARANIM") ? std::atoi(std::getenv("SLY2N_PCCHARANIM")) : -1;
    static const char* s_szCycle = std::getenv("SLY2N_PCCHARANIMS");   // (debug: oid,oid,..: each for 40 frames from frame 360)
    const int s_cstep = g_iframeMain;
    if (s_szCycle && s_cstep >= 360)
    {
        std::vector<int> a;
        for (const char* p = s_szCycle; *p;) { a.push_back(std::atoi(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
        static const int s_cn = std::getenv("SLY2N_PCCHARANIMSN") ? std::max(1, std::atoi(std::getenv("SLY2N_PCCHARANIMSN"))) : 40;   // (frames each)
        const int i = (s_cstep - 360) / s_cn;
        if (i < int(a.size()))
        {
            if ((s_cstep - 360) % s_cn == 0) std::printf("pc character: step %d animation %d\n", s_cstep, a[size_t(i)]);
            PlayPuppetAnim(PasegOf(a[size_t(i)]), true);
        }
    }
    else if (s_oidForce == -2) {}   // (debug: SLY2N_PCCHARANIM=-2: the rest pose)
    else if (s_oidForce >= 0) PlayPuppetAnim(PasegOf(s_oidForce), true);
    else
    {
        switch (pupm)
        {
        case PUPM_Idle: PlayPuppetAnim(PasegOf(def.oidIdle), true); break;
        case PUPM_Walk:
            PlayPuppetAnim(PasegOf(def.oidWalk), true);
            svt = std::clamp(s_pup.sSpeed / std::max(def.sWalk, 1.0f), 0.3f, 1.6f);   // (her slow walk: slowed down)
            break;
        case PUPM_Run:
            PlayPuppetAnim(PasegOf(def.oidRun), true);
            svt = std::clamp(s_pup.sSpeed / std::max(def.sRun, 1.0f), 0.6f, 1.8f);
            break;
        case PUPM_Air:
        {
            // the jump from its start when she leaves the floor (a bit faster: Sly's jumps are short), then the fall's pose
            ASEG* pasegHigh = def.oidHigh >= 0 ? PasegOf(def.oidHigh) : nullptr;
            const bool fJumpAnim = s_pup.pasegCur && (s_pup.pasegCur == PasegOf(def.oidJump) || s_pup.pasegCur == pasegHigh);
            // (her moveset: the fall's pose once past the top, not the dive the leap animations end in)
            const bool fFalling = fMoveset && VelAloPos(paloPlayer).z < -150.0f && s_pup.dtAirHold <= 0.0f;
            // (her double jump's flip plays to its end, at its own speed: then the fall's pose)
            const bool fFlip = s_pup.fFlip && s_pup.pasega && s_pup.pasegCur == PasegOf(def.oidJump);
            if (s_pup.pupm != PUPM_Air) PlayPuppetAnim(PasegOf(def.oidJump), false, true);
            else if (fFlip)
            {
                if (s_pup.pasega->t >= c_tFlipEnd) { s_pup.fFlip = false; PlayPuppetAnim(PasegOf(def.oidFall), false); }
            }
            else if (s_pup.pasega && fJumpAnim && (fFalling || s_pup.pasega->t >= PasegdOf(s_pup.pasegCur)->tMax))
                PlayPuppetAnim(PasegOf(def.oidFall), false);
            if (s_pup.pasegCur == PasegOf(def.oidJump)) svt = fFlip ? 1.0f : 1.5f;
            break;
        }
        case PUPM_Land: break;   // (the landing plays out)
        case PUPM_Aim:
        {
            // her aim mode: standing, her pistol-forward stance; moving, her chase's run cycle toward the strafe's
            // side in her frame (forward / back / left / right), at the speed's share of its own 600 / s
            const glm::vec2 v = s_pup.vGround;
            const float sFwd = v.x * std::cos(rad) + v.y * std::sin(rad), sLeft = -v.x * std::sin(rad) + v.y * std::cos(rad);
            int oid = c_oidAimIdle;
            if (s_pup.sSpeed >= 40.0f)
            {
                oid = std::fabs(sFwd) >= std::fabs(sLeft) ? (sFwd > 0.0f ? c_oidStrafeF : c_oidStrafeB) : (sLeft > 0.0f ? c_oidStrafeL : c_oidStrafeR);
                svt = std::clamp(s_pup.sSpeed / 600.0f, 0.5f, 1.4f);
            }
            ASEG* p = PasegOf(oid);
            PlayPuppetAnim(p ? p : PasegOf(def.oidIdle), true);
            break;
        }
        case PUPM_Turn:
            // turning in place: her side step toward the turn (left: a positive turn), sped up by the turn's rate
            PlayPuppetAnim(PasegOf(s_pup.wTurn > 0.0f ? def.oidTurnL : def.oidTurnR), true);
            svt = std::clamp(std::fabs(s_pup.wTurn) / 6.0f, 0.6f, 1.4f);
            break;
        default: break;
        }
        s_pup.pupm = pupm;
    }
    AdvancePuppetAnim(dt, svt);
    AdvancePuppetUpper(dt);
    // her records posed by the animation (its controllers on them), then placed: her root record at the player's feet
    for (ALO* palo : s_pup.apaloTree) UpdateAloXfs(palo, g_clock.t, dt);
    ALO* proot = s_pup.proot;
    const float c = std::cos(rad), s = std::sin(rad);
    glm::mat4 mat(1.0f);
    mat[0] = glm::vec4(c, s, 0.0f, 0.0f);
    mat[1] = glm::vec4(-s, c, 0.0f, 0.0f);
    mat[3] = glm::vec4(posFeet, 1.0f);
    // (debug: SLY2N_PCCHARBESIDE=d: she stands d units to the player's left, the player drawn too: a size comparison)
    static const float s_dBeside = std::getenv("SLY2N_PCCHARBESIDE") ? float(std::atof(std::getenv("SLY2N_PCCHARBESIDE"))) : 0.0f;
    if (s_dBeside != 0.0f) mat[3] += glm::vec4(-s, c, 0.0f, 0.0f) * s_dBeside;
    if (!proot->axf.empty()) proot->axf[0]->mat = mat;
    UpdateAloHierarchyWorlds(proot);
    // (debug: SLY2N_PCCHARFEET=1: each record's place in her frame (x ahead, y left, z up from her feet), for the
    // stride measurements that set her cycles' ground speeds)
    if (static const bool s_fFeet = std::getenv("SLY2N_PCCHARFEET") != nullptr; s_fFeet && s_pup.pasega)
    {
        const glm::mat4 inv = glm::inverse(mat);
        std::printf("pc feet: aseg %d t %.3f", s_pup.pasegCur ? s_pup.pasegCur->oid : -1, s_pup.pasega->t);
        for (size_t i = 0; i < proot->axf.size(); ++i)
        {
            const glm::vec3 v = glm::vec3(inv * proot->axf[i]->matWorld[3]);
            std::printf(" | %zu %.1f %.1f %.1f", i, v.x, v.y, v.z);
        }
        std::printf("\n");
    }
    // a guard's attack: its hits and shots at their times in the animation (pcguard.cpp)
    if (fMoveset && FGuardDef(def)) UpdatePcGuardAttack(psw, paloPlayer);
    // her shot thrown at its time in the animation (her hand's muzzle point where the pose put it)
    if (fMoveset && !FGuardDef(def) && s_fShotPending &&
        ((s_pup.pasega && s_pup.pasegCur == PasegOf(def.oidAttack) && s_pup.pasega->t >= def.tShot) ||
         (s_pup.pasegaUpper && s_pup.pasegUpper == PasegOf(def.oidAttack) && s_pup.pasegaUpper->t >= def.tShot)))
    {
        s_fShotPending = false;
        ThrowShot(psw, paloPlayer);
    }
    // (the upper body's shot over: its records back to the locomotion)
    if (s_pup.pasegaUpper && s_pup.pasegaUpper->t >= PasegdOf(s_pup.pasegaUpper->paseg)->tMax) StopPuppetUpper();
}

// ------------------------------------------------------------ Sly's outfit
struct SLYSWAP
{
    ALO* palo = nullptr;
    std::shared_ptr<GLOBSET> pglobsetOrig;
    std::vector<std::vector<GLOBBONEBIND>> aabindOrig;
    bool fBound = false;
};
std::vector<SLYSWAP> s_aswap;
uint32_t s_serialSwap = 0;

void RestoreSlyOutfit(SW* psw)
{
    if (psw && psw->serial == s_serialSwap)
        for (SLYSWAP& w : s_aswap)
        {
            w.palo->pglobset = w.pglobsetOrig;
            if (w.fBound) SetAloBoneBindings(w.palo, w.aabindOrig);
        }
    s_aswap.clear();
    s_fSceneRebuild = true;
}

// The model's mesh put on each of the world's Slys (the main object only: his parts' meshes, the cane's and the rest,
// are the same in every world): a copy of its glob set with the outfit's loaded group flags, and its rest-pose bone
// bindings re-pointed at the same records of the Sly it goes on.
void ApplySlyOutfit(SW* psw, int idef)
{
    const PCCHARDEF& def = Adef()[idef];
    ALO* pmodel = PaloFindModel(def.iwrec, def.oid);
    if (!pmodel || !pmodel->pglobset) return;
    s_serialSwap = psw->serial;
    const auto* paabindModel = PaabindKept(pmodel);
    for (const auto& up : psw->aploAll)
    {
        if (!FCidDerivedFrom(up->Cid(), 0x35)) continue;
        ALO* psly = static_cast<ALO*>(up.get());
        // the bones by object and record index: the same skeleton (the same parts, the same records) is required
        std::vector<std::vector<GLOBBONEBIND>> aabind;
        bool fOk = true;
        if (paabindModel)
        {
            aabind = *paabindModel;
            for (auto& abind : aabind)
                for (GLOBBONEBIND& b : abind)
                {
                    if (!b.pxfBone || !fOk) continue;
                    const ALO* pboneSrc = b.pxfBone->palo;
                    ALO* pboneDst = nullptr;
                    if (pboneSrc == pmodel) pboneDst = psly;
                    else if (pboneSrc && FLoInParentChain(pboneSrc, pmodel))
                        for (const auto& plo : psw->aploAll)
                            if (plo->oid == pboneSrc->oid && FCidDerivedFrom(plo->Cid(), 0x30) && FLoInParentChain(plo.get(), psly))
                            {
                                pboneDst = static_cast<ALO*>(plo.get());
                                break;
                            }
                    int ixf = -1;
                    if (pboneSrc)
                        for (size_t i = 0; i < pboneSrc->axf.size(); ++i)
                            if (pboneSrc->axf[i].get() == b.pxfBone) ixf = int(i);
                    if (!pboneDst || ixf < 0 || pboneDst->axf.size() != pboneSrc->axf.size()) fOk = false;
                    else b.pxfBone = pboneDst->axf[size_t(ixf)].get();
                }
        }
        if (!fOk)
        {
            std::printf("pc character: Sly %d: the outfit's skeleton differs, not changed\n", psly->oid);
            continue;
        }
        SLYSWAP w;
        w.palo = psly;
        w.pglobsetOrig = psly->pglobset;
        if (const auto* p = PaabindOfAlo(psly)) { w.aabindOrig = *p; w.fBound = true; }
        // (the loaded flags: the object's own run-time flags, which its animations set, still win)
        auto pglobset = std::make_shared<GLOBSET>(*pmodel->pglobset);
        for (GLOB& g : pglobset->aglob)
        {
            g.pgl = nullptr;
            for (SUBGLOB& sg : g.asubglob) sg.pgpu = nullptr;
            for (const int* p = def.an0Show; *p != -1; ++p) if (g.n0 == *p) g.grfGroup &= ~0x800u;
            for (const int* p = def.an0Hide; *p != -1; ++p) if (g.n0 == *p) g.grfGroup |= 0x800u;
        }
        pglobset->palo = psly;
        if (FPrint())
        {
            std::printf("pc character: Sly %d: %zu groups -> %zu\n", psly->oid, w.pglobsetOrig ? w.pglobsetOrig->aglob.size() : 0,
                        pglobset->aglob.size());
            if (w.pglobsetOrig)
                for (const GLOB& g : w.pglobsetOrig->aglob)
                    std::printf("  own n0 %d shown %d bones %zu vtx %zu\n", g.n0, !(g.grfGroup & 0x800), g.abone.size(), g.avtx.size());
            for (const GLOB& g : pglobset->aglob)
                std::printf("  outfit n0 %d shown %d bones %zu vtx %zu\n", g.n0, !(g.grfGroup & 0x800), g.abone.size(), g.avtx.size());
        }
        psly->pglobset = pglobset;
        if (paabindModel) SetAloBoneBindings(psly, aabind);
        s_aswap.push_back(std::move(w));
    }
    s_fSceneRebuild = true;
}

// ------------------------------------------------------------ her shot's projectiles
// Three copies of her world's projectile template in the running world (out of it until thrown): made with the world's
// load (before its start) or, chosen while it runs, at once (started then: pcloader.h PloPcCloneIntoWorld).
void MakeShots(SW* psw, int idef)
{
    s_apaloShot.clear();
    s_serialShot = 0;
    if (!psw || idef < 0 || Adef()[idef].oidProj < 0) return;
    LO* ptmpl = PloKeptFind(Adef()[idef].iwrec, Adef()[idef].oidProj, 0x4F);
    if (!ptmpl) return;
    for (int i = 0; i < c_cShot; ++i)
    {
        LO* plo = PloPcCloneIntoWorld(psw, ptmpl, false);
        if (plo && FCidDerivedFrom(plo->Cid(), 0x30)) s_apaloShot.push_back(static_cast<ALO*>(plo));
    }
    s_serialShot = psw->serial;
    if (FPrint()) std::printf("pc character: %zu shots of %d made in the world\n", s_apaloShot.size(), ptmpl->oid);
}

bool FShotsHere(SW* psw) { return psw && !s_apaloShot.empty() && s_serialShot == psw->serial; }

// a guard's projectiles and effects in the running world (pcguard.cpp), unless made there already
void MakeGuardShots(SW* psw, int idef)
{
    if (!psw || idef < 0 || FPcGuardProjReady(psw, Adef()[idef])) return;
    PcGuardOnWorldLoaded(psw, &Adef()[idef], PswKeptOf(idef), PaloFindModel(Adef()[idef].iwrec, Adef()[idef].oid));
}

// The choices waiting for their kept world, applied once it is ready (the running world unchanged meanwhile).
void ApplyWanted(SW* psw)
{
    if (s_idefPuppetWant >= 0)
    {
        const PCKWS k = PckwsKeptWorld(Adef()[s_idefPuppetWant].iwrec);
        if (k == PCKWS_Failed) s_idefPuppetWant = -2;
        else if (k == PCKWS_Ready)
        {
            const int idef = s_idefPuppetWant;
            s_idefPuppetWant = -2;
            if (FGuardDef(Adef()[idef])) MakeGuardShots(psw, idef);
            else if (Adef()[idef].oidProj >= 0 && !FShotsHere(psw)) MakeShots(psw, idef);
            if (FBeginPuppet(idef))
            {
                s_idefPuppet = idef;
                s_fSceneRebuild = true;
                std::printf("pc character: playing as %s\n", Adef()[idef].pchzName);
            }
            else std::printf("pc character: %s: the model was not found\n", Adef()[idef].pchzName);
        }
    }
    if (s_idefSlyWant >= 0)
    {
        const PCKWS k = PckwsKeptWorld(Adef()[s_idefSlyWant].iwrec);
        if (k == PCKWS_Failed) s_idefSlyWant = -2;
        else if (k == PCKWS_Ready)
        {
            s_idefSly = s_idefSlyWant;
            s_idefSlyWant = -2;
            ApplySlyOutfit(psw, s_idefSly);
        }
    }
}
}  // namespace


// ------------------------------------------------------------ Carmelita's moveset (the hooks)
bool FPcCarmMovesetOn() { return s_fMoveset; }
void SetPcCarmMoveset(bool f) { s_fMoveset = f; }
bool FPcCarm(const LO* plo) { return s_idefPuppet >= 0 && FMovesetPlayer(plo); }
float PcCarmMoveScale(float sGait, float uJoy, bool fR1)
{
    if (s_idefPuppet < 0) return 1.0f;
    const PCCHARDEF& def = Adef()[s_idefPuppet];
    if (FGuardDef(def)) return PcGuardRunScale(def, fR1);
    // (a scripted move, the pad at rest (a world's walk-in, a cutscene's): Sly's own speed, so it ends where it should)
    if (g_joyps2.uDeflect < 0.1f || sGait < 1.0f) return 1.0f;
    // As Sly's gait (its speed by the stick: a slight push creeps, a full one runs): her walk's speed by the stick (a
    // slight push a slow walk, at least c_uSlowWalk of it; a full push her walk); R1 held: Sly's own R1 run by the stick
    // (round 3: the gang's running speed, c_sRunSly; never slower than her walk at that push). Her animation follows the
    // speed (StepPuppet: the walk slowed down, her run cycle at the speed's share of its 600 / s).
    const float u = std::clamp(uJoy, 0.0f, 1.0f);
    const float sWalk = def.sWalk * std::max(u, c_uSlowWalk);
    const float s = fR1 ? std::max(c_sRunSly * u, sWalk) : sWalk;
    if (FPrint() && fR1 && g_iframeMain % 60 == 0) std::printf("pc character: run: Sly's gait %.0f at %.2f -> %.0f\n", sGait, u, s);
    return s / sGait;
}
bool FPcGuardMoves() { return s_idefPuppet >= 0 && FGuardDef(Adef()[s_idefPuppet]); }
bool FPcCarmCanJump() { return !FPcGuardMoves() || PcGuardJumpHeight(Adef()[s_idefPuppet]) > 0.0f; }
bool FPcCarmCircle(ALO* palo)
{
    if (!FPcCarm(palo)) return false;
    if (!FPcGuardMoves()) return true;   // (Carmelita: her high jump)
    if (s_pup.dtAttack <= 0.0f) PcGuardAttack(palo, 1);
    return false;
}
bool FPcCarmShooting() { return s_idefPuppet >= 0 && s_pup.dtAttack > 0.0f; }

bool FPcCarmDoubleJump(const LO* plo) { return FPcCarm(plo) && !FPcGuardMoves(); }

void PcCarmJumped(ALO* palo, bool fHigh, bool fDouble)
{
    if (!FPcCarm(palo)) return;
    const SOST* ps = Psost(palo);
    const float g = ps && ps->dvGravity.z < 0.0f ? -ps->dvGravity.z : 2000.0f;
    glm::vec3 v = VelAloPos(palo);
    const float h = FPcGuardMoves() ? PcGuardJumpHeight(Adef()[s_idefPuppet]) : fDouble ? c_hJumpDouble : fHigh ? c_hJumpHigh : c_hJump;
    v.z = std::sqrt(2.0f * g * h);
    SetAloVelocityTo(palo, v);
    s_fJumpNew = true;
    s_fJumpHigh = fHigh;
    s_fJumpDouble = fDouble;
    s_pup.dtAttack = 0.0f;
    // (a jump from the ground leaves her aim mode, carm-4: the aim toggle's own way out of it; a fall keeps it)
    if (!fDouble) s_fAimOn = false;
    if (FPrint()) std::printf("pc character: %s, vz %.0f (gravity %.0f)\n", fDouble ? "double jump" : fHigh ? "high jump" : "jump", v.z, g);
}

void PcCarmFire(ALO* palo)
{
    if (!FPcCarm(palo) || s_pup.dtAttack > 0.0f || s_pup.pasegaUpper) return;
    if (FGuardDef(Adef()[s_pup.idef])) { PcGuardAttack(palo, 0); return; }   // (a guard: its primary attack)
    const PCCHARDEF& def = Adef()[s_pup.idef];
    ASEG* p = def.oidAttack >= 0 ? PasegOf(def.oidAttack) : nullptr;
    if (!p) return;
    STEP* pstep = PstepFromLo(palo);
    s_radShot = pstep ? RadStepHeading(pstep) : 0.0f;
    // her aim mode: at the crosshair (its guard, else its point), facing as she is
    s_fShotAimed = FAimLive();
    s_posShotAim = s_posAim;
    s_ploShotTarget = s_fShotAimed ? s_ploAimLock : PloShotTarget(g_psw, palo, s_radShot);
    // walking, running or strafing on the ground (or aiming): the shot on her upper body only, her legs keep their
    // cycle and the player keeps moving (it froze her legs: the whole body's shot)
    if (pstep && pstep->pxpFloor && (s_pup.sSpeed >= 40.0f || s_fShotAimed) && s_pup.proot && s_pup.proot->axf.size() > c_ixfCarmUpper)
    {
        PlayPuppetUpper(p);
        if (s_pup.pasegaUpper)
        {
            s_fShotPending = true;
            return;
        }
    }
    // on the ground she turns to the guard aimed at
    if (!s_fShotAimed && s_ploShotTarget && pstep && pstep->pxpFloor)
    {
        const glm::vec3 d = PosAloCenter(static_cast<ALO*>(s_ploShotTarget)) - PosAloCenter(palo);
        s_radShot = std::atan2(d.y, d.x);
        SetStepPhysHeading(palo, s_radShot);
    }
    s_pup.dtAttack = PasegdOf(p)->tMax;
    s_pup.pupm = PUPM_Attack;
    s_fShotPending = true;
    PlayPuppetAnim(p, false, true);
}

bool PcCarmAimInput(ALO* palo, bool fTriangle, int state)
{
    // A toggle (it was held): Triangle turns it on and off. It goes off by itself on a jump from the ground
    // (PcCarmJumped), in any state she does not aim in (an interaction, damage, a cutscene's ...), when her player's
    // input did not run the step before (a pause, a world change, a cutscene), and with the guest or her moves off.
    const bool fCan = FPcCarm(palo) && !FPcGuardMoves() && g_psw;
    const bool fState = state == 0 || state == 1 || state == 2 || state == 10 || state == 12;
    const bool fWas = s_fAimOn;
    if (!fCan || !fState || s_iframeAim != g_iframeMain - 1) s_fAimOn = false;
    if (fTriangle && !s_fTrianglePrev && fCan && fState) s_fAimOn = !fWas;
    s_fTrianglePrev = fTriangle;
    const bool f = s_fAimOn;
    if (f != fWas && FPrint()) std::printf("pc character: aim mode %s\n", f ? "on" : "off");
    s_fAim = f;
    s_iframeAim = g_iframeMain;
    if (f) UpdateAim(g_psw, palo);
    else s_ploAimLock = nullptr;
    return f;
}
bool FPcCarmAiming() { return FAimLive(); }
void PcCarmFaceAim(ALO* palo)
{
    // the heading the step's physics turns to, and the body turned there now (quickly, 20 / s): standing, the
    // step's own turn does not run (she kept facing where she stood, e.g. toward the camera, and her strafes looked
    // the wrong way round)
    SetStepPhysHeading(palo, s_radAim);
    STEP* pstep = PstepFromLo(palo);
    if (!pstep) return;
    const float h = RadStepHeading(pstep);
    float d = s_radAim - h;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    const float hNew = h + d * std::min(1.0f, g_clock.dt * 20.0f);
    const float c = std::cos(hNew), sn = std::sin(hNew);
    SetStepRotation(pstep, glm::mat3(glm::vec3(c, sn, 0.0f), glm::vec3(-sn, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
}
float PcCarmAimHeading() { return s_radAim; }
glm::vec3 PcCarmStrafeVel(float uJoy, float radJoy, bool fR1)
{
    const float s = (fR1 ? c_sStrafeFast : c_sStrafe) * std::clamp(uJoy, 0.0f, 1.0f);
    // (the stick's direction from her heading, in the step's velocity frame, which points the other way: measured
    // (SLY2N_STRAFEDBG), a velocity (0, -420) moved her to the camera's left and (-420, 0) forward; "her
    // strafing seems inverted": both directions were)
    const float rad = radJoy - s_radAim;
    return -glm::vec3(std::cos(rad) * s, std::sin(rad) * s, 0.0f);
}

bool FPcCarmCrosshair() { return FAimLive() && s_pup.ploPlayer && s_pup.fPlayerDrawn; }

void DrawPcCarmCrosshair()
{
    // the picture's NDC on the 2D canvas (640 x 448 of the picture's 4:3 part; the canvas's own scale on wide ones)
    const glm::vec2 c(((s_ndcCross.x - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f,
                      (1.0f - (s_ndcCross.y - g_display.uiOffset.y) / g_display.uiScale.y) * 224.0f);
    const bool fLock = s_ploAimLock != nullptr;
    // GS colours (0x80 = 1): white, locked on a guard her shock's red; a dark outline under it
    const glm::vec4 rgba = fLock ? glm::vec4(128, 36, 24, 128) : glm::vec4(128, 128, 128, 120), rgbaDark(0, 0, 0, 80);
    const float r = fLock ? 16.0f : 13.0f, w = 3.0f;
    constexpr float z = 0.9995f;
    g_hud.DrawRing(c.x, c.y, r + 1.5f, r - w - 1.5f, 1.0f, rgbaDark, z - 1e-4f);
    g_hud.DrawRing(c.x, c.y, r, r - w, 1.0f, rgba, z);
    // four ticks out of the ring (the width of a canvas pixel of height corrected as DrawRing does: 480 / 448)
    const float kx = 448.0f / 480.0f;
    const float g0 = r + 3.0f, g1 = r + 9.0f;
    const glm::vec2 adir[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const glm::vec2& d : adir)
    {
        const glm::vec2 a = c + glm::vec2(d.x * g0 * kx, d.y * g0), b = c + glm::vec2(d.x * g1 * kx, d.y * g1);
        g_hud.FillScreenLine(a, b, w + 2.0f, rgbaDark, z - 1e-4f);
        g_hud.FillScreenLine(a, b, w, rgba, z);
    }
    g_hud.FillScreenRect(c.x - 1.0f, c.y - 1.0f, c.x + 1.0f, c.y + 1.0f, rgba, z);
}

bool FPcCarmShotHits(const LO* pproj, const LO* po)
{
    if (FPcGuardProjectile(pproj)) return !(s_pup.ploPlayer && po && PloRoot(const_cast<LO*>(po)) == PloRoot(s_pup.ploPlayer));
    if (s_apaloShot.empty() || std::find(s_apaloShot.begin(), s_apaloShot.end(), pproj) == s_apaloShot.end()) return true;
    return !(s_pup.ploPlayer && po && PloRoot(const_cast<LO*>(po)) == PloRoot(s_pup.ploPlayer));
}

void PcCharOnWorldLoaded(SW* psw)
{
    // (the old world's Sly is gone with it: its fit is dropped, made again on the new one's first step)
    s_fit.palo = nullptr;
    s_fit.asoe.clear();
    g_ploPcCmDist = nullptr;
    g_uPcCmDist = 1.0f;
    const int idef = s_idefPuppetWant >= 0 ? s_idefPuppetWant : s_idefPuppet;
    s_apaloShot.clear();
    s_serialShot = 0;
    PcGuardOnWorldLoaded(psw, nullptr, nullptr, nullptr);   // (the old world's guard projectiles are gone)
    if (idef >= 0 && FKeptReady(idef) && FGuardDef(Adef()[idef])) MakeGuardShots(psw, idef);
    else if (idef >= 0 && FKeptReady(idef)) MakeShots(psw, idef);
}

const PCCHARFIT& PcCharFit(bool* pfApplied)
{
    if (pfApplied) *pfApplied = s_fit.palo != nullptr;
    return s_fit.fit;
}

int CPcCharDefs() { return CDef(); }
const PCCHARDEF& PcCharDef(int idef) { return Adef()[std::clamp(idef, 0, CDef() - 1)]; }
int IdefPcCharPuppet() { return s_idefPuppet; }
int IdefPcCharSlyOutfit() { return s_idefSly; }
bool FPcCharPending() { return s_idefPuppetWant != -2 || s_idefSlyWant != -2; }
bool FPcCharLoaded(int idef) { return idef >= 0 && idef < CDef() && FKeptReady(idef); }
int IdefPcCharPuppetWanted() { return s_idefPuppetWant >= 0 ? s_idefPuppetWant : -1; }
int IdefPcCharSlyOutfitWanted() { return s_idefSlyWant >= 0 ? s_idefSlyWant : -1; }

void RequestPcCharPuppet(SW* psw, int idef)
{
    if (idef >= CDef() || (idef >= 0 && !FPuppetKind(Adef()[idef].kind))) return;
    s_idefPuppetWant = -2;
    if (idef < 0)
    {
        EndPuppet();
        s_idefPuppet = -1;
        s_fSceneRebuild = true;
        return;
    }
    // (its kept world loads while this one runs: the choice is applied when it is ready, the old one kept until then)
    RequestPcKeptWorld(Adef()[idef].iwrec);
    s_idefPuppetWant = idef;
    ApplyWanted(psw);
}

void RequestPcCharSlyOutfit(SW* psw, int idef)
{
    if (idef >= CDef() || (idef >= 0 && Adef()[idef].kind != PCCHARK_SlyOutfit)) return;
    s_idefSlyWant = -2;
    if (idef < 0)
    {
        RestoreSlyOutfit(psw);
        s_idefSly = -1;
        return;
    }
    RestoreSlyOutfit(psw);
    s_idefSly = -1;
    RequestPcKeptWorld(Adef()[idef].iwrec);
    s_idefSlyWant = idef;
    ApplyWanted(psw);
}

void PcCharOnWorldStarted(SW* psw)
{
    // (debug: SLY2N_PCMAPDUMP=file: the level's collision edges (class 0x31, world space) written as lines
    // "x0 y0 z0 x1 y1 z1", for a map of its doorways and passages)
    if (const char* sz = std::getenv("SLY2N_PCMAPDUMP"))
        if (FILE* pf = std::fopen(sz, "w"))
        {
            for (LO* plo : psw->mpcidaplo[0x31])
                for (SOE& e : static_cast<ALO*>(plo)->asoe)
                {
                    UpdateSoeWorld(&e);
                    for (const EDGE& ed : e.geomWorld.aedge)
                        if (ed.ipos0 < e.geomWorld.apos.size() && ed.ipos1 < e.geomWorld.apos.size())
                        {
                            const glm::vec3 a = e.geomWorld.apos[ed.ipos0], b = e.geomWorld.apos[ed.ipos1];
                            std::fprintf(pf, "%.0f %.0f %.0f %.0f %.0f %.0f\n", a.x, a.y, a.z, b.x, b.y, b.z);
                        }
                }
            std::fclose(pf);
        }
    s_aswap.clear();   // (the old world's objects are gone)
    // (tests: SLY2N_PCCHAR=puppet[,outfit] (def indices, -1 none) chosen at the first world's start, Debug keys on: the
    // models' worlds then load while it runs)
    static bool s_fEnv = false;
    if (!s_fEnv)
    {
        s_fEnv = true;
        if (const char* sz = std::getenv("SLY2N_PCCHAR"); sz && g_pcinput.fDebugKeys)
        {
            int a = -1, b = -1;
            std::sscanf(sz, "%d,%d", &a, &b);
            if (a >= 0 && a < CDef() && FPuppetKind(Adef()[a].kind)) { s_idefPuppetWant = a; RequestPcKeptWorld(Adef()[a].iwrec); }
            if (b >= 0 && b < CDef() && Adef()[b].kind == PCCHARK_SlyOutfit) { s_idefSlyWant = b; RequestPcKeptWorld(Adef()[b].iwrec); }
        }
    }
    if (s_idefSly >= 0) ApplySlyOutfit(psw, s_idefSly);
    if (s_idefPuppet >= 0)
    {
        if (FGuardDef(Adef()[s_idefPuppet])) MakeGuardShots(psw, s_idefPuppet);
        else if (!FShotsHere(psw) && Adef()[s_idefPuppet].oidProj >= 0) MakeShots(psw, s_idefPuppet);
        BindPuppet();
    }
    ApplyWanted(psw);
}

void UpdatePcChar(SW* psw, bool fDebugKeys)
{
    if (!fDebugKeys)
    {
        if (s_idefPuppet >= 0) RequestPcCharPuppet(psw, -1);
        if (s_idefSly >= 0) RequestPcCharSlyOutfit(psw, -1);
        s_idefPuppetWant = s_idefSlyWant = -2;
        return;
    }
    // (test aid: SLY2N_PCCHAROFF=frame: the puppet turned off at that frame, as the menu's Off)
    if (static const int s_iframeOff = std::getenv("SLY2N_PCCHAROFF") ? std::atoi(std::getenv("SLY2N_PCCHAROFF")) : -1;
        s_iframeOff >= 0 && g_iframeMain == s_iframeOff && s_idefPuppet >= 0)
        RequestPcCharPuppet(psw, -1);
    // (test aid: SLY2N_PCKILL=frame[,frame..]: at those frames the player takes a fatal hit (its health 0, the fatal
    // the actor's own hit path), so the death and the job's restart at its checkpoint can be checked with a guest)
    if (static const char* s_szKill = std::getenv("SLY2N_PCKILL"); s_szKill)
        for (const char* p = s_szKill; p && *p; p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : nullptr)
            if (std::atoi(p) == g_iframeMain)
                if (ACTOR* pa = PactorFromLo(PloPoCurrent(psw)))
                {
                    CTMSG ctm;   // (a hit deeper than its capacity, as a guard's: the actor's own hit path)
                    ctm.grfFilter = 0x200;
                    ctm.nState = -1000;
                    FAcceptDamage(pa, &ctm);
                    std::printf("pc character: frame %d: the player %d killed (test)\n", g_iframeMain, pa->palo ? pa->palo->oid : -1);
                }
    if (FPcCharPending()) ApplyWanted(psw);
    if (s_idefPuppet >= 0 && s_pup.proot) StepPuppet(psw);
}

void AppendPcCharSceneAlos(std::vector<ALO*>& apalo)
{
    CoopAppendSceneAlos(apalo);   // (SafeHouse-Coop: the other player, coop.h)
    if (s_idefPuppet >= 0)
        for (ALO* palo : s_pup.apaloDraw) apalo.push_back(palo);
}

int NPcCharDrawOverride(const ALO* palo)
{
    if (const int nCoop = NCoopDrawOverride(palo); nCoop >= 0) return nCoop;   // (SafeHouse-Coop, coop.h)
    if (s_idefPuppet < 0 || !s_pup.proot) return -1;
    if (std::find(s_pup.apaloDraw.begin(), s_pup.apaloDraw.end(), palo) != s_pup.apaloDraw.end())
    {
        if (auto it = s_pup.mppartfade.find(palo); it != s_pup.mppartfade.end() && it->second.g <= 0.0f) return 0;   // (a removed part)
        return s_pup.ploPlayer && s_pup.fPlayerDrawn ? 1 : 0;
    }
    static const bool s_fBeside = std::getenv("SLY2N_PCCHARBESIDE") != nullptr;   // (debug: the player drawn too)
    if (s_pup.ploPlayer && FLoInParentChain(palo, s_pup.ploPlayer)) return s_fBeside ? -1 : 0;
    return -1;
}

bool FPcCharTakeSceneRebuild()
{
    const bool f = s_fSceneRebuild | FCoopTakeSceneRebuild();   // (SafeHouse-Coop: the other player's model, coop.h)
    s_fSceneRebuild = false;
    return f;
}

// ------------------------------------------------------------ the menu's controllers (guards / bosses: pcguard.cpp)
namespace
{
constinit PCCHARCTRL s_actrl[4];   // (pcguard.cpp registers its controllers from a static initializer)
constinit bool s_afCtrl[4] = {};

// the guest characters' world hooks (pcloader.h)
struct INITPCCHAR
{
    INITPCCHAR()
    {
        PCWORLDHOOKS h;
        h.pfnLoaded = PcCharOnWorldLoaded;
        h.pfnStarted = PcCharOnWorldStarted;
        h.pfnStep = UpdatePcChar;
        RegisterPcWorldHooks(h);
    }
} s_initpcchar;
}  // namespace

void RegisterPcCharController(PCCHARK kind, const PCCHARCTRL& ctrl)
{
    if (int(kind) < 0 || int(kind) >= 4) return;
    s_actrl[int(kind)] = ctrl;
    s_afCtrl[int(kind)] = true;
}

const PCCHARCTRL* PPcCharController(PCCHARK kind)
{
    return int(kind) >= 0 && int(kind) < 4 && s_afCtrl[int(kind)] ? &s_actrl[int(kind)] : nullptr;
}

// ------------------------------------------------------------ the puppet, for pcguard.cpp
SW* PswPcPuppetKept() { return s_pup.pswKept; }
ALO* PaloPcPuppetRoot() { return s_idefPuppet >= 0 ? s_pup.proot : nullptr; }
LO* PloPcPuppetPlayer() { return s_pup.ploPlayer; }
bool FPcPuppetPlay(int oid, bool fLoop)
{
    if (!s_pup.proot) return false;
    ASEG* p = PasegOf(oid);
    if (!p) return false;
    PlayPuppetAnim(p, fLoop, true);
    return true;
}
bool FShowPcPuppetPart(int oid, bool fOn, float dtFade)
{
    bool fFound = false;
    for (ALO* palo : s_pup.apaloDraw)
    {
        if (palo->oid != oid) continue;
        fFound = true;
        PUPPET::PARTFADE& f = s_pup.mppartfade[palo];
        if (dtFade > 0.0f) f.dg = (fOn ? 1.0f : -1.0f) / dtFade;
        else f = PUPPET::PARTFADE{fOn ? 1.0f : 0.0f, 0.0f};
        if (fOn && f.g <= 0.0f && dtFade > 0.0f) f.g = 1e-3f;
    }
    return fFound;
}

float TPcPuppetAnim() { return s_pup.pasega ? s_pup.pasega->t : 0.0f; }
int OidPcPuppetAnim() { return s_pup.pasegCur ? s_pup.pasegCur->oid : -1; }
void SetPcPuppetAttack(float dt)
{
    s_pup.dtAttack = dt;
    if (dt > 0.0f) s_pup.pupm = PUPM_Attack;
}
LO* PloPcPuppetNear(int oid) { return PloFindNear(s_pup.pswKept, s_pup.proot, oid); }
