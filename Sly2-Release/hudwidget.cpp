// See hudwidget.h. Reference ports: Sly2-Reference game/m_1D93D8.cpp (0x1D9B60, 0x1DA320, 0x1DA5C8),
// m_1D93D8_c.cpp (0x1D9520, 0x1D9648, 0x1DA2E0, 0x1DA350), m_1D9DF8_p3.cpp (0x1D9968, 0x1D9DF8); the decomp's
// func_001DA4B8.c / func_001DA578.c.

#include "hudwidget.h"
#include "actioninput.h"
#include "skipcut.h"
#include "debugmenu.h"
#include "joy.h"
#include <map>
#include <bit>
#include "activate.h"
#include "actor.h"
#include "damage.h"
#include "disc.h"
#include "gadget.h"
#include "objmethod.h"

#include "clock.h"
#include "cls.h"
#include "hackmouse.h"
#include "hud.h"
#include "po.h"
#include "ref.h"
#include "savedata.h"
#include "frontend.h"
#include "menu.h"
#include "shop.h"
#include "screenrt.h"
#include "splayer.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "vecmat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

HUDPLAYER g_hudplayer;
extern int g_iframeHarness;   // development trace frame (game.cpp)

bool FFloatsNear(float g1, float g2, float gEpsilon);   // xfmath.h (its SMP clashes with vecmat.h's)

namespace
{
// orig: 0x003D9E80 (vtable 0x2D8668)
struct GAUGE
{
    float hShown = 0, hTarget = 0;     // +0x20 / +0x24 health
    float gShown = 0, gTarget = 0;     // +0x28 / +0x2C gadget power
    int state = 0;                     // +0x30
    LO* ploPlayer = nullptr;           // +0x34
    LO* plo = nullptr;                 // +0x38 the screen object 0x36B
    int aid[5] = {0x33D, 0x33E, 0x33F, 0x340, 0x341};   // +0x3C..+0x4C: idle, hit, hide / gone, back, low
};
// orig: 0x003DA0C0 (vtable 0x2CA490)
struct PROMPT
{
    int state = 0;                     // +0x10
    float t = 0;                       // +0x14
    int id = -1;                       // +0x18
    float g = 0;                       // +0x1C
    int idNext = -1;                   // +0x20
    float gNext = -1.0f;               // +0x24
    LO* plo = nullptr;                 // +0x28 the screen object 0x371
};
GAUGE s_gauge;
PROMPT s_prompt;

constexpr cmm::SMP s_smpGauge{100.0f, 0.0f, 0.5f};   // D_002C3248
constexpr float s_gNear = 1e-4f;                // 0x38D1B717

LO* PloScreen(SW* psw, OID oid)
{
    LO* plo = PloFindSwObjectByOid(psw, oid);
    return plo && FCidDerivedFrom(plo->Cid(), 0x7E) ? plo : nullptr;
}

SW* s_psw = nullptr;

// orig: 0x001C4840 (the current player) when it is a PLAYER (0x132B10 0x34).
LO* PloPlayerPo()
{
    // (no gauge under the title: the reference's title frames show none, though the boot world's Sly is current)
    if (FTitleShown()) return nullptr;
    LO* plo = s_psw ? PloPoCurrent(s_psw) : nullptr;
    return plo && FCidDerivedFrom(plo->Cid(), 0x34) ? plo : nullptr;
}
// orig: 0x001D9B60's owned-gadget table walk (ownership, irrespective of the three assigned buttons).
bool FPlayerPowerGadget(LO* player)
{
    uint32_t table = 0;
    int count = 0;
    if (FCidDerivedFrom(player->Cid(), 0x35)) { table = 0x2C2DE8; count = 18; }
    else if (FCidDerivedFrom(player->Cid(), 0x37)) { table = 0x2C3028; count = 9; }
    else if (FCidDerivedFrom(player->Cid(), 0x36)) { table = 0x2C3148; count = 8; }
    for (int i = 0; i < count; ++i)
    {
        int32_t id = -1, cost = 0;
        const uint8_t* entry = g_disc.PbElf(table + 0x20 * i, 4);
        if (!entry) continue;
        std::memcpy(&id, entry, 4);
        if (!FPowerupOwned(id)) continue;
        const uint8_t* itemCost = g_disc.PbElf(0x2BCD00 + 0x20 * id + 0xC, 4);
        if (itemCost) std::memcpy(&cost, itemCost, 4);
        if (cost > 0) return true;
    }
    return false;
}

// orig: 0x001D9968 / 0x001D9DF8: reacquire the current actor's counters after each player/world change.
bool ReadHudPlayer(LO* player)
{
    g_hudplayer = HUDPLAYER{};
    ACTOR* actor = player ? PactorFromLo(player) : nullptr;
    int* health = actor ? DmgCounterOf(actor) : nullptr;
    if (!health || DmgCapacityOf(actor) <= 0 || actor->n1300 <= 0) return false;
    g_hudplayer.cHit = *health;
    g_hudplayer.cHitMax = DmgCapacityOf(actor);
    g_hudplayer.nPower = NActorPower(actor);
    g_hudplayer.nPowerMax = actor->n1300;
    g_hudplayer.fGadgets = FPlayerPowerGadget(player);
    return true;
}
float GHealth() { return g_hudplayer.cHitMax > 0 ? float(g_hudplayer.cHit) / float(g_hudplayer.cHitMax) : 0.0f; }
float GPower() { return g_hudplayer.nPowerMax > 0 ? float(g_hudplayer.nPower) / float(g_hudplayer.nPowerMax) : 0.0f; }
bool FDead() { return g_hudplayer.cHitMax > 0 && g_hudplayer.cHit <= 0; }

void SetGaugeIds(GAUGE& p)
{
    const CID cid = p.ploPlayer->Cid();
    const int a0 = FCidDerivedFrom(cid, 0x37) ? 0x347 : FCidDerivedFrom(cid, 0x36) ? 0x342 : 0x33D;
    for (int i = 0; i < 5; ++i) p.aid[i] = a0 + i;
}

// orig: 0x001D9B60
void SetGaugeState(GAUGE& p, int s)
{
    const int old = p.state;
    if (old == s || !p.plo) return;
    if (old == 2 || old == 6) ShowScreenObj(p.plo, p.aid[0]);
    p.state = s;
    switch (s)
    {
    case 0:
    case 5: ShowScreenObj(p.plo, p.aid[2]); break;
    case 1:
        ShowScreenObj(p.plo, p.aid[0]);
        SelectScreenObjPart(p.plo, 0x34F);
        SetScreenObjFraction(p.plo, 0x34F, 1.0f - p.hShown);
        if (g_hudplayer.fGadgets)
        {
            SelectScreenObjPart(p.plo, 0x350);
            SetScreenObjFraction(p.plo, 0x350, 1.0f - p.gShown);
        }
        else ResetScreenObjPart(p.plo, 0x350);
        break;
    case 2: ShowScreenObj(p.plo, p.aid[1]); break;
    case 6: ShowScreenObj(p.plo, p.aid[3]); break;
    case 7: ShowScreenObj(p.plo, p.aid[4]); break;
    default: break;
    }
}

// orig: 0x001D9968
void InitGauge(GAUGE& p, SW* psw)
{
    p = GAUGE{};
    p.plo = PloScreen(psw, 0x36B);
    p.ploPlayer = PloPlayerPo();
    if (!ReadHudPlayer(p.ploPlayer)) p.ploPlayer = nullptr;
    if (!p.ploPlayer) return;
    p.hShown = p.hTarget = GHealth();
    p.gShown = p.gTarget = GPower();
    SetGaugeIds(p);
    if (!p.plo) return;
    ShowScreenObj(p.plo, p.aid[0]);
    SelectScreenObjPart(p.plo, 0x34F);
    SelectScreenObjPart(p.plo, 0x350);
    SetScreenObjFraction(p.plo, 0x34F, 1.0f - p.hShown);
    SetScreenObjFraction(p.plo, 0x350, 1.0f - p.gShown);
}

// orig: 0x001D9DF8
void UpdateGauge(GAUGE& p)
{
    if (!p.plo) return;
    LO* old = p.ploPlayer;
    p.ploPlayer = PloPlayerPo();
    if (!ReadHudPlayer(p.ploPlayer)) p.ploPlayer = nullptr;
    if (!p.ploPlayer)
    {
        p.hShown = p.hTarget = p.gShown = p.gTarget = 0.0f;
        if (old) SetGaugeState(p, 0);
        return;
    }
    if (old != p.ploPlayer)
    {
        SetGaugeState(p, old ? 5 : 3);
        SetGaugeIds(p);
        p.hShown = p.hTarget = GHealth();
        p.gShown = p.gTarget = GPower();
    }
    const float dt = g_clock.dt;
    switch (p.state)
    {
    case 1:
    case 6:
    case 7:
        p.hTarget = GHealth();
        p.gTarget = GPower();
        if (FDead()) { SetGaugeState(p, 0); break; }
        if (p.hTarget < p.hShown) SetGaugeState(p, 2);
        else if (p.hShown < p.hTarget) SetGaugeState(p, 3);
        else if (!FFloatsNear(p.gShown, p.gTarget, s_gNear)) SetGaugeState(p, 4);
        else if (PactorFromLo(p.ploPlayer)->n10EC == 4) SetGaugeState(p, 7);
        else if (PactorFromLo(p.ploPlayer)->n10C8 != -1) SetGaugeState(p, 6);
        else SetGaugeState(p, 1);
        break;
    case 2:
        if (!FScreenObjBusy(p.plo, p.aid[1])) SetGaugeState(p, 3);
        break;
    case 5:
        if (!FScreenObjBusy(p.plo, p.aid[2])) SetGaugeState(p, 1);
        break;
    case 0:
        p.hTarget = GHealth();
        p.gTarget = GPower();
        if (p.hShown < p.hTarget)
        {
            p.hShown = p.hTarget;
            SetScreenObjFraction(p.plo, 0x34F, 1.0f - p.hTarget);
            p.gShown = p.gTarget;
            SetScreenObjFraction(p.plo, 0x350, 1.0f - p.gTarget);
            SetGaugeState(p, 1);
        }
        break;
    case 3:
    {
        const float h = GHealth();
        p.hTarget = h;
        if (p.hShown < h) p.hShown = h;
        else p.hShown = cmm::GSmooth(p.hShown, h, dt, &s_smpGauge, nullptr);
        SetScreenObjFraction(p.plo, 0x34F, 1.0f - p.hShown);
        if (FFloatsNear(p.hShown, p.hTarget, s_gNear)) SetGaugeState(p, 1);
        break;
    }
    case 4:
    {
        const float g = GPower();
        p.gTarget = g;
        if (p.gShown < g) p.gShown = g;
        else p.gShown = cmm::GSmooth(p.gShown, g, dt, &s_smpGauge, nullptr);
        SetScreenObjFraction(p.plo, 0x350, 1.0f - p.gShown);
        if (FFloatsNear(p.gShown, p.gTarget, s_gNear)) SetGaugeState(p, 1);
        break;
    }
    default: break;
    }
}

// orig: 0x001DA5C8
void SetPromptState(PROMPT& p, int n)
{
    if (n == p.state) return;
    switch (p.state)
    {
    case 3:
        if (n == 1 && p.t == g_clock.t) n = 2;
        break;
    case 4:
        p.id = p.idNext;
        p.g = p.gNext;
        p.idNext = -1;
        p.gNext = -1.0f;
        break;
    default: break;
    }
    if (n == 0) p.id = -1;
    if (p.plo)
    {
        if (p.state == 4 && n == 2) StartScreenObjPart(p.plo, 0x310);
        switch (n)
        {
        case 4: StartScreenObjPart(p.plo, 0x312); break;
        case 3: HideScreenObj(p.plo); break;
        case 1: ShowScreenObjArgs(p.plo, 1, &p.id); break;
        default: break;
        }
    }
    p.state = n;
    p.t = g_clock.t;
}

// orig: 0x001DA350
void UpdatePrompt(PROMPT& p)
{
    if (!p.plo) return;
    switch (p.state)
    {
    case 1:
        if (NScreenObjState(p.plo) == 2) SetPromptState(p, 2);
        break;
    case 2:
        if (p.idNext != -1) SetPromptState(p, 4);
        else if (p.g == 0.0f) SetPromptState(p, 3);
        else if (p.g > 0.0f && !FScreenObjVisible(p.plo)) p.t += g_clock.dt;
        break;
    case 3:
        if (NScreenObjState(p.plo) < 2) SetPromptState(p, 0);
        break;
    case 4:
        if (FScreenObjBusy(p.plo, 0x312)) break;
        SetPromptState(p, p.idNext == -1 ? 0 : 2);
        break;
    default: break;
    }
}
}  // namespace

namespace
{
const SW* s_pswHud = nullptr;
uint32_t s_serialHud = 0;   // the generation actually bound by InitHudWidgets, independent of SW address reuse
}

// orig: 0x002113E0 (the 0x2F1 script string: the prompt's string)
std::string StrHudPromptText();
std::string StrHudPromptText() { return StrLocalized(s_pswHud, s_prompt.id); }

namespace
{
// a prompt asked for while the next world loads (before InitHudWidgets resets the widget for it: e.g. the safe
// house's select prompt from its state machine at the world start), replayed by InitHudWidgets
struct { int id = -1; float g = 0; uint32_t serial = 0; } s_promptPending;
}

void HudPrompt(int id, float g)
{
    if (!s_pswHud || s_serialHud != SW::s_serialNext)
    {
        s_promptPending = {id, g, SW::s_serialNext};
        return;
    }
    PROMPT& p = s_prompt;
    if (std::getenv("SLY2_HUDDBG")) std::printf("hud: prompt 0x%X (%g) in state %d: \"%s\"\n", id, g, p.state, StrLocalized(s_pswHud, id).c_str());
    if (id == -1)
    {
        HudPromptEnd(-1);
        return;
    }
    switch (p.state)
    {
    case 1:
        SetPromptState(p, -1);
        [[fallthrough]];
    case 0:
    case 3:
        p.id = id;
        p.g = g;
        SetPromptState(p, 1);
        break;
    case 2:
        if (p.id == id)
        {
            p.g = g;
            break;
        }
        [[fallthrough]];
    case 4:
        p.idNext = id;
        p.gNext = g;
        break;
    default: break;
    }
}

bool FHudPromptTicker(float* pt, float* pgEnd)
{
    if (s_prompt.state != 2) return false;
    *pt = g_clock.t - s_prompt.t;
    *pgEnd = s_prompt.g;
    return true;
}
void EndHudPromptTicker() { SetPromptState(s_prompt, 3); }

// orig: 0x001DA5B0 (the prompt asked for next, else the one shown)
int IdDisplayedHudPrompt() { return s_prompt.id; }
int IdHudPrompt() { return s_prompt.idNext != -1 ? s_prompt.idNext : s_prompt.id; }

void HudPromptEnd(int id)
{
    PROMPT& p = s_prompt;
    if (id == -1)
    {
        p.idNext = -1;
        p.g = 0.0f;
    }
    else if (id == p.id) p.g = 0.0f;
    else if (id == p.idNext) p.idNext = -1;
}

// orig: 0x001D9408 (gauge 0x001D9B30 and prompt 0x001DA320), called by 0x0020E068 before freeing the SW.
static LO* s_ploSaveIcon = nullptr;  // (D_002E65A0 +0x24: the save icon's screen object)

void ResetHudWidgets()
{
    s_ploSaveIcon = nullptr;
    ResetScreenObjPauseCount();  // orig 0x1D9408: also releases a pending failure restart's count
    SetGaugeState(s_gauge, 0);
    s_gauge = GAUGE{};
    g_hudplayer = HUDPLAYER{};
    s_prompt.idNext = -1;
    s_prompt.id = -1;
    s_prompt.plo = nullptr;
    SetPromptState(s_prompt, 0);
    s_psw = nullptr;
    s_pswHud = nullptr;
    s_serialHud = 0;
    s_promptPending = {};
}

void InitHudWidgets(SW* psw)
{
    ResetScreenObjs(psw);
    s_pswHud = psw;
    s_serialHud = psw->serial;
    if (std::getenv("SLY2_NOHUD")) { s_gauge = GAUGE{}; s_prompt = PROMPT{}; return; }
    s_psw = psw;
    InitMenu(psw);
    InitShop(psw);
    InitJobHelp(psw);
    StartFrontEnd(psw, g_transition.req.flags);
    InitGauge(s_gauge, psw);
    // orig: 0x001DA320 (the reset) / 0x001DA2E0
    s_prompt = PROMPT{};
    s_prompt.plo = PloScreen(psw, 0x371);
    // orig: 0x001ADDF0 / 0x001DA7D0 (the save icon D_002E65A0: screen object 0x5D4, the Thievius Raccoonus book with
    // the game's completion)
    s_ploSaveIcon = PloScreen(psw, 0x5D4);
    if (s_promptPending.serial == psw->serial && s_promptPending.id != -1) HudPrompt(s_promptPending.id, s_promptPending.g);
    s_promptPending = {};
    // debug: SLY2_HUDPROMPT=id shows a prompt; SLY2_HUDSHOW=oid[,oid...] shows screen objects (their 0x30F / 0x310)
    if (std::getenv("SLY2_HUDSHOP")) SetShopPage(0);
    if (const char* sz = std::getenv("SLY2_HUDPROMPT")) HudPrompt(int(std::strtol(sz, nullptr, 0)), -1.0f);
    if (const char* sz = std::getenv("SLY2_HUDSHOW"))
        for (const char* pch = sz; *pch;)
        {
            char* pchEnd = nullptr;
            const long oid = std::strtol(pch, &pchEnd, 0);
            if (pchEnd == pch) break;
            if (LO* plo = PloScreen(psw, OID(oid))) ShowScreenObj(plo, -1);
            pch = *pchEnd == ',' ? pchEnd + 1 : pchEnd;
        }
    if (std::getenv("SLY2_HUDDBG"))
    {
        for (LO* plo : psw->mpcidaplo[0x85])
        {
            const TASK* pt = static_cast<const TASK*>(plo);
            if (pt->idDialog5C != -1) std::printf("hud: task 0x%X prompt 0x%X f60 %d state %d \"%s\"\n", pt->oid, pt->idDialog5C, pt->f60, pt->nState, StrLocalized(psw, pt->idDialog5C).c_str());
        }
        std::printf("hud: gauge %p (player %p) state %d, prompt %p\n", (void*)s_gauge.plo, (void*)s_gauge.ploPlayer, s_gauge.state, (void*)s_prompt.plo);
    }
}

void UpdateHud(SW* psw)
{
    s_pswHud = psw;
    s_psw = psw;
    ++g_cframeScreen;
    // orig 0x001D9648: Start opens the pause menu (0x1B0290; else the press is left for the title), then the
    // widgets: the menu 0x3D9ED0, the gauge, the prompt, the card dialog, the title
    // The original Select shortcut requires R1 released. Advanced mode gives this menu its own action.
    const bool advanced = sly2actions::Active();
    if ((g_grfjoyt & 2) && !FTitleShown() && !FMenuShown() && !FShopShown() && !FGadgetMenuShown() && !FJobHelpShown() && PloPlayerPo() &&
        (advanced ? sly2actions::Pressed(sly2actions::Action::GadgetMenu)
                  : GrfbtnPressed(0x100) && (GrfbtnHeld() & 0x108) == 0x100))
    {
        GrfbtnTakePressed(advanced ? 0x107 : 0x100);
        OpenGadgetMenu(psw, PloPlayerPo());
    }
    // orig 0x1D9648 reads the gameplay pad (JoyFor(2)); a failure screen's
    // PauseGame(0) leaves confirmation available but prevents opening this menu.
    if ((g_grfjoyt & 2) && !FTitleShown() && !FMenuShown() && !FShopShown() && !FGadgetMenuShown() && !FJobHelpShown() && GrfbtnPressed(0x800) && FOpenPauseMenu(psw)) GrfbtnTakePressed(0x800);
    UpdateMenu(psw);
    UpdateShop(psw);
    UpdateJobHelp(psw);
    UpdateGadgetMenu(psw, PloPlayerPo());
    UpdateGauge(s_gauge);
    // Development-only semantic trace for comparisons with the original gauge.
    static const bool s_fMeter = std::getenv("SLY2N_HUDMETER") != nullptr;
    if (s_fMeter)
    {
        ACTOR* actor = s_gauge.ploPlayer ? PactorFromLo(s_gauge.ploPlayer) : nullptr;
        int* health = actor ? DmgCounterOf(actor) : nullptr;
        std::fprintf(stderr, "HUD_METER {\"frame\":%d,\"t\":%.9g,\"dt\":%.9g,"
                    "\"current_oid\":%d,\"current_cid\":%d,\"screen_available\":%s,"
                    "\"actual_health\":%d,\"actual_capacity\":%d,\"actual_power\":%d,\"actual_power_max\":%d,"
                    "\"hud_health\":%d,\"hud_health_max\":%d,\"hud_power\":%d,\"hud_power_max\":%d,\"hud_gadgets\":%s,"
                    "\"health_shown\":%.9g,\"health_target\":%.9g,\"power_shown\":%.9g,\"power_target\":%.9g,"
                    "\"state\":%d,\"hint10C8\":%d,\"hint10EC\":%d}\n",
                    g_iframeHarness, double(g_clock.t), double(g_clock.dt),
                    s_gauge.ploPlayer ? s_gauge.ploPlayer->oid : -1,
                    s_gauge.ploPlayer ? int(s_gauge.ploPlayer->Cid()) : -1, s_gauge.plo ? "true" : "false",
                    health ? *health : -1, actor ? DmgCapacityOf(actor) : -1,
                    actor ? NActorPower(actor) : -1, actor ? actor->n1300 : -1,
                    g_hudplayer.cHit, g_hudplayer.cHitMax, g_hudplayer.nPower, g_hudplayer.nPowerMax,
                    g_hudplayer.fGadgets ? "true" : "false",
                    double(s_gauge.hShown), double(s_gauge.hTarget), double(s_gauge.gShown), double(s_gauge.gTarget),
                    s_gauge.state, actor ? actor->n10C8 : -1, actor ? actor->n10EC : -1);
    }
    UpdatePrompt(s_prompt);
    // orig: 0x001ADEE8 (the save icon hidden once shown idle for more than a second and the card written; natively
    // the write is synchronous)
    if (s_ploSaveIcon && FScreenObjIdleLonger(s_ploSaveIcon, 1.0f)) HideScreenObj(s_ploSaveIcon);  // 0x1ADE70: vt slot 50
    UpdateFrontEnd(psw);
    UpdateScreenObjs(psw);
    static const bool s_fDbg = std::getenv("SLY2_HUDDBG") != nullptr;
    static int s_stateLast = -2;
    if (s_fDbg && s_gauge.state != s_stateLast)
    {
        s_stateLast = s_gauge.state;
        if (g_transition.ploPlayer) std::printf("hud: transition player oid 0x%X cid 0x%X\n", g_transition.ploPlayer->oid, g_transition.ploPlayer->Cid());
        std::printf("hud: t %.2f player %p gauge state %d (screen state %d, visible %d)\n", g_clock.t3C, (void*)s_gauge.ploPlayer, s_gauge.state,
                    s_gauge.plo ? NScreenObjState(s_gauge.plo) : -1, s_gauge.plo ? int(FScreenObjVisible(s_gauge.plo)) : -1);
    }
}

// orig: 0x0015DC80 module (cid 0x74, a screen fade): started by the splice method 0x10C6A8 (0x15E1D8: its start time
// +0x68, added to the world unless in it), drawn as a clear of the frame (0x15DD48: the colour +0x50 -> +0x54 over
// +0x5C s, +0x54 held +0x60 s, +0x54 -> +0x58 over +0x64 s; each channel by NLerp), removed when the three phases
// are over (0x15DCD8: vt slot 9)
namespace
{
std::map<LO*, float> s_mpfadeStart;
uint32_t s_serialFades = 0;   // (the world the fades belong to: a new world drops the old one's objects)
int NLerpFade(float g, int a, int b)
{
    const float k = 0.999f;   // 0x3F7FBE77
    if (a == b) return b;
    if (a < b) return a + int(g * ((float(b) + k) - float(a)));
    return b + int((1.0f - g) * ((float(a) + k) - float(b)));
}
void DrawFades(SW* psw)
{
    if (s_serialFades != psw->serial)
    {
        s_mpfadeStart.clear();
        s_serialFades = psw->serial;
    }
    for (auto it = s_mpfadeStart.begin(); it != s_mpfadeStart.end();)
    {
        LO* plo = it->first;
        const float d5C = std::bit_cast<float>(plo->U32Field(0, 0x5C, 0)), d60 = std::bit_cast<float>(plo->U32Field(0, 0x60, 0)),
                    d64 = std::bit_cast<float>(plo->U32Field(0, 0x64, 0));
        float dt = g_clock.t - it->second;
        if (!plo->fInWorld || d5C + d60 + d64 < dt)
        {
            if (plo->fInWorld) RemoveLo(psw, plo);   // (vt slot 9)
            it = s_mpfadeStart.erase(it);
            continue;
        }
        const uint32_t c50 = plo->U32Field(0, 0x50, 0), c54 = plo->U32Field(0, 0x54, 0), c58 = plo->U32Field(0, 0x58, 0);
        uint32_t ca = c54, cb = c54;
        float d = 1.0f;
        if (dt < d5C) { ca = c50; cb = c54; d = d5C; }
        else if ((dt -= d5C) < d60) { d = d60; }
        else { dt -= d60; ca = c54; cb = c58; d = d64; }
        const float g = d > 0.0f ? dt / d : 1.0f;
        glm::vec4 rgba;
        for (int i = 0; i < 4; ++i) rgba[i] = float(NLerpFade(g, int((ca >> (8 * i)) & 0xFF), int((cb >> (8 * i)) & 0xFF)));
        g_hud.Fade(rgba);
        ++it;
    }
}
}  // namespace

// orig: 0x0015E1D8 (a fade object (cid 0x74) started: its start time +0x68, added to the world unless in it)
void StartFadeLo(LO* plo)
{
    if (plo && !plo->fInWorld)
    {
        if (g_psw && s_serialFades != g_psw->serial) { s_mpfadeStart.clear(); s_serialFades = g_psw->serial; }
        s_mpfadeStart[plo] = g_clock.t;
        if (g_psw) AddLo(g_psw, plo);   // (vt slot 8)
    }
}

namespace
{
struct INITFADE
{
    INITFADE()
    {
        RegisterObjectMethod(0x10C6A8, [](LO* plo, const std::vector<CRef>&) {
            StartFadeLo(plo);
            return CRef(TAGK_Void);
        });
    }
} s_initfade;
}  // namespace

void DrawHud(SW* psw, int width, int height)
{
    g_hud.Begin();
    if (psw) DrawFades(psw);
    // (debugmenu.h: F11 hides the HUD for clean pictures; the game's own menus stay)
    if (!FDebugHudHidden() || FMenuShown()) DrawScreenObjs(psw);
    DrawHackCursor();   // (hackmouse.h: the hacking minigame's mouse cursor)
    DrawSkipCut();   // (skipcut.h: the hint / the indicator over everything)
    DrawSafeHousePrompt();
    g_hud.Flush(width, height);
}

bool (*g_pfnWriteSave)() = nullptr;

void HudAutoSave(bool fForce)
{
    if (std::getenv("SLY2N_SAVEPRINT"))
        std::printf("autosave? force %d slot %d pending %d episode %d\n", (int)fForce, g_islotChosen, (int)g_transition.fPending, IepisodeCur());
    if (g_islotChosen < 0) return;                 // (g_save +0xFD50: no slot chosen, nothing to save to)
    if (g_transition.fPending) return;             // (0x20E040)
    if (IepisodeCur() == 0 && !fForce) return;     // (0x210AE0)
    const CRef ref = RefGetProgress({"flags", "fAutosaveWarned"});
    const bool fWarned = (ref.m_tagk == TAGK_Bool && ref.m_bool) || (ref.m_tagk == TAGK_S32 && ref.m_n != 0);
    if (!fWarned)
    {
        ShowCardMessage(0x22, 3);   // (0x2E6550 +0x4C = 3: the save follows the warning)
        return;
    }
    // orig 0x001ABCE8 (the record into the slot, written) and 0x001ADE38 (the card icon 0x5D4 for a second)
    const bool f = FWriteSaveSlot(g_islotChosen);
    std::printf("auto-save: slot %d %s\n", g_islotChosen + 1, f ? "written" : "not written");
    if (s_ploSaveIcon) ShowScreenObj(s_ploSaveIcon, -1);  // 0x1ADE38 (vt slot 49)
}
