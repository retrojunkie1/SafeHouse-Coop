// See skipcut.h.

#include "skipcut.h"
#include "paint.h"

#include "audio.h"
#include "actor.h"
#include "activate.h"
#include "ai.h"
#include "alo.h"
#include "clock.h"
#include "cm.h"
#include "cls.h"
#include "hud.h"
#include "joy.h"
#include "menu.h"
#include "pcinput.h"
#include "porttext.h"
#include "region.h"
#include "screenobj.h"
#include "screenrt.h"
#include "scpt.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "po.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

bool g_fSkipCutFast = false;
extern int g_frameJoy;   // (joy.cpp: the step's frame)

namespace
{
constexpr float T_HOLD = 3.0f;          // seconds of Circle to skip
constexpr float T_HINT = 4.0f;          // the hint stays this long after the last button
constexpr int CSTEP_MAX = 60 * 600;     // a skip's fast-forward stops after 10 minutes of game time (a stuck scene; Ep 8's Arpeggio / Clockwerk scene 3299 is 4 min)

struct SKIPCUT
{
    bool fHint = false;         // the hint shown
    float tHint = 0.0f;         // since the last button
    float tHold = 0.0f;         // Circle held this long
    bool fFast = false;         // fast-forwarding
    int cstep = 0;              // its steps
    bool fMaskCircle = false;   // Circle held from the skip: hidden from the game until released
    int frameWait = -100;       // the last step a script waited for a button
    uint64_t idScene = 0;       // the scene whose hold / fast-forward this belongs to
    bool fAwaitRelease = false; // a Circle held across a scene boundary cannot start another skip
    int frameStop = -1;         // the step the last fast-forward ended at (the scene's end: the check's)
    uint16_t grfCarried = 0;    // buttons held into the scene (or that turn on in its first moments, e.g. a held movement
                                // key becoming the d-pad once the scene takes the pad): no hint until released
    float tScene = 0.0f;        // since the scene started
};
SKIPCUT s_skip;
SKIPCUT s_skipMovie;

bool FScptWaitsNow() { return g_frameJoy - s_skip.frameWait <= 2; }

uint64_t IdCutscene()
{
    if (FMenuShown() || !g_psw) return 0;
    uint64_t id = IdScptCutscene();
    for (int cid : {0x35, 0x36, 0x37})
        for (LO* plo : g_psw->mpcidaplo[cid])
            if (ACTOR* pa = PactorFromLo(plo); pa && pa->nF04 != -1 && pa->nF20 == 1 &&
                pa->binoc.idSkipScene && (!id || pa->binoc.idSkipScene < id))
                id = pa->binoc.idSkipScene;
    return id;
}

void StopFast()
{
    if (!s_skip.fFast) return;
    s_skip.fFast = false;
    s_skip.frameStop = g_frameJoy;
    g_fSkipCutFast = false;
    SetOutputMute(false);
    if (std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: fast-forward over after %d steps\n", s_skip.cstep);
}

void StartFast()
{
    s_skip.fFast = true;
    s_skip.cstep = 0;
    s_skip.fHint = false;
    s_skip.tHold = 0.0f;
    s_skip.fMaskCircle = true;
    s_skip.fAwaitRelease = true;
    g_fSkipCutFast = true;
    SetOutputMute(true);
    if (std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: skipping (fast-forward)\n");
}

// the state machine shared by the game and the movies: true when the skip starts now
bool FUpdateHold(SKIPCUT& s, float dtReal, uint16_t grfHeld, uint16_t grfPressed, bool fSkippable)
{
    const bool fCircle = (grfHeld & BTN_Circle) != 0;
    if (!fCircle) s.fAwaitRelease = false;
    if (!fSkippable)
    {
        s.fHint = false;
        s.tHold = 0.0f;
        return false;
    }
    if (grfPressed) { s.fHint = true; s.tHint = 0.0f; }
    else s.tHint += dtReal;
    if (s.fAwaitRelease)
    {
        s.tHold = 0.0f;
        return false;
    }
    if (fCircle && s.fHint)
    {
        s.tHint = 0.0f;
        if (g_pcinput.fInstantSkipCutscenes && (grfPressed & BTN_Circle)) return true;
        s.tHold += dtReal;
        if (s.tHold >= T_HOLD) return true;
    }
    else s.tHold = 0.0f;
    if (s.tHint > T_HINT) s.fHint = false;
    return false;
}

float DxText(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size)
{
    float w = 0.0f;
    const FONTBRX* pf = &font;
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '&' && i + 1 < text.size() && text[i + 1] != '&')
        {
            pf = text[i + 1] == '.' || !picons ? &font : picons;
            i += 2;
            continue;
        }
        size_t cb = 1;   // (a character: a byte, or the Japanese text's UTF-8 form: region.h)
        w += pf->DxFromCh(ChDecode(text, i, &cb));
        i += cb;
    }
    return w * size / 51.2f;
}

// the hint (and the indicator while Circle is held) at the bottom right of the 4:3 canvas
void DrawHint(const SKIPCUT& s)
{
    if (!s.fHint) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const std::string text = StrPort(picons ? PTX_HoldToSkip : PTX_HoldToSkipNoIcons);   // (the game's language: porttext.h)
    const float size = 24.0f;
    const float xRight = 616.0f, yTop = 412.0f;   // (canvas pixels)
    const float w = DxText(*pfont, picons, text, size);
    const float depth = 16000000.0f;   // (over every screen object)
    // (the colour offset leaves the glyphs' own alpha alone, as the game's HUD texts do (marker.cpp, dance.cpp): an
    // alpha offset made the whole glyph cells opaque, the letters' counters filled)
    const glm::vec4 white(200.0f, 200.0f, 200.0f, 0.0f);
    auto drawText = [&](const std::string& t, float px, float py, float sz, glm::vec4 off, float dz) {
        g_hud.DrawTextBox(*pfont, picons, t, px - 320.0f, (py - 224.0f) * 1.1f, sz, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                          glm::vec4(1.0f), glm::vec4(0.0f), depth + dz, false);
    };
    // a dark plate under the text
    const float hText = size * 0.909f;
    g_hud.FillScreenRect(xRight - w - 10.0f, yTop - 5.0f, xRight + 6.0f, yTop + hText + 7.0f, glm::vec4(0, 0, 0, 72), depth / 16777216.0f - 1e-4f);
    drawText(text, xRight - w, yTop, size, white, 100.0f);
    if (s.tHold > 0.0f && picons && !g_pcinput.fInstantSkipCutscenes)
    {
        // the indicator: the Circle icon, a line circling it as the hold fills
        const float hIcon = 26.0f;
        const float cx = xRight - w - 26.0f, cy = yTop + hText * 0.5f + 1.0f;
        g_hud.DrawIconGlyph(*picons, 'O', cx, cy, hIcon, depth + 200.0f);
        const float r = hIcon * 0.62f;
        g_hud.DrawRing(cx, cy, r + 3.5f, r, 1.0f, glm::vec4(0, 0, 0, 80), 0.99f);
        g_hud.DrawRing(cx, cy, r + 3.0f, r + 0.5f, s.tHold / T_HOLD, glm::vec4(250, 250, 250, 128), 0.995f);
    }
}

// (test: SLY2N_SKIPCHECK, skipcut.h) true: skip now
void WriteSkipCheckState(const std::string& file)
{
    SW* psw = g_psw;
    std::FILE* pf = psw ? std::fopen(file.c_str(), "w") : nullptr;
    if (!pf) return;
    for (LO* plo : psw->mpcidaplo[0x85])
        std::fprintf(pf, "task %d %d\n", plo->oid, static_cast<TASK*>(plo)->nState);
    if (LO* ppo = PloPoCurrent(psw))
    {
        const glm::vec3 p = PosLoWorld(ppo);
        std::fprintf(pf, "player %d %.1f %.1f %.1f\n", ppo->oid, p.x, p.y, p.z);
    }
    std::fprintf(pf, "cm %d object %d\n", g_cm.nCur, g_cm.ploCur ? g_cm.ploCur->oid : -1);
    std::fprintf(pf, "pause %d %d %d %d scene %d\n", g_gamepause.c78, g_gamepause.c7C, g_gamepause.c80, g_gamepause.c84,
                 IdCutscene() ? 1 : 0);
    int cStack = 0;
    const int idMusic = IdMusicTop(&cStack);
    std::fprintf(pf, "music %d stack %d\n", idMusic, cStack);
    std::map<int, int> mpc;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!FCidDerivedFrom(plo->Cid(), 0x30) || static_cast<ALO*>(plo)->paloParent) continue;
        const glm::vec3 p = PosLoWorld(plo);
        std::fprintf(pf, "obj %d#%d cid 0x%X in %d %.1f %.1f %.1f %s\n", plo->oid, mpc[plo->oid]++, plo->Cid(), plo->fInWorld ? 1 : 0,
                     p.x, p.y, p.z, FCidDerivedFrom(plo->Cid(), 0x39) ? "guard" : "-");
    }
    std::fclose(pf);
}

bool FUpdateSkipCheck(uint64_t id)
{
    static const char* s_sz = std::getenv("SLY2N_SKIPCHECK");
    if (!s_sz) return false;
    static int s_nScene = 0, s_off = 0, s_cAfter = 0, s_cSeen = 0, s_frameStart = -1, s_frameEnd = -1, s_cDump = 0;
    static uint64_t s_idPrev = 0, s_idWatch = 0;
    static bool s_fSkip = false, s_fInit = false;
    static std::string s_file;
    if (!s_fInit)
    {
        s_fInit = true;
        char sz[512] = {}, szMode[16] = {};
        std::sscanf(s_sz, "%d:%d:%d:%511[^:]:%15s", &s_nScene, &s_off, &s_cAfter, sz, szMode);
        s_file = sz;
        s_fSkip = !std::strcmp(szMode, "skip");
    }
    const int frame = g_frameJoy;
    if (id != s_idPrev)
    {
        s_idPrev = id;
        if (id && ++s_cSeen == s_nScene && !s_idWatch) s_frameStart = frame;
    }
    if (!s_idWatch)
    {
        if (s_frameStart < 0 || frame < s_frameStart + s_off) return false;
        if (id == 0)
        {
            std::printf("skipcheck: scene %d over before frame %d\n", s_nScene, s_frameStart + s_off);
            s_frameStart = -1;
            return false;
        }
        s_idWatch = id;
        std::printf("skipcheck: frame %d scene %llu (scene %d of the run, begun at frame %d)\n", frame,
                    static_cast<unsigned long long>(id), s_nScene, s_frameStart);
        return s_fSkip;
    }
    if (s_frameEnd < 0)
    {
        if (id == s_idWatch) return false;
        // (a skip's fast-forward ended the scene at its last step, before this frame's)
        s_frameEnd = s_fSkip && s_skip.frameStop > s_frameStart ? s_skip.frameStop : frame;
        std::printf("skipcheck: scene over at frame %d t %.3f\n", s_frameEnd, g_clock.t);
    }
    if (s_cDump < 2 && frame >= s_frameEnd + s_cAfter + 30 * s_cDump)
    {
        WriteSkipCheckState(s_file + "." + std::to_string(++s_cDump));
        std::printf("skipcheck: state %d written at frame %d\n", s_cDump, frame);
    }
    return false;
}
}  // namespace

bool FCutsceneActive()
{
    return IdCutscene() != 0;
}

uint64_t NewSkipCutScene()
{
    static uint64_t s_id = 0;
    return ++s_id;
}

void NoteScptWaitsForButton() { s_skip.frameWait = g_frameJoy; }

void UpdateSkipCut(float dtReal, uint16_t grfHeld, uint16_t grfPressed)
{
    if (s_skip.fMaskCircle && !(grfHeld & BTN_Circle) && !s_skip.fFast) s_skip.fMaskCircle = false;
    if (s_skip.fFast) return;
    const uint64_t id = IdCutscene();
    if (s_skip.idScene != id)
    {
        if (std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: scene %llu at frame %d\n", static_cast<unsigned long long>(id), g_frameJoy);
        s_skip.idScene = id;
        s_skip.fHint = false;
        s_skip.tHint = s_skip.tHold = 0.0f;
        s_skip.fAwaitRelease = (grfHeld & BTN_Circle) && !(grfPressed & BTN_Circle);
        s_skip.grfCarried = grfHeld;
        s_skip.tScene = 0.0f;
    }
    // (PC QoL) a button already held when the scene started shows no hint: only a fresh press does
    s_skip.tScene += dtReal;
    if (s_skip.tScene < 0.25f) s_skip.grfCarried |= grfHeld;
    s_skip.grfCarried &= grfHeld;
    grfPressed &= uint16_t(~s_skip.grfCarried);
    const bool fSkippable = id && !FScptWaitsNow();
    const bool fCheckSkip = FUpdateSkipCheck(id) && fSkippable;   // (test: SLY2N_SKIPCHECK)
    // (tests: SLY2N_SKIPAT=frame: the skip starts at that step, as after the hold)
    static const int s_frameSkipAt = std::getenv("SLY2N_SKIPAT") ? std::atoi(std::getenv("SLY2N_SKIPAT")) : -1;
    bool fStart = FUpdateHold(s_skip, dtReal, grfHeld, grfPressed, fSkippable);
    if (s_frameSkipAt >= 0 && g_frameJoy >= s_frameSkipAt && g_frameJoy < s_frameSkipAt + 1 && fSkippable) fStart = true;
    // (tests: SLY2N_SKIPHINT=1 shows the hint while a cutscene can be skipped, as after a press: its screenshots)
    static const bool s_fHintAlways = std::getenv("SLY2N_SKIPHINT") != nullptr;
    if (s_fHintAlways && fSkippable) { s_skip.fHint = true; s_skip.tHint = 0.0f; }
    if (std::getenv("SLY2N_SKIPPRINT") && fSkippable && s_skip.fHint)
    {
        static int s_c = 0;
        if (s_c++ % 30 == 0) std::printf("skipcut: hint (hold %.2f)\n", s_skip.tHold);
    }
    if (fStart || fCheckSkip) StartFast();
}

bool FSkipCutFastForward() { return s_skip.fFast; }

bool FSkipCutStep()
{
    if (!s_skip.fFast) return false;
    ++s_skip.cstep;
    if (IdCutscene() != s_skip.idScene || FScptWaitsNow() || s_skip.cstep >= CSTEP_MAX)
    {
        StopFast();
        return false;
    }
    return true;
}

uint16_t GrfSkipCutMask() { return s_skip.fMaskCircle ? uint16_t(BTN_Circle) : uint16_t(0); }

void DrawSkipCut()
{
    if (s_skip.fFast)
    {
        g_hud.Fade(glm::vec4(0, 0, 0, 128));   // (a fast-forward longer than game.cpp's 5 s frame: not shown)
        return;
    }
    DrawHint(s_skip);
}

void ResetMovieSkip()
{
    StopFast();
    s_skipMovie = SKIPCUT{};
    s_skipMovie.fAwaitRelease = true;
}

bool FMovieSkip(GLFWwindow* window, float dtReal)
{
    uint16_t grf = 0;
    float xl, yl, xr, yr;
    bool fPad = false;
    if (window) ReadPcInput(window, &grf, &xl, &yl, &xr, &yr, &fPad);
    static uint16_t s_grfPrev = 0;
    const uint16_t grfPressed = grf & ~s_grfPrev;
    s_grfPrev = grf;
    if (FUpdateHold(s_skipMovie, dtReal, grf, grfPressed, true))
    {
        s_skip.fMaskCircle = true;   // (the game after the movie does not see this Circle)
        return true;
    }
    return false;
}

void DrawMovieSkip() { DrawHint(s_skipMovie); }

void FMovieSkipTest() { s_skip.fMaskCircle = true; }   // (SLY2N_MOVIESKIPAT: what a hold's skip leaves behind)


// (QoL, po.h: the walkable safe house) its prompt at the bottom right, as the skip hint: Triangle to sit down at the
// table / to stand up from it
// (QoL, paint.h) the safe house's wardrobe while open: a panel at the left, its rows (the character's paint slots,
// then Done), each slot's colour with a swatch
void DrawWardrobe(const WARDROBEVIEW& v)
{
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const float depth = 16000000.0f, zPlate = depth / 16777216.0f;
    const glm::vec4 white(200.0f, 200.0f, 200.0f, 0.0f), dim(110.0f, 110.0f, 110.0f, 0.0f), gold(230.0f, 190.0f, 60.0f, 0.0f);
    auto text = [&](const std::string& t, float x, float y, float sz, const glm::vec4& off) {
        g_hud.DrawTextBox(*pfont, picons, t, x - 320.0f, (y - 224.0f) * 1.1f, sz, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                          glm::vec4(1.0f), glm::vec4(0.0f), depth + 100.0f, false);
    };
    auto hsv = [](float h, float sat, float val) {
        const float hh = (h - std::floor(h)) * 6.0f;
        const int i = int(hh) % 6;
        const float f = hh - std::floor(hh), p = val * (1 - sat), q = val * (1 - sat * f), t = val * (1 - sat * (1 - f));
        return i == 0 ? glm::vec3(val, t, p) : i == 1 ? glm::vec3(q, val, p) : i == 2 ? glm::vec3(p, val, t)
             : i == 3 ? glm::vec3(p, q, val) : i == 4 ? glm::vec3(t, p, val) : glm::vec3(val, p, q);
    };
    const float x0 = 28.0f, y0 = 70.0f, w = 330.0f, hRow = 30.0f;
    const int crow = 4 + v.ccos + 1;
    const float h = 62.0f + crow * hRow + 30.0f;
    g_hud.FillScreenRect(x0, y0, x0 + w, y0 + h, glm::vec4(0, 0, 0, 96), zPlate - 1e-4f);
    text(std::string("Wardrobe: ") + SzPaintChar(v.ichar), x0 + 12.0f, y0 + 10.0f, 26.0f, gold);
    const PAINTBAND& band = PaintBand(v.ichar, v.ipart);
    const float hue = v.ipreset < 0 ? band.hue : PaintPreset(v.ipreset).hue;
    auto pct = [](float g) { return std::to_string(int(std::lround(g * 100.0f))) + "%"; };
    std::vector<const char*> aszLabel = {"Part", "Color", "Brightness", "Saturation"};
    std::vector<std::string> aszVal = {band.szName ? band.szName : "", v.ipreset < 0 ? "Original" : PaintPreset(v.ipreset).szName,
                                       pct(v.val), pct(v.sat)};
    for (int i = 0; i < v.ccos; ++i) aszLabel.push_back(v.aszCosLabel[i]), aszVal.push_back(v.astrCosVal[i]);
    aszLabel.push_back("Done"), aszVal.push_back("");
    for (int r = 0; r < crow; ++r)
    {
        const float y = y0 + 52.0f + r * hRow;
        const bool fSel = r == v.irow;
        if (fSel) g_hud.FillScreenRect(x0 + 6.0f, y - 3.0f, x0 + w - 6.0f, y + hRow - 5.0f, glm::vec4(255, 255, 255, 28), zPlate - 5e-5f);
        text(aszLabel[r], x0 + 16.0f, y, 22.0f, fSel ? white : dim);
        if (aszVal[r].empty()) continue;
        text(std::string(fSel ? "< " : "") + aszVal[r] + (fSel ? " >" : ""), x0 + 160.0f, y, 22.0f, fSel ? white : dim);
    }
    // the part's colour as chosen (its hue at the chosen saturation and brightness)
    const glm::vec3 rgb = hsv(hue, std::clamp(0.75f * v.sat, 0.0f, 1.0f), std::clamp(0.75f * v.val, 0.0f, 1.0f));
    const float ySw = y0 + 52.0f + 1 * hRow;
    g_hud.FillScreenRect(x0 + w - 36.0f, ySw - 1.0f, x0 + w - 12.0f, ySw + 21.0f, glm::vec4(rgb * 255.0f, 128.0f), zPlate - 2e-5f);
    text(picons ? "&2O&. Done" : "Circle: Done", x0 + 16.0f, y0 + h - 30.0f, 18.0f, dim);
}

void DrawSafeHousePrompt()
{
    if (WARDROBEVIEW v; FWardrobeView(&v))
    {
        DrawWardrobe(v);
        return;
    }
    const int n = NSafeHousePrompt();
    if (!n) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const char* szAct = n == 1 ? "Sit down" : n == 3 ? "Wardrobe" : "Stand up";
    const std::string text = std::string(picons ? (n != 2 ? "&2O&. " : "&2T&. ") : (n != 2 ? "Circle: " : "Triangle: ")) + szAct;
    const float size = 24.0f, xRight = 616.0f, yTop = 412.0f, depth = 16000000.0f;
    const float w = DxText(*pfont, picons, text, size);
    const float hText = size * 0.909f;
    g_hud.FillScreenRect(xRight - w - 10.0f, yTop - 5.0f, xRight + 6.0f, yTop + hText + 7.0f, glm::vec4(0, 0, 0, 72), depth / 16777216.0f - 1e-4f);
    g_hud.DrawTextBox(*pfont, picons, text, xRight - w - 320.0f, (yTop - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0,
                      glm::vec4(200.0f, 200.0f, 200.0f, 0.0f), glm::vec4(1.0f), glm::vec4(0.0f), depth + 100.0f, false);
}
