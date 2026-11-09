// See pcinput.h. After ThiefRush-Dev's Sly1-Release Input.cpp (JOY::Update: bindings, Escape, Enter in menus,
// the camera keys, the device tracker) and mouseinput.cpp (capture, motion, bindings, menu buttons).

#include "pcinput.h"
#include "coop.h"
#include "actioninput.h"
#include "rumble.h"

#include "display.h"
#include "frontend.h"
#include "hackmouse.h"
#include "joy.h"
#include "menu.h"
#include "shop.h"
#include "debugmenu.h"

#include <GLFW/glfw3.h>

#include "../platform/button_prompts.h"
#include "../platform/platform.h"
#include "../platform/controller_family.h"
#include "../platform/system_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace st = cane::settings;

extern int g_frameJoy;   // (joy.cpp: the step's frame)

namespace
{
// (tests) SLY2N_TESTKEYS=binding@frame[+count],...: the binding (a GLFW key code, or a mouse button 2000 + n) held
// for count steps (default 4) from that step, as if pressed in the window (window runs only; prints the pad bits the
// game gets while one is held)
struct TESTKEY { int binding, frame0, frame1; };
const std::vector<TESTKEY>& TestKeys()
{
    static std::vector<TESTKEY> s_a;
    static bool s_fParsed = false;
    if (!s_fParsed)
    {
        s_fParsed = true;
        if (const char* psz = std::getenv("SLY2N_TESTKEYS"))
        {
            std::string s(psz);
            for (size_t i = 0; i < s.size();)
            {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                int binding = 0, frame = 0, count = 4;
                if (std::sscanf(s.substr(i, j - i).c_str(), "%d@%d+%d", &binding, &frame, &count) >= 2)
                    s_a.push_back({binding, frame, frame + count});
                i = j + 1;
            }
        }
    }
    return s_a;
}
bool FTestKeyHeld(int binding)
{
    for (const TESTKEY& k : TestKeys())
        if (k.binding == binding && g_frameJoy >= k.frame0 && g_frameJoy < k.frame1) return true;
    return false;
}
}  // namespace

PCINPUTOPT g_pcinput = PcInputDefaults();

namespace
{
int s_frameStickY = -100, s_frameStickY2 = -100;   // (NoteScriptStickRead: the frames a script last read stick Y / Y2)
}
void NoteScriptStickRead(int optid)
{
    if (optid == 901) s_frameStickY = g_frameJoy;
    if (optid == 905) s_frameStickY2 = g_frameJoy;
}
bool FTankSticks() { return g_frameJoy - s_frameStickY <= 3 && g_frameJoy - s_frameStickY2 <= 3; }

PCINPUTOPT PcInputDefaults()
{
    PCINPUTOPT o;
    // (Sly 2's own keyboard / mouse layout: st::kSly2DefaultKeyboardBindings)
    std::copy(std::begin(st::kSly2DefaultKeyboardBindings), std::end(st::kSly2DefaultKeyboardBindings), o.keyboard.begin());
    std::copy(std::begin(st::kDefaultGamepadBindings), std::end(st::kDefaultGamepadBindings), o.gamepad.begin());
    std::copy(std::begin(st::kDefaultCameraKeys), std::end(st::kDefaultCameraKeys), o.cameraKeys.begin());
    std::copy(std::begin(st::kDefaultTankKeys), std::end(st::kDefaultTankKeys), o.tankKeys.begin());
    return o;
}

void PcInputFromSettings(const st::SystemSettings& s)
{
    PCINPUTOPT& o = g_pcinput;
    std::copy(std::begin(s.keyboardBindings), std::end(s.keyboardBindings), o.keyboard.begin());
    std::copy(std::begin(s.gamepadBindings), std::end(s.gamepadBindings), o.gamepad.begin());
    std::copy(std::begin(s.cameraKeyBindings), std::end(s.cameraKeyBindings), o.cameraKeys.begin());
    std::copy(std::begin(s.tankKeyBindings), std::end(s.tankKeyBindings), o.tankKeys.begin());
    o.fDisableUnfocusedPad = s.disableControllerInputWhenUnfocused != 0;
    o.fMouseCamera = s.mouseCamera != 0;
    o.mouseSensitivity = s.mouseSensitivity;
    o.mouseAimSensitivity = s.mouseAimSensitivity;
    o.fMouseInvertX = s.mouseInvertX != 0;
    o.fMouseInvertY = s.mouseInvertY != 0;
    o.fMouseInvertBinocX = s.mouseInvertBinocX != 0;
    o.fMouseInvertBinocY = s.mouseInvertBinocY != 0;
    o.fMouseInvertTurretX = s.mouseInvertTurretX != 0;
    o.fMouseInvertTurretY = s.mouseInvertTurretY != 0;
    o.fPadInvertCameraX = s.padInvertCameraX != 0;
    o.fPadInvertCameraY = s.padInvertCameraY != 0;
    o.fAutoCameraRotation = s.autoCameraRotation != 0;
    o.fMouseRaw = s.mouseRawInput != 0;
    o.fMouseWheelZoom = s.mouseWheelZoom != 0;
    o.fMouseMenus = s.mouseMenus != 0;
    o.promptStyle = s.promptStyle;
    o.fAlwaysOriginalPs = s.alwaysOriginalPsPrompts != 0;
    o.fInstantSkipCutscenes = s.instantSkipCutscenes != 0;
    o.fHackMouseAim = s.hackMouseAim != 0;
    // (fDebugKeys: not from the settings' v14 debugKeys, which is ignored and kept as is since 2026-11-04; the
    //  launcher's Settings > Advanced > Debug mode turns them on, read in main.cpp from Debug Mode.ini)
    g_fControllerVibration = s.controllerVibration != 0;   // (rumble.h; v15)
}

void PcInputToSettings(st::SystemSettings& s)
{
    const PCINPUTOPT& o = g_pcinput;
    std::copy(o.keyboard.begin(), o.keyboard.end(), s.keyboardBindings);
    std::copy(o.gamepad.begin(), o.gamepad.end(), s.gamepadBindings);
    std::copy(o.cameraKeys.begin(), o.cameraKeys.end(), s.cameraKeyBindings);
    std::copy(o.tankKeys.begin(), o.tankKeys.end(), s.tankKeyBindings);
    s.disableControllerInputWhenUnfocused = o.fDisableUnfocusedPad;
    s.mouseCamera = o.fMouseCamera;
    s.mouseSensitivity = o.mouseSensitivity;
    s.mouseAimSensitivity = o.mouseAimSensitivity;
    s.mouseInvertX = o.fMouseInvertX;
    s.mouseInvertY = o.fMouseInvertY;
    s.mouseInvertBinocX = o.fMouseInvertBinocX;
    s.mouseInvertBinocY = o.fMouseInvertBinocY;
    s.mouseInvertTurretX = o.fMouseInvertTurretX;
    s.mouseInvertTurretY = o.fMouseInvertTurretY;
    s.padInvertCameraX = o.fPadInvertCameraX;
    s.padInvertCameraY = o.fPadInvertCameraY;
    s.autoCameraRotation = o.fAutoCameraRotation;
    s.mouseRawInput = o.fMouseRaw;
    s.mouseWheelZoom = o.fMouseWheelZoom;
    s.mouseMenus = o.fMouseMenus;
    s.promptStyle = o.promptStyle;
    s.alwaysOriginalPsPrompts = o.fAlwaysOriginalPs;
    s.instantSkipCutscenes = o.fInstantSkipCutscenes;
    s.hackMouseAim = o.fHackMouseAim;
    s.controllerVibration = g_fControllerVibration;
}

void ApplyPcDebugMode()
{
    g_pcinput.fDebugKeys = cane::debugMode();
    std::printf("debug mode: %s\n", g_pcinput.fDebugKeys ? "on" : "off");
}

void ApplyPcInputEnv()
{
    const char* psz = std::getenv("SLY2N_PCOPT");
    if (!psz) return;
    std::string s(psz);
    size_t i = 0;
    while (i < s.size())
    {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
            const std::string k = item.substr(0, eq);
            const int n = std::atoi(item.c_str() + eq + 1);
            if (k == "alwaysps") g_pcinput.fAlwaysOriginalPs = n != 0;
            else if (k == "instantskip") g_pcinput.fInstantSkipCutscenes = n != 0;
            else if (k == "promptstyle") g_pcinput.promptStyle = std::clamp(n, 0, cane::input::kPromptStyleLast);
            // (tests: a binding by action index, pcinput.h PCACTION: pad6=1 puts Cross on GLFW button B, key4=81)
            else if (k.rfind("pad", 0) == 0 && std::atoi(k.c_str() + 3) >= 0 && std::atoi(k.c_str() + 3) < PCA_Max)
                g_pcinput.gamepad[size_t(std::atoi(k.c_str() + 3))] = n;
            else if (k.rfind("key", 0) == 0 && std::atoi(k.c_str() + 3) >= 0 && std::atoi(k.c_str() + 3) < PCA_Max)
                g_pcinput.keyboard[size_t(std::atoi(k.c_str() + 3))] = n;
            else if (k == "mousecamera") g_pcinput.fMouseCamera = n != 0;
            else if (k == "hackmouse") g_pcinput.fHackMouseAim = n != 0;
            else if (k == "debugkeys") g_pcinput.fDebugKeys = n != 0;
            else if (k == "invcx") g_pcinput.fMouseInvertX = n != 0;
            else if (k == "invcy") g_pcinput.fMouseInvertY = n != 0;
            else if (k == "invbx") g_pcinput.fMouseInvertBinocX = n != 0;
            else if (k == "invby") g_pcinput.fMouseInvertBinocY = n != 0;
            else if (k == "invtx") g_pcinput.fMouseInvertTurretX = n != 0;
            else if (k == "invty") g_pcinput.fMouseInvertTurretY = n != 0;
            else if (k == "cinvx") g_pcinput.fPadInvertCameraX = n != 0;
            else if (k == "cinvy") g_pcinput.fPadInvertCameraY = n != 0;
        }
        i = j + 1;
    }
}

void StartupPcInput()
{
    cane::input::promptAtlas().upload();   // (no atlas: every family falls back to the game's own icons)
    cane::input::tracker().forceFromEnv();
}

bool FPcMenuMode()
{
    // (the menus' own pad: the pause / options / card pages, the title, the episode menu; a world's load or a
    // talk also takes the players' pad (lock bits 4 / 8): no camera there either)
    bool FEpisodeMenuActive();   // frontend.cpp
    return FMouseMenuUp() || (g_grfjoyt & 2) == 0;
}

// (PC menus) the right button in menus: Triangle, the menus' back; on the episode screen with Replay Jobs offered,
// where Triangle opens the list, that screen's back (Circle; Japan: Cross)
uint16_t BtnMouseBack()
{
    const uint32_t btn = BtnEpisodeScreenBack();   // (frontend.h: Sly 2's bit order, 0 elsewhere)
    return btn ? uint16_t(((btn & 0xFF) << 8) | (btn >> 8)) : uint16_t(BTN_Triangle);
}

bool FMouseMenuUp()
{
    bool FEpisodeMenuActive();   // frontend.cpp
    if (g_fMoviePlaying) return false;   // (a movie over the episode menu, the title: no cursor)
    return FMenuShown() || FTitleShown() || FEpisodeMenuActive() || FDebugMenuOpen() || FGadgetMenuShown() || FJobHelpShown();
}

// ------------------------------------------------------------ the mouse
namespace
{
constexpr int MOUSE_BUTTONS = st::kMouseButtonCount;
// Radians of turn per mouse count at 100% (ThiefRush's tuning).
constexpr float RAD_PER_COUNT_ORBIT = 0.0030f;
constexpr float RAD_PER_COUNT_AIM = 0.0012f;

bool s_fCaptured = false;
bool s_fLive = false;                           // (FPcInputLive)
bool s_fFreeHidden = false;                     // (hackmouse.h) the cursor free in the window, the OS one hidden
bool s_fCursor = false;                         // s_xCursor / s_yCursor: the free cursor in framebuffer pixels
double s_xCursor = 0.0, s_yCursor = 0.0;
bool s_fHaveLast = false;
double s_xLast = 0.0, s_yLast = 0.0;
float s_dxCounts = 0.0f, s_dyCounts = 0.0f;   // + = right / down
double s_dyScrollUp = 0.0, s_dyScrollDown = 0.0;
int s_wheelUp = 0, s_wheelDown = 0;              // notches in each direction this step
int s_wheel = 0;                                // signed net scroll for the basic controls
int s_cwheelMenu = 0;                           // menu steps still to give (one press every other step)
bool s_afButton[MOUSE_BUTTONS] = {};
bool s_afButtonPrev[MOUSE_BUTTONS] = {};
bool s_afButtonBlocked[MOUSE_BUTTONS] = {};
float s_travel = 0.0f;
GLFWscrollfun s_pfnScrollPrev = nullptr;
GLFWwindowfocusfun s_pfnFocusPrev = nullptr;

void FocusCallback(GLFWwindow* window, int focused)
{
    // Ignore mouse buttons held across a focus change until they are released.
    std::fill(std::begin(s_afButtonBlocked), std::end(s_afButtonBlocked), true);
    s_dyScrollUp = s_dyScrollDown = 0.0;
    s_cwheelMenu = 0;
    s_fHaveLast = false;
    if (s_pfnFocusPrev) s_pfnFocusPrev(window, focused);
}

void ScrollCallback(GLFWwindow* window, double dx, double dy)
{
    if (dy > 0.0) s_dyScrollUp += dy;
    else s_dyScrollDown -= dy;
    if (s_pfnScrollPrev) s_pfnScrollPrev(window, dx, dy);
}

bool FBasicWheelBound()
{
    return std::any_of(g_pcinput.keyboard.begin(), g_pcinput.keyboard.end(),
                       [](int key) { return key == st::kMouseWheelUp || key == st::kMouseWheelDown; });
}

bool FWantCapture(GLFWwindow* window)
{
    if (!g_pcinput.fMouseCamera) return false;
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE || glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE)
        return false;
    return !FPcMenuMode() && !FHackMouseMode();
}

// (hackmouse.h) the cursor moves freely in the window while the game draws its own
bool FWantFreeHidden(GLFWwindow* window)
{
    return glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE && glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_TRUE &&
           FHackMouseMode();
}

// (PC menus) the OS cursor while it is free: shown only in a menu the mouse can use (FMouseMenuUp), and there not
// after a controller's input until the mouse moves again; hidden over cutscenes, talks, loads and movies
bool s_fCursorShown = true;
bool s_fMenuCursor = false;                     // FMenuCursor: free, shown, in a menu, Mouse in Menus on
double s_xFree = 0.0, s_yFree = 0.0;            // the free cursor in framebuffer pixels (last step's)
bool s_fHaveFree = false;
bool s_fFreeMoved = false;
bool s_fMenuTarget = false, s_fMenuTargetUsed = false, s_fMenuClick = false;

void SetCapture(GLFWwindow* window, bool fCapture, bool fFreeHidden, bool fShown)
{
    if (fCapture)
    {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, g_pcinput.fMouseRaw ? GLFW_TRUE : GLFW_FALSE);
    }
    else
    {
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(window, GLFW_CURSOR, fFreeHidden || !fShown ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
    }
    if (fCapture != s_fCaptured || (!fCapture && s_fFreeHidden != fFreeHidden)) s_fHaveLast = false;
    s_fCaptured = fCapture;
    s_fFreeHidden = !fCapture && fFreeHidden;
    s_fCursorShown = fShown;
    if (std::getenv("SLY2N_CURSORPRINT"))
        std::printf("cursor: frame %d %s\n", g_frameJoy, fCapture ? "captured" : fFreeHidden ? "free hidden (hack)" : fShown ? "shown" : "hidden");
}

// (tests) SLY2N_TESTCURSOR=x:y@frame+count,...: the menu cursor at canvas (x, y) for those steps
struct TESTCURSOR { float x, y; int frame0, frame1; };
const std::vector<TESTCURSOR>& TestCursors()
{
    static std::vector<TESTCURSOR> s_a;
    static bool s_fParsed = false;
    if (!s_fParsed)
    {
        s_fParsed = true;
        if (const char* psz = std::getenv("SLY2N_TESTCURSOR"))
        {
            std::string s(psz);
            for (size_t i = 0; i < s.size();)
            {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                float x = 0, y = 0;
                int frame = 0, count = 1;
                if (std::sscanf(s.substr(i, j - i).c_str(), "%f:%f@%d+%d", &x, &y, &frame, &count) >= 3)
                    s_a.push_back({x, y, frame, frame + count});
                i = j + 1;
            }
        }
    }
    return s_a;
}
const TESTCURSOR* PtestCursor()
{
    for (const TESTCURSOR& c : TestCursors())
        if (g_frameJoy >= c.frame0 && g_frameJoy < c.frame1) return &c;
    return nullptr;
}

void UpdateMouse(GLFWwindow* window)
{
    static GLFWwindow* s_pwindowHooked = nullptr;
    if (s_pwindowHooked != window)
    {
        s_pfnScrollPrev = glfwSetScrollCallback(window, ScrollCallback);
        s_pfnFocusPrev = glfwSetWindowFocusCallback(window, FocusCallback);
        s_pwindowHooked = window;
    }
    s_dxCounts = s_dyCounts = 0.0f;
    const bool fFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    const auto notches = [](double dy) {
        const int rounded = int(std::lround(dy));
        return rounded == 0 && dy != 0.0 ? (dy > 0.0 ? 1 : -1) : rounded;
    };
    s_wheelUp = fFocused ? notches(s_dyScrollUp) : 0;
    s_wheelDown = fFocused ? notches(s_dyScrollDown) : 0;
    s_wheel = fFocused ? notches(s_dyScrollUp - s_dyScrollDown) : 0;
    s_dyScrollUp = s_dyScrollDown = 0.0;
    bool fClicked = false;
    for (int b = 0; b < MOUSE_BUTTONS; ++b)
    {
        s_afButtonPrev[b] = s_afButton[b];
        // (the test keys' buttons, SLY2N_TESTKEYS, need no focus: as the keyboard's)
        const bool fHeld = glfwGetMouseButton(window, b) == GLFW_PRESS;
        if (!fHeld) s_afButtonBlocked[b] = false;
        s_afButton[b] = (fFocused && fHeld && !s_afButtonBlocked[b]) || FTestKeyHeld(st::kMouseBindingBase + b);
        fClicked |= s_afButton[b] && !s_afButtonPrev[b];
    }
    if (fClicked || s_wheelUp || s_wheelDown) cane::input::tracker().noteMouse();
    // the free cursor's motion (a deliberate move shows it again after a controller's input)
    s_fFreeMoved = false;
    {
        double xw = 0.0, yw = 0.0;
        glfwGetCursorPos(window, &xw, &yw);
        int wx = 0, wy = 0, fbw = 0, fbh = 0;
        glfwGetWindowSize(window, &wx, &wy);
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (wx > 0 && wy > 0)
        {
            const double xf = xw * double(fbw) / double(wx), yf = yw * double(fbh) / double(wy);
            if (!s_fCaptured && s_fHaveFree && fFocused && (std::fabs(xf - s_xFree) > 0.5 || std::fabs(yf - s_yFree) > 0.5))
            {
                s_fFreeMoved = true;
                if (std::fabs(xf - s_xFree) + std::fabs(yf - s_yFree) > 3.0) cane::input::tracker().noteMouse();
            }
            s_xFree = xf;
            s_yFree = yf;
            s_fHaveFree = !s_fCaptured;
        }
    }
    const bool fWant = FWantCapture(window);
    const bool fWantFree = !fWant && FWantFreeHidden(window);
    // (shown: a mouse menu, the mouse or keyboard used last (a controller hides it until the mouse moves); never
    // over a cutscene, a talk, a load or a movie; unfocused: the OS's)
    const bool fShown = !fFocused || (FMouseMenuUp() && !cane::input::tracker().lastWasGamepad());
    if (fWant != s_fCaptured || fWantFree != s_fFreeHidden || fShown != s_fCursorShown) SetCapture(window, fWant, fWantFree, fShown);
    s_fMenuCursor = !s_fCaptured && !s_fFreeHidden && fShown && fFocused && g_pcinput.fMouseMenus && FMouseMenuUp();
    if (const TESTCURSOR* pc = PtestCursor())
    {
        static float s_xT = -1.0f, s_yT = -1.0f;
        s_fFreeMoved = pc->x != s_xT || pc->y != s_yT;
        s_xT = pc->x;
        s_yT = pc->y;
        s_fMenuCursor = g_pcinput.fMouseMenus && FMouseMenuUp();
    }
    s_fCursor = false;
    if (!s_fCaptured && !s_fFreeHidden) return;
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (s_fFreeHidden)
    {
        // (window coordinates to framebuffer pixels: they differ on scaled displays)
        int wx = 0, wy = 0, fbw = 0, fbh = 0;
        glfwGetWindowSize(window, &wx, &wy);
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (wx > 0 && wy > 0)
        {
            s_xCursor = x * double(fbw) / double(wx);
            s_yCursor = y * double(fbh) / double(wy);
            s_fCursor = true;
        }
    }
    if (s_fHaveLast)
    {
        const double dx = x - s_xLast, dy = y - s_yLast;
        // (the jump GLFW can report right after the capture changes is ignored)
        if (std::fabs(dx) < 4000.0 && std::fabs(dy) < 4000.0)
        {
            s_dxCounts = float(dx);
            s_dyCounts = float(dy);
        }
    }
    s_xLast = x;
    s_yLast = y;
    s_fHaveLast = true;
    // a deliberate swipe (not a brush of the trackpad) makes the prompts show keys
    s_travel = s_travel * 0.9f + std::fabs(s_dxCounts) + std::fabs(s_dyCounts);
    if (s_travel > 60.0f)
    {
        cane::input::tracker().noteMouse();
        s_travel = 0.0f;
    }
}

bool FMouseBindingHeld(int binding)
{
    if (binding == st::kMouseWheelUp) return sly2actions::profile.enabled ? s_wheelUp != 0 : s_wheel > 0;
    if (binding == st::kMouseWheelDown) return sly2actions::profile.enabled ? s_wheelDown != 0 : s_wheel < 0;
    const int b = binding - st::kMouseBindingBase;
    return b >= 0 && b < MOUSE_BUTTONS && s_afButton[b];
}

bool FKeyHeld(GLFWwindow* window, int key)
{
    return key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST && (glfwGetKey(window, key) == GLFW_PRESS || FTestKeyHeld(key));
}

bool FBindingHeld(GLFWwindow* window, int binding)
{
    return st::mouseBinding(binding) ? FMouseBindingHeld(binding) : FKeyHeld(window, binding);
}

// The keys that count as keyboard use for the prompts: every bound key, the camera keys and Escape.
uint64_t GrfKeyboardHeld(GLFWwindow* window)
{
    uint64_t grf = 0;
    for (int a = 0; a < PCA_Max; ++a)
        if (FKeyHeld(window, g_pcinput.keyboard[size_t(a)])) grf |= 1ull << a;
    for (int i = 0; i < 4; ++i)
        if (FKeyHeld(window, g_pcinput.cameraKeys[size_t(i)])) grf |= 1ull << (PCA_Max + i);
    for (int i = 0; i < 2; ++i)
        if (FKeyHeld(window, g_pcinput.tankKeys[size_t(i)])) grf |= 1ull << (PCA_Max + 6 + i);
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) grf |= 1ull << (PCA_Max + 4);
    if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS) grf |= 1ull << (PCA_Max + 5);
    return grf;
}

constexpr uint16_t s_abtnAction[PCA_Max] = {
    BTN_Up, BTN_Down, BTN_Left, BTN_Right, BTN_Square, BTN_Circle, BTN_Cross, BTN_Triangle,
    BTN_Start, BTN_Select, BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_L3, BTN_R3,
};

float Deadzone(float g) { return std::fabs(g) < 0.15f ? 0.0f : g; }
int s_jidPad = -1;
uint64_t s_grfKeyboardPrev = 0;
}  // namespace

void ReadPcInput(GLFWwindow* window, uint16_t* pgrf, float* pxl, float* pyl, float* pxr, float* pyr, bool* pfPad)
{
    uint16_t grf = 0, grfPad = 0;
    float xl = 0, yl = 0, xr = 0, yr = 0;
    *pfPad = false;
    s_fLive = true;
    s_fMenuTargetUsed = s_fMenuTarget;   // (the last step's menus: SetMenuMouseTarget)
    s_fMenuTarget = false;
    s_fMenuClick = false;
    UpdateMouse(window);
    const bool fFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    const bool fMenu = FPcMenuMode();
    const bool fGadgetMenuBindings = sly2actions::profile.enabled && FGadgetMenuShown();
    auto isMenuGadgetSlot = [&](int button) {
        return fGadgetMenuBindings && cane::sly2input::isGadgetSlot(cane::sly2input::legacyButtonAction(button));
    };
    auto gadgetWheelBound = [&](int binding) {
        if (!fGadgetMenuBindings || (binding != st::kMouseWheelUp && binding != st::kMouseWheelDown)) return false;
        for (auto slot : cane::sly2input::kGadgetSlots)
            if (sly2actions::profile.keyboard[size_t(slot)] == binding) return true;
        return false;
    };
    auto bindingFor = [&](int action, bool controller) {
        if (isMenuGadgetSlot(action))
            return sly2actions::Binding(cane::sly2input::legacyButtonAction(action), action, controller);
        if (!fMenu) return sly2actions::Binding(action, controller);
        if (controller) return g_pcinput.gamepad[size_t(action)];
        const int binding = g_pcinput.keyboard[size_t(action)];
        return gadgetWheelBound(binding) ? st::kKeyUnknown : binding;
    };
    GLFWgamepadstate gp{};

    // the gamepad: the most active mapped pad (Steam can park an idle virtual pad below the one in use)
    // (co-op: only the focused window reads the pad, so two games on one PC each take their own turn)
    if ((!g_pcinput.fDisableUnfocusedPad && !FCoopActive()) || fFocused)
    {
        s_jidPad = cane::input::selectActivePad(s_jidPad, &gp);
        cane::input::tracker().pollGamepads();
        if (s_jidPad >= 0)
        {
            *pfPad = true;
            for (int a = 0; a < PCA_Max; ++a)
            {
                const int b = bindingFor(a, true);
                bool fHeld = false;
                if (b >= 0 && b <= GLFW_GAMEPAD_BUTTON_LAST) fHeld = gp.buttons[b] == GLFW_PRESS;
                else if (b == st::kGamepadLeftTrigger) fHeld = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f;
                else if (b == st::kGamepadRightTrigger) fHeld = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f;
                if (fHeld) grf |= s_abtnAction[a];
            }
            grfPad = grf;
            xl = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
            yl = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
            xr = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
            yr = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
        }
    }
    else s_jidPad = -1;
    SetRumblePad(s_jidPad, true);   // (rumble.h: the pad the motors are driven on; -1 none)
    {
        std::array<bool, cane::sly2input::kActionCount> keys{}, pad{};
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = fFocused && FBindingHeld(window, sly2actions::profile.keyboard[i]);
            const int b = sly2actions::profile.gamepad[i];
            pad[i] = *pfPad && ((b >= 0 && b <= GLFW_GAMEPAD_BUTTON_LAST && gp.buttons[b] == GLFW_PRESS) ||
                (b == st::kGamepadLeftTrigger && gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f) ||
                (b == st::kGamepadRightTrigger && gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f));
        }
        sly2actions::Sample(keys, pad, !fMenu && !FDebugMenuOpen(), fGadgetMenuBindings);
    }

    // the keyboard (and mouse bindings): the movement actions move the left stick, the others are buttons
    // (in a menu with "Mouse in Menus" on, the left / right buttons only confirm / go back: an action bound to
    // them, Square / R3 by default, is not sent there)
    const bool fMenuMouse = fMenu && g_pcinput.fMouseMenus;
    bool afKey[PCA_Max] = {};
    for (int a = 0; a < PCA_Max; ++a)
    {
        const int binding = bindingFor(a, false);
        if (fMenuMouse && !isMenuGadgetSlot(a) && (binding == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_LEFT ||
                           binding == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_RIGHT))
            continue;
        afKey[a] = FBindingHeld(window, binding);
    }
    // (hackmouse.h) while the hack's cursor is up, left click fires the ship's guns: its own action is not sent
    if (FHackMouseMode())
        for (int a = 0; a < PCA_Max; ++a)
            if (g_pcinput.keyboard[size_t(a)] == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_LEFT) afKey[a] = false;
    if (afKey[PCA_Left] && !afKey[PCA_Right]) xl = -1.0f;
    if (afKey[PCA_Right] && !afKey[PCA_Left]) xl = 1.0f;
    const bool fTankTracks = sly2actions::TankTrackAxes(yl, yr);
    if (!fTankTracks)
    {
        if (afKey[PCA_Up] && !afKey[PCA_Down]) yl = 1.0f;
        if (afKey[PCA_Down] && !afKey[PCA_Up]) yl = -1.0f;
    }
    // (PC) the tank (FTankSticks: a script drives one track by each stick's Y): with the keyboard, the left / right track
    // keys (A / D) drive their track forward, both together; forward alone drives both, back reverses the held tracks
    // (or both). The controller keeps its sticks.
    if (!sly2actions::profile.enabled && FTankSticks() && !fMenu && !cane::input::tracker().lastWasGamepad())
    {
        const bool fL = FKeyHeld(window, g_pcinput.tankKeys[0]), fR = FKeyHeld(window, g_pcinput.tankKeys[1]);
        const bool fFwd = afKey[PCA_Up], fBack = afKey[PCA_Down];
        float gL = 0.0f, gR = 0.0f;
        if (fBack) { gL = (fL || !fR) ? -1.0f : 0.0f; gR = (fR || !fL) ? -1.0f : 0.0f; }
        else { gL = (fL || fFwd) ? 1.0f : 0.0f; gR = (fR || fFwd) ? 1.0f : 0.0f; }
        xl = xr = 0.0f;
        yl = gL;
        yr = gR;
    }
    for (int a = PCA_Square; a < PCA_Max; ++a)
        if (afKey[a]) grf |= s_abtnAction[a];
    // Escape: a fixed second Start (it pauses, and leaves the menus that Start closes)
    const bool fEscape = (fFocused && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) || FTestKeyHeld(GLFW_KEY_ESCAPE);
    if (fEscape) grf |= BTN_Start;
    // a newly pressed key makes the keyboard the last used device
    const uint64_t grfKeys = GrfKeyboardHeld(window);
    if (grfKeys & ~s_grfKeyboardPrev) cane::input::tracker().noteKeyboard();
    s_grfKeyboardPrev = grfKeys;

    // Enter in a menu confirms (Cross), as PC menus do: an Enter still held from opening the menu (Enter is Start
    // by default) waits for its release, and one that confirmed stays a confirm until released (closing a menu
    // with it must not open the pause menu again)
    // (the card dialog (0x1ACCC8: the overwrite / erase questions, the notices) takes Square (0x80) for Yes / OK, not
    // Cross: the PC confirms there send that (N14: a click or Enter did not answer it))
    const uint16_t btnConfirm = fMenu && FCardMessageActive() ? BTN_Square : BTN_Cross;
    // (R40: the bare title, "Press Start", reads only Start (0x1DD920 takes 0x800): Enter stays Start there; with a
    // menu over the title (the data access menu, the language menu, the card's messages) it confirms again)
    const bool fEnterMenu = fMenu && !(FTitleShown() && !FMenuShown() && !FDebugMenuOpen());
    // (K/M could not skip the intro) with no menu up, only the pad locked (the opening's intro
    // in the title world, cutscenes, talks, loads), an Enter pressed there stays the Start it is bound to as well:
    // the intro reads Start (a controller's skips it), and Cross still answers a scene waiting for a button
    static bool s_fEnterLatched = false, s_fEnterConfirms = false, s_fEnterKeepsStart = false;
    const bool fEnter = FKeyHeld(window, GLFW_KEY_ENTER) || FKeyHeld(window, GLFW_KEY_KP_ENTER);
    if (!fEnter) s_fEnterLatched = s_fEnterConfirms = s_fEnterKeepsStart = false;
    else if (fEnterMenu && !s_fEnterLatched && !s_fEnterConfirms)
    {
        s_fEnterConfirms = true;
        s_fEnterKeepsStart = !FMouseMenuUp();
    }
    else if (!fEnterMenu && !s_fEnterConfirms) s_fEnterLatched = true;
    if (s_fEnterConfirms)
    {
        if (fMenu) grf |= btnConfirm;
        const int kStart = g_pcinput.keyboard[PCA_Start];
        if ((kStart == GLFW_KEY_ENTER || kStart == GLFW_KEY_KP_ENTER) && !fEscape && !(grfPad & BTN_Start) && !s_fEnterKeepsStart)
            grf &= uint16_t(~BTN_Start);
    }

    // the camera keys: the right stick, or in a menu the d-pad
    bool fCamL = FKeyHeld(window, g_pcinput.cameraKeys[0]), fCamR = FKeyHeld(window, g_pcinput.cameraKeys[1]);
    bool fCamU = FKeyHeld(window, g_pcinput.cameraKeys[2]), fCamD = FKeyHeld(window, g_pcinput.cameraKeys[3]);
    if (fMenu)
    {
        if (fCamL) grf |= BTN_Left;
        if (fCamR) grf |= BTN_Right;
        if (fCamU) grf |= BTN_Up;
        if (fCamD) grf |= BTN_Down;
        fCamL = fCamR = fCamU = fCamD = false;
    }
    if (fCamL && !fCamR) xr = -1.0f;
    if (fCamR && !fCamL) xr = 1.0f;
    if (!fTankTracks)
    {
        if (fCamU && !fCamD) yr = 1.0f;
        if (fCamD && !fCamU) yr = -1.0f;
    }

    // the mouse in menus: the wheel steps (one press every other step, so each notch is an edge), left confirms,
    // right goes back
    static bool s_fWheelPulse = false;
    if (fMenuMouse)
    {
        int menuWheel = 0;
        if (sly2actions::profile.enabled) {
            const auto unbound = [&](int binding) {
                for (int a = 0; a < PCA_Max; ++a)
                    if (bindingFor(a, false) == binding) return false;
                return true;
            };
            menuWheel = (unbound(st::kMouseWheelUp) ? s_wheelUp : 0) -
                        (unbound(st::kMouseWheelDown) ? s_wheelDown : 0);
        }
        else if (!FBasicWheelBound()) menuWheel = s_wheel;
        if (menuWheel) s_cwheelMenu = std::clamp(s_cwheelMenu - menuWheel, -4, 4);   // (up: negative steps)
        if (s_fWheelPulse) s_fWheelPulse = false;
        else if (s_cwheelMenu)
        {
            grf |= s_cwheelMenu < 0 ? BTN_Up : BTN_Down;
            s_cwheelMenu += s_cwheelMenu < 0 ? 1 : -1;
            s_fWheelPulse = true;
        }
        // (PC menus) a press over a menu's own target (SetMenuMouseTarget, the last step's) goes to that menu
        // (FMenuClick), not to the confirm button, until released
        static bool s_fClickTaken = false;
        const bool fLeft = s_afButton[GLFW_MOUSE_BUTTON_LEFT];
        if (!fLeft) s_fClickTaken = false;
        else if (!s_afButtonPrev[GLFW_MOUSE_BUTTON_LEFT] && s_fMenuTargetUsed && s_fMenuCursor) s_fClickTaken = s_fMenuClick = true;
        if (fLeft && !s_fClickTaken) grf |= btnConfirm;
        if (s_afButton[GLFW_MOUSE_BUTTON_RIGHT]) grf |= BtnMouseBack();
    }
    else s_cwheelMenu = 0, s_fWheelPulse = false;

    if (!TestKeys().empty())
    {
        for (const TESTKEY& k : TestKeys())
            if (g_frameJoy >= k.frame0 && g_frameJoy < k.frame1)
            {
                std::printf("testkeys: frame %d binding %d menu %d grf 0x%04x\n", g_frameJoy, k.binding, int(fMenu), unsigned(grf));
                break;
            }
    }
    *pgrf = grf;
    *pxl = xl;
    *pyl = yl;
    *pxr = xr;
    *pyr = yr;
}

bool FMouseCaptured() { return s_fCaptured; }
bool FPcInputLive() { return s_fLive; }

bool FMenuCursor(float* pxCanvas, float* pyCanvas)
{
    if (!s_fMenuCursor) return false;
    if (const TESTCURSOR* pc = PtestCursor())
    {
        *pxCanvas = pc->x;
        *pyCanvas = pc->y;
        return true;
    }
    const DISPLAY& d = g_display;
    const float xn = float((s_xFree - double(d.presentX)) / double(std::max(1, d.presentWidth))) * 2.0f - 1.0f;
    const float yn = 1.0f - float((s_yFree - double(d.presentY)) / double(std::max(1, d.presentHeight))) * 2.0f;
    *pxCanvas = ((xn - d.uiOffset.x) / d.uiScale.x + 1.0f) * 320.0f;
    *pyCanvas = (1.0f - (yn - d.uiOffset.y) / d.uiScale.y) * 224.0f;
    return true;
}
bool FMenuCursorMoved() { return s_fMenuCursor && s_fFreeMoved; }
void SetMenuMouseTarget() { s_fMenuTarget = true; }
bool FMenuClick() { return s_fMenuClick; }

bool FCursorPicture(float* pxNdc, float* pyNdc, bool* pfLeft)
{
    if (!s_fCursor) return false;
    const DISPLAY& d = g_display;
    // (the picture's place in the framebuffer is centred, so its offset is the same from the top as from the bottom)
    const float x = float((s_xCursor - double(d.presentX)) / double(std::max(1, d.presentWidth))) * 2.0f - 1.0f;
    const float y = 1.0f - float((s_yCursor - double(d.presentY)) / double(std::max(1, d.presentHeight))) * 2.0f;
    // (on the bars around the picture: held at its edge)
    *pxNdc = std::clamp(x, -1.0f, 1.0f);
    *pyNdc = std::clamp(y, -1.0f, 1.0f);
    *pfLeft = s_afButton[GLFW_MOUSE_BUTTON_LEFT];
    return true;
}

// (tests) SLY2N_TESTMOUSE=dx:dy:frame0:frame1: these mouse counts every step of frames [frame0, frame1), as if captured
bool FTestMouse(float* pdx, float* pdy)
{
    static const char* s_psz = std::getenv("SLY2N_TESTMOUSE");
    if (!s_psz) return false;
    extern int g_frameJoy;
    float dx = 0, dy = 0;
    int f0 = 0, f1 = 1 << 30;
    std::sscanf(s_psz, "%f:%f:%d:%d", &dx, &dy, &f0, &f1);
    if (g_frameJoy < f0 || g_frameJoy >= f1) return false;
    *pdx = dx;
    *pdy = dy;
    return true;
}

// (the test mouse's counts go through the same inversion as the real mouse's, at 100% sensitivity)
void MouseOrbitDelta(float* pdradPan, float* pdradTilt)
{
    *pdradPan = *pdradTilt = 0.0f;
    float dx = 0.0f, dy = 0.0f, k = RAD_PER_COUNT_ORBIT;
    if (!FTestMouse(&dx, &dy))
    {
        if (!s_fCaptured) return;
        dx = s_dxCounts;
        dy = s_dyCounts;
        k *= float(g_pcinput.mouseSensitivity) / 100.0f;
    }
    *pdradPan = dx * k * (g_pcinput.fMouseInvertX ? -1.0f : 1.0f);
    *pdradTilt = -dy * k * (g_pcinput.fMouseInvertY ? -1.0f : 1.0f);
}

void MouseAimDelta(MOUSEAIM aim, float* pdxRight, float* pdyUp)
{
    *pdxRight = *pdyUp = 0.0f;
    float dx = 0.0f, dy = 0.0f, k = RAD_PER_COUNT_AIM;
    if (!FTestMouse(&dx, &dy))
    {
        if (!s_fCaptured) return;
        dx = s_dxCounts;
        dy = s_dyCounts;
        k *= float(g_pcinput.mouseAimSensitivity) / 100.0f;
    }
    const bool fInvertX = aim == MOUSEAIM_Turret ? g_pcinput.fMouseInvertTurretX : g_pcinput.fMouseInvertBinocX;
    const bool fInvertY = aim == MOUSEAIM_Turret ? g_pcinput.fMouseInvertTurretY : g_pcinput.fMouseInvertBinocY;
    *pdxRight = dx * k * (fInvertX ? -1.0f : 1.0f);
    *pdyUp = -dy * k * (fInvertY ? -1.0f : 1.0f);
}

float MouseWheelZoom()
{
    if (!sly2actions::profile.enabled)
        return s_fCaptured && g_pcinput.fMouseWheelZoom && !FBasicWheelBound() ? float(s_wheel) : 0.0f;
    if (FPcMenuMode()) return 0.0f;

    const bool automaticZoom = s_fCaptured && g_pcinput.fMouseWheelZoom;
    return float(sly2actions::WheelZoom(st::kMouseWheelUp, s_wheelUp, automaticZoom) +
                 sly2actions::WheelZoom(st::kMouseWheelDown, s_wheelDown, automaticZoom));
}

const char* PchzMouseBindingName(int binding)
{
    switch (binding - st::kMouseBindingBase)
    {
    case 0: return "Left Mouse";
    case 1: return "Right Mouse";
    case 2: return "Middle Mouse";
    case 3: return "Mouse 4";
    case 4: return "Mouse 5";
    case 5: return "Mouse 6";
    case 6: return "Mouse 7";
    case 7: return "Mouse 8";
    case 8: return "Wheel Up";
    case 9: return "Wheel Down";
    default: return "Mouse";
    }
}

const char* PchzKeyName(int key)
{
    if (st::mouseBinding(key)) return PchzMouseBindingName(key);
    switch (key)
    {
    case GLFW_KEY_UNKNOWN: return "Unbound";
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_KP_ENTER: return "Keypad Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_ESCAPE: return "Escape";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_UP: return "Up Arrow";
    case GLFW_KEY_DOWN: return "Down Arrow";
    case GLFW_KEY_LEFT: return "Left Arrow";
    case GLFW_KEY_RIGHT: return "Right Arrow";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Delete";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_PAGE_UP: return "Page Up";
    case GLFW_KEY_PAGE_DOWN: return "Page Down";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25)
    {
        static char s_ach[8];
        std::snprintf(s_ach, sizeof s_ach, "F%d", key - GLFW_KEY_F1 + 1);
        return s_ach;
    }
    if (const char* psz = glfwGetKeyName(key, 0))
    {
        static char s_ach[16];
        std::snprintf(s_ach, sizeof s_ach, "%s", psz);
        if (s_ach[0] >= 'a' && s_ach[0] <= 'z' && s_ach[1] == '\0') s_ach[0] = char(s_ach[0] - 'a' + 'A');
        return s_ach;
    }
    return "Key";
}

int KeyBindingPressed(GLFWwindow* window)
{
    static bool s_afPrev[GLFW_KEY_LAST + 1] = {};
    static bool s_afMousePrev[MOUSE_BUTTONS] = {};
    int found = -1;
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
    {
        const bool f = glfwGetKey(window, k) == GLFW_PRESS;
        if (f && !s_afPrev[k] && found < 0) found = k;
        s_afPrev[k] = f;
    }
    for (int b = 0; b < MOUSE_BUTTONS; ++b)
    {
        const bool f = FMouseBindingHeld(st::kMouseBindingBase + b);
        if (f && !s_afMousePrev[b] && found < 0) found = st::kMouseBindingBase + b;
        s_afMousePrev[b] = f;
    }
    if (found < 0 && FMouseBindingHeld(st::kMouseWheelUp)) found = st::kMouseWheelUp;
    if (found < 0 && FMouseBindingHeld(st::kMouseWheelDown)) found = st::kMouseWheelDown;
    return found;
}

// (pcinput.h) the debug function keys: nothing unless Debug mode is on (fDebugKeys)
bool FDebugKeyHeld(GLFWwindow* window, int key)
{
    if (!g_pcinput.fDebugKeys) return false;
    return FTestKeyHeld(key) || (window && glfwGetKey(window, key) == GLFW_PRESS);
}
