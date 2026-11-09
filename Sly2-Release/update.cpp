#include "coop.h"
#include <cstring>
#include "worldglobals.h"
#include "update.h"
#include "debugmenu.h"
#include "rope.h"
#include "dance.h"
#include "turret.h"
#include "vehiclehooks.h"
#include "cmworld.h"
#include "interact.h"
#include "jt.h"
#include "aichase.h"
#include "detect.h"
#include "murraycarry.h"
#include "overlay.h"
#include "booty.h"
#include "pickup.h"
#include "marker.h"
#include "hudwidget.h"
#include "laser.h"
#include "trail.h"
#include "so.h"
#include "actor.h"
#include "sly.h"
#include "asega.h"
#include "vtnative.h"

#include "smrun.h"
#include "sidebag.h"
#include "proc.h"
#include "pair.h"
#include <bit>
#include <string>

#include "activate.h"
#include "anim.h"
#include "guard.h"
#include "nav.h"
#include "task.h"
#include <cmath>
#include "transition.h"
#include "savedata.h"
#include "po.h"

#include "brx.h"
#include "cls.h"
#include "clock.h"
#include "eval.h"
#include "frame.h"
#include "sw.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
std::unordered_map<uint32_t, PFNUPDATE>& MpUpdate()
{
    static std::unordered_map<uint32_t, PFNUPDATE> s_map;
    return s_map;
}
// A registration's source file (its base name) orders it: an entry goes after every entry of a file whose name sorts
// before or equal to its own, so the lists are in the files' link order on Linux (CMake's sorted glob), then each file's
// own order, independent of the order the files' static constructors run in (reversed on Windows by MinGW)
const char* PszBaseName(const char* psz)
{
    const char* pszBase = psz;
    for (const char* pch = psz; *pch; ++pch)
        if (*pch == '/' || *pch == '\\') pszBase = pch + 1;
    return pszBase;
}
template <class T>
void InsertInFileOrder(std::vector<std::pair<const char*, T>>& a, const char* pszFile, T&& t)
{
    const char* pszBase = PszBaseName(pszFile);
    auto it = a.end();
    while (it != a.begin() && std::strcmp((it - 1)->first, pszBase) > 0) --it;
    a.insert(it, {pszBase, std::move(t)});
}

std::unordered_map<uint32_t, std::vector<std::pair<const char*, PFNSWPASS>>>& MpPass()
{
    static std::unordered_map<uint32_t, std::vector<std::pair<const char*, PFNSWPASS>>> s_map;
    return s_map;
}

void Pass(uint32_t pfnOrig, SW* psw)
{
    auto it = MpPass().find(pfnOrig);
    if (it == MpPass().end()) return;
    for (auto& [pszFile, pfn] : it->second) pfn(psw);
}

// orig SW +0x39F0: the timed events, by time: an object's deferred event (plo, symid), or a scheduled callback
// (schedule-callback 0x123BD8: refProc applied to the arguments in the global frame; id from SW +0x3D6C)
struct DEFEV
{
    LO* plo;
    uint32_t symid;
    float t;
    std::vector<CRef> aref;
    CRef refProc;
    int id = -1;
};
std::vector<DEFEV> s_adefev;
WORLD_TABLE(s_adefev);   // (PC: moved aside while a kept world loads, worldglobals.h)
struct DEFCALL
{
    void* object;
    uint32_t pfnOrig;
    PFNDEFERREDCALL pfn;
    int arg;
    PFNDEFERREDPOINTERCALL pfnPointer = nullptr;
    void* pointerArg = nullptr;
};
std::vector<DEFCALL> s_adefcall;
WORLD_TABLE(s_adefcall);   // (PC: moved aside while a kept world loads, worldglobals.h)
int s_idDefevLast = 0;
void InsertDefev(DEFEV&& ev)
{
    auto it = std::find_if(s_adefev.begin(), s_adefev.end(), [&](const DEFEV& e) { return e.t > ev.t; });
    s_adefev.insert(it, std::move(ev));
}

// orig 0x001A5E28 (LO vt slot 16): the splice "update" event (symid 1), at most once per +0x30 time
void UpdateLo(LO* plo)
{
    if (plo->tNextUpdate <= g_clock.t) HandleLoSpliceEvent(plo, 1, true, 0, nullptr);
}

// orig 0x001F36E0 (ALO vt slot 16): the LO update, then the fade of +0x12C by +0x130 per second
void UpdateAlo(LO* plo)
{
    UpdateLo(plo);
    ALO* palo = static_cast<ALO*>(plo);
    UpdateAloRegion(palo);  // the region entry (0x1F37C8 with 0x1C0D70 at its position)
    if (palo->dgFade != 0.0f)
    {
        palo->gFade += g_clock.dt * palo->dgFade;
        if (palo->gFade <= 0.0f)
        {
            palo->gFade = 0.0f;
            palo->dgFade = 0.0f;
            RemoveLo(g_psw, palo);  // vt slot 9
        }
        else if (palo->gFade >= 1.0f)
        {
            palo->gFade = 1.0f;
            palo->dgFade = 0.0f;
        }
    }
}

std::vector<std::pair<const char*, std::function<bool(LO*, int, void*)>>>& LoMessageHandlers()
{
    static std::vector<std::pair<const char*, std::function<bool(LO*, int, void*)>>> s_a;
    return s_a;
}

// The objects' messages (vt slot 5: ASEGA ends 2 / wraps 3 / events 9, SMA 10): the class's native handler
namespace
{
struct INITLOMESSAGE
{
    INITLOMESSAGE()
    {
        g_pfnLoMessage = [](LO* plo, int msgk, void* pv) {
            // the class's own native slot 5 first (e.g. Murray's 0x1B68B8), else the registered handlers
            using PFN5 = void (*)(LO*, int, void*);
            if (PFN5 pfn = reinterpret_cast<PFN5>(PvNativeVt(plo, 5))) { pfn(plo, msgk, pv); return; }
            for (const auto& [pszFile, pfnHandler] : LoMessageHandlers())
                if (pfnHandler(plo, msgk, pv)) return;
        };
    }
} s_initlomessage;
}

struct INITUPD
{
    INITUPD()
    {
        RegisterLoUpdate(0x1A5E28, UpdateLo);
        RegisterLoUpdate(0x1F36E0, UpdateAlo);
        // Classes whose own update (vt slot 16) begins with the LO's splice "update" event 0x1A5E28 and whose
        // remaining work is done natively elsewhere (the screen runtime: screenrt.cpp's UpdateScreenObjs; the exits'
        // and the camera zones' passes): their table entries still get that splice event, as the original's do
        // (e.g. the Ep7 power chop meter, sprite set 3206: its s1 handler advances the meter's time). A full native
        // port registered for the same address elsewhere replaces this (emplace: never overwrites).
        for (uint32_t pfnOrig : {0x212C50u, 0x211F90u, 0x221388u, 0x15DCD8u, 0x228538u})
            MpUpdate().emplace(pfnOrig, UpdateLo);  // 0x212C50 / 0x221388 / 0x15DCD8 / 0x228538: 0x1A5E28 first
    }
} s_initupd;
}

void RegisterLoMessageHandler(std::function<bool(LO*, int, void*)> pfn, const char* pszFile)
{
    InsertInFileOrder(LoMessageHandlers(), pszFile, std::move(pfn));
}

// The objects' listeners (orig LO +0x8: a counted list of {id, count}; 0x1333C0 adds, 0x133448 drops, 0x133508 sends a
// message to the object (its slot 5) and then to each listener's slot 5)
namespace
{
struct LISTENER
{
    const void* id;
    int c;
    std::function<void(int, void*)> pfn;
};
std::unordered_map<const LO*, std::vector<LISTENER>>& MpListeners()
{
    static std::unordered_map<const LO*, std::vector<LISTENER>> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}
}
void AddLoListener(LO* plo, const void* id, std::function<void(int msgk, void* pv)> pfn)
{
    auto& a = MpListeners()[plo];
    for (LISTENER& l : a)
        if (l.id == id) { ++l.c; return; }
    a.insert(a.begin(), LISTENER{id, 1, std::move(pfn)});
}
void RemoveLoListener(LO* plo, const void* id)
{
    auto it = MpListeners().find(plo);
    if (it == MpListeners().end()) return;
    auto& a = it->second;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id == id)
        {
            if (--a[i].c <= 0) a.erase(a.begin() + static_cast<long>(i));
            return;
        }
}
void NotifyLo(LO* plo, int msgk, void* pv)
{
    if (!plo) return;
    if (g_pfnLoMessage) g_pfnLoMessage(plo, msgk, pv);
    auto it = MpListeners().find(plo);
    if (it == MpListeners().end()) return;
    const std::vector<LISTENER> a = it->second;  // (a listener may drop itself)
    for (const LISTENER& l : a)
        if (l.pfn) l.pfn(msgk, pv);
}
bool FLoHasListeners(const LO* plo)
{
    auto it = MpListeners().find(plo);
    return it != MpListeners().end() && !it->second.empty();
}
void NotifyLoListeners(LO* plo, int msgk, void* pv)
{
    auto it = MpListeners().find(plo);
    if (it == MpListeners().end()) return;
    const std::vector<LISTENER> a = it->second;  // (a listener may drop itself)
    for (const LISTENER& l : a)
        if (l.pfn) l.pfn(msgk, pv);
}
void ResetLoListeners() { MpListeners().clear(); }
void ForgetLoListeners(const LO* plo) { MpListeners().erase(plo); }
namespace
{
struct INITLONOTIFY
{
    INITLONOTIFY() { g_pfnLoNotify = [](LO* plo, int msgk, void* pv) { NotifyLo(plo, msgk, pv); }; }
} s_initlonotify;
}

void RegisterLoUpdate(uint32_t pfnOrig, PFNUPDATE pfn) { MpUpdate()[pfnOrig] = pfn; }

PFNUPDATE PfnUpdateForClass(const CLS* pcls)
{
    if (!pcls) return nullptr;
    auto it = MpUpdate().find(pcls->PfnOrig(16));
    return it == MpUpdate().end() ? nullptr : it->second;
}

bool FHasSwPass(uint32_t pfnOrig) { return MpPass().count(pfnOrig) != 0; }

PFNUPDATE PfnUpdateOrig(uint32_t pfnOrig)
{
    auto it = MpUpdate().find(pfnOrig);
    return it == MpUpdate().end() ? nullptr : it->second;
}

void RegisterSwPass(uint32_t pfnOrig, PFNSWPASS pfn, const char* pszFile) { InsertInFileOrder(MpPass()[pfnOrig], pszFile, std::move(pfn)); }

// orig 0x00210140
void AddLoToUpdateTable(SW* psw, LO* plo)
{
    if (plo->iupd >= 0 || (plo->grfUpd & GRFUPD_Never)) return;
    if (psw->aupd.size() >= 0x1000 || (psw->aupd.size() && float(psw->cupdDead) / float(psw->aupd.size()) > 0.1f))
    {
        // compact (the entry being updated keeps its place: SW +0x3F1C)
        std::size_t w = 0;
        for (std::size_t r = 0; r < psw->aupd.size(); ++r)
        {
            if (static_cast<int>(r) == psw->iupdCur) psw->iupdCur = static_cast<int>(w);
            if (psw->aupd[r].plo)
            {
                psw->aupd[r].plo->iupd = static_cast<int>(w);
                psw->aupd[w++] = psw->aupd[r];
            }
        }
        psw->aupd.resize(w);
        psw->cupdDead = 0;
    }
    plo->iupd = static_cast<int>(psw->aupd.size());
    psw->aupd.push_back(UPDENT{plo->grfUpd, plo});
}

// orig 0x00210268
void RemoveLoFromUpdateTable(SW* psw, LO* plo)
{
    if (plo->iupd < 0) return;
    UPDENT& e = psw->aupd[plo->iupd];
    plo->grfUpd = e.grf;
    e = UPDENT{0, nullptr};
    plo->iupd = -1;
    ++psw->cupdDead;
}

void PrintRef(const CRef& ref, std::string* ps);
void HandleLoSpliceEvent(LO* plo, uint32_t symid, bool fDeferred, int carg, CRef* aref, CRef* prefResult)
{
    // orig 0x1331F8: only when the object's module frame binds the event (0x127DB8, recursive)
    if (!plo->pframe) return;
    CRef* pref = plo->pframe->PrefFindBinding(symid, 1);
    if (!pref) return;
    static const bool s_fPrint = std::getenv("SLY2N_EVENTPRINT") != nullptr;  // (debug: the bound events fired)
    if (s_fPrint && symid != 1)
    {
        std::string str;
        if (std::getenv("SLY2N_EVENTPRINT")[0] == '2')
        {
            if (pref->m_tagk == TAGK_Proc && pref->m_pproc)
            {
                CRef body;
                body.SetPair(pref->m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
            }
            else PrintRef(*pref, &str);
        }
        std::string strArgs;
        for (int i = 0; i < carg && aref; ++i)
        {
            strArgs += i ? " " : " args ";
            if (LO* p = dynamic_cast<LO*>(aref[i].m_tagk == TAGK_Basic ? aref[i].m_pbasic : nullptr)) strArgs += "obj" + std::to_string(p->oid);
            else PrintRef(aref[i], &strArgs);
        }
        std::printf("event %u of %d (cid 0x%X)%s t %.3f%s %s\n", symid, plo->oid, plo->Cid(), fDeferred ? " deferred" : "", g_clock.t, strArgs.c_str(), str.c_str());
    }
    if (fDeferred)
    {
        DEFEV ev{plo, symid, g_clock.t, {}, CRef(), -1};
        for (int i = 0; i < carg; ++i) ev.aref.push_back(aref[i]);
        InsertDefev(std::move(ev));  // orig 0x133040
        return;
    }
    // orig 0x132ED8 / 0x133000: the form (symid object args...) evaluated in the object's frame: the handler gets the
    // object as its first argument
    std::vector<CRef> aargs;
    aargs.reserve(static_cast<size_t>(carg) + 1);
    CRef refSelf;
    refSelf.SetBasic(plo);
    aargs.push_back(refSelf);
    for (int i = 0; i < carg; ++i) aargs.push_back(aref[i]);
    CRef ref = RefApply(*pref, static_cast<int>(aargs.size()), aargs.data(), plo->pframe.get());
    if (prefResult) *prefResult = ref;  // (0x133000: the result assigned to the caller's CRef)
}

bool FLoSpliceEventTrue(LO* plo, uint32_t symid, int n)
{
    CRef ref;
    ref.SetBool(1);
    CRef arg;
    arg.SetS32(n);
    HandleLoSpliceEvent(plo, symid, false, 1, &arg, &ref);
    return ref.FTrue();
}


void PrintRef(const CRef& ref, std::string* ps);  // (transition.cpp, debug)

// orig 0x1331F8 for an SMA (smrun.h): its event in its SM's module frame, the SMA as the handler's object
namespace
{
void SmaScript(SMA* psma, int symid, int nA, int nB)
{
    if (std::getenv("SLY2N_POPRINT"))
        std::printf("sma script: sm %d symid %d (%d -> %d) frame %d binding %d\n", psma && psma->psm ? psma->psm->oid : -1, symid, nA, nB,
                    psma && psma->psm && psma->psm->pframe ? 1 : 0,
                    psma && psma->psm && psma->psm->pframe && psma->psm->pframe->PrefFindBinding(static_cast<uint32_t>(symid), 1) ? 1 : 0);
    if (!psma || !psma->psm || !psma->psm->pframe) return;
    CRef* pref = psma->psm->pframe->PrefFindBinding(static_cast<uint32_t>(symid), 1);
    if (!pref) return;
    if (std::getenv("SLY2N_POPRINT") && pref->m_tagk == TAGK_Proc && pref->m_pproc)
    {
        std::string str;
        CRef body;
        body.SetPair(pref->m_pproc->m_ppairCodeExpr);
        PrintRef(body, &str);
        std::printf("  %s\n", str.substr(0, 8000).c_str());
    }
    std::vector<CRef> a(1);
    a[0].SetBasic(psma);
    if (symid == 8)
    {
        a.resize(3);
        a[1].SetS32(nA);
        a[2].SetS32(nB);
    }
    RefApply(*pref, static_cast<int>(a.size()), a.data(), psma->psm->pframe.get());
}
// orig 0x1331F8 for an ASEGA (asega.h): its ASEG's module frame's handler of the event k (2 start, 3 retract, 4 the
// timed event 12 with the event's word, arg type 6), the ASEGA as the handler's object (scripts read its property
// 251, the object: the mine's fuse)
void AsegaScript(ASEGA* pasega, int symid, int n)
{
    ASEG* paseg = pasega ? pasega->paseg : nullptr;
    if (!paseg || !paseg->pframe) return;
    CRef* pref = paseg->pframe->PrefFindBinding(static_cast<uint32_t>(symid), 1);
    if (std::getenv("SLY2N_EVENTPRINT") || std::getenv("SLY2N_POPRINT"))
    {
        std::string str;
        if (pref && pref->m_tagk == TAGK_Proc && pref->m_pproc)
        {
            CRef body;
            body.SetPair(pref->m_pproc->m_ppairCodeExpr);
            PrintRef(body, &str);
        }
        std::printf("aseg event %d of %d (%d) object %d t %.3f %s\n", symid, paseg->oid, n, pasega->palo ? pasega->palo->oid : -1,
                    g_clock.t, str.substr(0, 1500).c_str());
    }
    if (!pref) return;
    std::vector<CRef> a(1);
    a[0].SetBasic(pasega);
    if (symid == 4)
    {
        a.resize(2);
        a[1].SetS32(n);
    }
    RefApply(*pref, static_cast<int>(a.size()), a.data(), paseg->pframe.get());
}
struct INITSMASCRIPT
{
    INITSMASCRIPT()
    {
        g_pfnSmaScript = SmaScript;
        g_pfnAsegaScript = AsegaScript;
    }
} s_initsmascript;
}

// orig 0x00149AB8 (once per object/member/argument on the pending list)
void PostDeferredCall(void* object, uint32_t pfnOrig, PFNDEFERREDCALL pfn, int arg)
{
    for (const DEFCALL& c : s_adefcall)
        if (c.object == object && c.pfnOrig == pfnOrig && !c.pfnPointer && c.arg == arg) return;
    s_adefcall.push_back({object, pfnOrig, pfn, arg});
}

// orig: 0x00149B58 with grf 1 (every pending call posted for the object removed)
void CancelDeferredCalls(void* object)
{
    s_adefcall.erase(std::remove_if(s_adefcall.begin(), s_adefcall.end(), [object](const DEFCALL& c) { return c.object == object; }),
                     s_adefcall.end());
}

// orig: 0x00149A50 (same pending FIFO, with no duplicate search)
void PostDeferredPointerCall(void* object, uint32_t pfnOrig, PFNDEFERREDPOINTERCALL pfn, void* arg)
{
    s_adefcall.push_back({object, pfnOrig, nullptr, 0, pfn, arg});
}

// orig 0x00149C80 (posted member calls, then the deferred splice events)
void RunDeferredSpliceEvents(SW* psw)
{
    PrintActivationDebug(psw);
    // The original detaches the whole FIFO before invoking its first member. A callback may post
    // another copy of a member in that detached batch; only the new pending list is deduplicated.
    while (!s_adefcall.empty())
    {
        std::vector<DEFCALL> batch;
        batch.swap(s_adefcall);
        for (const DEFCALL& c : batch)
        {
            if (std::getenv("SLY2N_POSTPRINT"))
                std::fprintf(stderr, "posted-call: t %.9g function %06X object %p arg %d\n", g_clock.t, c.pfnOrig, c.object, c.arg);
            if (c.pfnPointer) c.pfnPointer(c.object, c.pointerArg);
            else c.pfn(c.object, c.arg);
        }
    }
    // the events whose time has come, in order (the ones they queue for now run in this pass too)
    while (!s_adefev.empty() && s_adefev.front().t <= g_clock.t)
    {
        DEFEV ev = std::move(s_adefev.front());
        s_adefev.erase(s_adefev.begin());
        if (ev.plo) HandleLoSpliceEvent(ev.plo, ev.symid, false, static_cast<int>(ev.aref.size()), ev.aref.data());
        else
        {
            if (std::getenv("SLY2N_TASKDEBUG"))
            {
                std::string str;
                if (ev.refProc.m_tagk == TAGK_Proc && ev.refProc.m_pproc)
                {
                    CRef body;
                    body.SetPair(ev.refProc.m_pproc->m_ppairCodeExpr);
                    PrintRef(body, &str);
                }
                std::fprintf(stderr, "callback %d at %.3f: %s\n", ev.id, g_clock.t, str.substr(0, 800).c_str());
                if (ev.refProc.m_tagk == TAGK_Proc && ev.refProc.m_pproc && ev.refProc.m_pproc->m_pframe)
                    if (CRef* p1745 = ev.refProc.m_pproc->m_pframe->PrefFindBinding(1745, 1))
                    {
                        LO* plo = dynamic_cast<LO*>(p1745->m_pbasic);
                        std::fprintf(stderr, "   s1745 tag %d object %d\n", p1745->m_tagk, plo ? plo->oid : -1);
                        if (plo && plo->psidebag)
                        {
                            CRef r;
                            if (plo->psidebag->FFindBinding(1365, &r))
                            {
                                CRef res = RefApply(r, 0, nullptr, nullptr);
                                std::string sr;
                                PrintRef(res, &sr);
                                std::fprintf(stderr, "   (1365) = %s\n", sr.c_str());
                            }
                        }
                    }
                    for (uint32_t sym : {3358u, 1754u})
                        if (CRef* pr = ev.refProc.m_pproc->m_pframe->PrefFindBinding(sym, 1))
                        {
                            std::string strFn;
                            if (pr->m_tagk == TAGK_Proc && pr->m_pproc)
                            {
                                CRef f;
                                f.SetPair(pr->m_pproc->m_ppairFormals);
                                PrintRef(f, &strFn);
                                CRef b;
                                b.SetPair(pr->m_pproc->m_ppairCodeExpr);
                                PrintRef(b, &strFn);
                            }
                            std::fprintf(stderr, "   s%u = %s\n", sym, strFn.substr(0, 1500).c_str());
                        }
            }
            RefApply(ev.refProc, static_cast<int>(ev.aref.size()), ev.aref.data(), nullptr);
        }
    }
}

int IdScheduleCallback(float dt, const CRef& refProc, std::vector<CRef> aref)
{
    DEFEV ev{nullptr, 0, g_clock.t + dt, std::move(aref), refProc, ++s_idDefevLast};
    if (std::getenv("SLY2N_TASKDEBUG"))
        std::fprintf(stderr, "schedule-callback %d: dt %.3f proc tag %d, %zu args\n", ev.id, dt, refProc.m_tagk, ev.aref.size());
    const int id = ev.id;
    InsertDefev(std::move(ev));
    return id;
}

void CancelScheduledCallback(int id)
{
    if (id == -1) return;
    s_adefev.erase(std::remove_if(s_adefev.begin(), s_adefev.end(), [&](const DEFEV& e) { return e.id == id; }),
                   s_adefev.end());
}

void ClearDeferredSpliceEvents()
{
    if (std::getenv("SLY2N_POSTPRINT"))
        std::fprintf(stderr, "posted-clear: t %.9g calls %zu events %zu\n", g_clock.t, s_adefcall.size(), s_adefev.size());
    s_adefev.clear();
    s_adefcall.clear();
}

void StartSpawner(SW* psw, LO* plo);  // (spawner.cpp)

LO* g_ploFadeRecover = nullptr;   // g_game +0x3E60 (0x20E338): the fade after a fall's recovery (0x19F590)

// The slot-15 part of StartSw for one loaded object (its start: remove-at-start, the start script, the classes'
// PostLoads). (PC: also used for copies made while the world runs, pcloader.h PloPcCloneIntoWorld.)
void StartLoadedLo(SW* psw, LO* plo)
{
    if (plo->fRemoveAtStart)
    {
        plo->fRemoveAtStart = false;
        RemoveLo(psw, plo);
    }
    // orig: 0x001A5C48's vt slot 3 (0, a2 = 1): 0x1331F8 queues it (0x133040, at the clock's t). The batch is
    // drained below, after every object's remove-at-start (a parent's start that adds one of its children
    // back, e.g. the boot world's pumps 145035 choosing 145053 / 145074 / 145077, must survive that removal).
    if (const char* e = std::getenv("SLY2N_HASBIND"); e && plo->pframe && plo->pframe->PrefFindBinding(static_cast<SYMID>(std::atoi(e)), 1))
        std::fprintf(stderr, "hasbind %s: %d (cid 0x%X)\n", e, plo->oid, plo->Cid());  // (debug: the objects binding an event)
    if (const char* e = std::getenv("SLY2N_STARTPRINT"); e && std::atoi(e) == plo->oid && plo->pframe)
        if (CRef* pr = plo->pframe->PrefFindBinding(0, 1))
        {
            std::string str;
            CRef body;
            if (pr->m_tagk == TAGK_Proc && pr->m_pproc) body.SetPair(pr->m_pproc->m_ppairCodeExpr);
            PrintRef(pr->m_tagk == TAGK_Proc && pr->m_pproc ? body : *pr, &str);
            std::fprintf(stderr, "start of %d: %s\n", plo->oid, str.c_str());
        }
    HandleLoSpliceEvent(plo, 0, true, 0, nullptr);
    if (std::getenv("SLY2N_TASKDEBUG") && plo->pframe && plo->pframe->PrefFindBinding(0, 1))
        std::fprintf(stderr, "start event: %d (cid 0x%X)\n", plo->oid, plo->Cid());
    if (plo->pframe && plo->pframe->PrefFindBinding(1, 1))
    {
        if (plo->st == 2) SetLoSt(psw, plo, 0);
        if (plo->iupd >= 0) psw->aupd[plo->iupd].grf |= GRFUPD_Update;
        else plo->grfUpd |= GRFUPD_Update;
    }
    else plo->tNextUpdate = 3.4028235e38f;
    PostLoadDetector(plo); // CID10 loaded targets/fields before deferred startup
    PostLoadAnimLo(plo);  // ASEG 0x12F8F0 / SM 0x1F00C8 (anim.h)
    PostLoadCarryUse(plo); // CID24, orig0x1842B0: authored grip/hand references
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x147220) PostLoadAnimatedOverlay(plo);
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x14A0A8) PostLoadChasegraph(plo);  // cid 0x7B (aichase.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x225AA8) PostLoadTurret(plo);      // cid 0x45 (turret.cpp)
    PostLoadVehicle(plo);       // class0x4D, orig0x1D1518
    PostLoadVehiclePickup(plo); // class0x6C, orig0x1D4EC8
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1527C8) PostLoadDance(plo);       // cid 0x79 (dance.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x152100) PostLoadDancer(plo);      // cid 0x44 (dance.cpp)
    PostLoadLaser(plo);   // CID 0x55, orig 0x1DE568: endpoint tracks and broad-phase bounds
    PostLoadTrail(plo);   // CID 0x69, orig 0x21BD18: independent vertex-history meshes
    PostLoadPathRope(plo);  // paths: the loaded rope options (rope.cpp)
    PostLoadRopeMesh(plo);  // CID 0x6A, orig 0x1E9858: the meshes bent along a path
    // the players' start (0x1400B8; the rest is the actors'): their objects 0x2AB removed (vt slot 9), then slot
    // 79 (0): registered as player objects, not current
    if (FCidDerivedFrom(plo->Cid(), 0x34))
    {
        BindActorPartTable(PactorFromLo(plo));  // 0x1400B8: before posting restored booty to its holder
        if (ACTOR* pa = PactorFromLo(plo)) pa->asplash.clear();
        std::function<void(LO*)> walk = [&](LO* p) {
            for (LO* pc : std::vector<LO*>(p->aploOwned))
            {
                if (pc->oid == 0x2AB)
                {
                    // (0x15E650(G, 0x401, 0x2AB, p, 16): kept as its splash points, +0x1090 / +0x1094)
                    if (ACTOR* pa = PactorFromLo(plo); pa && pa->asplash.size() < 16) pa->asplash.emplace_back(pc, 0.0f);
                    RemoveLo(psw, pc);
                }
                walk(pc);
            }
        };
        walk(plo);
        EnablePo(psw, plo, 0);
        QueuePostLoadActorBooty(plo);  // 0x1400B8's posted 0x143BD8 (booty.cpp)
    }
    // 0x1C8030 (cid 7, the clue bottles): the world's n-th, removed when the save has it collected (the world's
    // key + afClue + n, 0x1C7B18)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1C8030)
    {
        const int n = psw->cClue++;
        SetClueIndex(plo, n);  // (pickup.cpp: the bottle's collect, 0x1C7EE8 with the proxy's +0x100)
        const auto& awrec = AwrecWorlds();
        const int i = g_transition.iwrecCur;
        if (i >= 0 && i < static_cast<int>(awrec.size()))
        {
            std::vector<int16_t> akey(awrec[i].an.begin(), awrec[i].an.end());
            akey.push_back(static_cast<int16_t>(NSymProgress("afClue")));
            akey.push_back(static_cast<int16_t>(n));
            const CRef r = RefGetProgressKey(akey);
            // (debug: SLY2N_CLUEKEEP keeps the collected ones: a bottle to collect with the 100% card)
            if (((r.m_tagk == TAGK_Bool && r.m_bool) || (r.m_tagk == TAGK_S32 && r.m_n)) && !std::getenv("SLY2N_CLUEKEEP")) RemoveLo(psw, plo);
            if (std::getenv("SLY2N_PICKUPPRINT"))
                std::printf("clue %d: proxy %d collected %d removed %d inworld %d\n", n, plo->oid, int(r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n), plo->fRemoved,
                            plo->fInWorld);
        }
    }
    // 0x1C4F60 (cid 0x5B..0x5F): its 0x12 child (+0x16C) removed, then slot 76 (0x1C6B28) with its state +0x158:
    // 1 becomes 2 (TODO: the sound 0x1FB2F8, the message 0x164640, event 5); 0x1C60B8: state 2 removes the object
    // (vt slot 9; TODO: its proxy 0x1CB658 / 0x1CB4B8), states 0 / 4 / 5 add the child back (vt slot 8), the
    // others leave it out (TODO: the links +0x1A4.., the state's SM +0x164 / +0x168)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1C4F60) PostLoadPickup(plo);  // (pickup.cpp, the actors code)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x229F88) PostLoadMarker(psw, plo);  // the job markers (marker.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x13BF00) PostLoadBooty(plo);              // the booty (booty.cpp)
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1EF850)  // the sky (cid 0x61, sky.cpp)
    {
        void PostLoadSky(SW * psw, LO * plo);
        PostLoadSky(psw, plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x13CEF8) PostLoadBootyStand(psw, plo);    // its stands
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x17F850)  // the hack ship (cid 0x48, hack.cpp)
    {
        void PostLoadHackShip(LO * plo);
        PostLoadHackShip(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x17F138)  // the hack base / pieces (cid 0x47 / 0x4A, hack.cpp)
    {
        void PostLoadHackSoBase(LO * plo);
        PostLoadHackSoBase(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1805C8)  // the hack enemies (cid 0x49, hack.cpp)
    {
        void PostLoadHackEnemy(LO * plo);
        PostLoadHackEnemy(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x181440)  // the hack docks (cid 0x4B, hack.cpp)
    {
        void PostLoadHackDock(LO * plo);
        PostLoadHackDock(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x181840)  // the hack swarms (cid 0x4C, hack.cpp)
    {
        void PostLoadHackSwarm(LO * plo);
        PostLoadHackSwarm(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x1B0D08)  // class 0x60 (bird.cpp)
    {
        void StartBirdLo(LO * plo);
        StartBirdLo(plo);
    }
    // 0x182F28 / 0x1833E0 / 0x183768 (a ledge / wall / swing's start): its own camera zone (the description's +0)
    // forced off (0x221268(zone, 0)): it is switched on only while a player holds on to it (0x192BD8 -> 0x140C88)
    if (plo->pcls)
    {
        const uint32_t pfn = plo->pcls->PfnOrig(15);
        const JTL jtl = pfn == 0x182F28 ? JTL_Ledge : pfn == 0x1833E0 ? JTL_Wall : pfn == 0x183768 ? JTL_Swing : JTL_Max;
        if (jtl != JTL_Max)
            if (LO* pz = PloJtDescZone(plo, jtl)) SetZoneForced(pz, 0);
    }
    // 0x1C3458 / 0x182F28 / 0x1CB950 (cid 0xB / 0xC / 0xD): the radius from the path's points (0x1E7D90) + 1250
    if (plo->pcls && (plo->pcls->PfnOrig(15) == 0x1C3458 || plo->pcls->PfnOrig(15) == 0x182F28 ||
                      plo->pcls->PfnOrig(15) == 0x1CB950) && plo->sRadius == 1e10f)
        if (LOXPATH* pp = dynamic_cast<LOXPATH*>(plo))
        {
            const glm::mat3 m = glm::mat3(pp->pxf ? pp->pxf->matWorld * pp->mat : pp->mat);
            float s2 = 0.0f;
            for (const glm::vec3& v : pp->avecF4) s2 = std::max(s2, glm::dot(m * v, m * v));  // (TODO: the Bezier controls)
            SetLoRadius(psw, plo, std::sqrt(s2) + 1250.0f);
        }
    // 0x1833E0 (a wall, cid 0x22): the radius its +0x10C (the wall's own radius) + 1250 (D_002BE8C0);
    // 0x183768 (a swing, cid 0x26): 1250 (D_002BE8C8); both only when unset (1e10)
    if (plo->pcls && plo->sRadius == 1e10f)
    {
        const uint32_t pfn = plo->pcls->PfnOrig(15);
        if (pfn == 0x1833E0) SetLoRadius(psw, plo, std::bit_cast<float>(plo->U32Field(0, 0x10C, 0)) + 1250.0f);
        else if (pfn == 0x183768) SetLoRadius(psw, plo, 1250.0f);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x20AE18)
    {
        void PostLoadGuardAction(LO* plo);
        PostLoadGuardAction(plo);
    }
    if (plo->pcls && plo->pcls->PfnOrig(15) == 0x20B500) PostLoadGuardManager(plo);
    // the classes' slot 15 after the base (0x162780 / 0x1638F8)
    if (plo->Cid() == 0x84) StartJob(psw, static_cast<JOB*>(plo));
    else if (plo->Cid() == 0x85) StartTask(psw, static_cast<TASK*>(plo));
    else if (plo->pcls && (plo->pcls->PfnOrig(15) == 0x2061D8 || plo->pcls->PfnOrig(15) == 0x1BA018 ||
                           plo->pcls->PfnOrig(15) == 0x155450 || plo->pcls->PfnOrig(15) == 0x14B1D8))
        PostLoadGuard(plo);  // the guards (guardstart.cpp; 0x3C's 0x1BA018, 0x3B's 0x155450 and 0x3F's 0x14B1D8 start
                             // with the base 0x2061D8, TODO(ai) their rest: 0x3B's +0x1014 records' four 0x15E938
                             // lookups, 0x3F's 0x03 object oid 0x62B at +0x1010)
    // 0x1733F8 (cid 0x77; TODO: its entries' 0x71 objects) and 0x2123C8 (cid 0x7E): kind 1, updated
    // (cid 0x81's 0x211EC0 starts with 0x2123C8; TODO: its two 0x15E938 lookups)
    else if (plo->pcls && (plo->pcls->PfnOrig(15) == 0x1733F8 || plo->pcls->PfnOrig(15) == 0x2123C8 ||
                           plo->pcls->PfnOrig(15) == 0x211EC0))
    {
        if (plo->pcls->PfnOrig(15) == 0x1733F8) StartSpawner(psw, plo);
        SetLoSt(psw, plo, 1);
        const uint32_t grf = plo->pcls->PfnOrig(15) == 0x1733F8 ? 2u : 0x102u;
        if (plo->iupd >= 0) psw->aupd[plo->iupd].grf |= grf;
        else plo->grfUpd |= grf;
    }
}

// orig 0x0020E740 (part) / 0x001A5C48: the objects of the load start: deferred load callbacks (option 26's
// PostLoad), the update flag from the object's module (a binding of symid 1), then the update table
void StartSw(SW* psw, bool fInitHud, PFNSWPASS pfnPostLoadPlayers)
{
    // orig 0x20E338: the world's 0x74 object (no oid; +0x60 0.1, +0x64 0.25, +0x54 0x80200000, +0x6C 1; PostLoad)
    {
        LO* plo = PloNew(0x74, psw, nullptr, -1, -1);
        plo->SetField(0, 0x60, std::bit_cast<uint32_t>(0.1f));
        plo->SetField(0, 0x64, std::bit_cast<uint32_t>(0.25f));
        plo->SetField(0, 0x54, 0x80200000u);
        plo->SetField(0, 0x6C, 1u);
        RemoveLo(psw, plo);   // (vt slot 9)
        g_ploFadeRecover = plo;
    }
    // TODO(M3): 0x20E2B0 (the class 0x31 object with the largest +0xB8), 0x1FF828, 0x1CA500 (the unused
    // instances' keys), 0x20F710
    // orig 0x1569B8: seven 0x75 objects, oids 706..712 (in this order: 706, 707, 708, 709, 711, 710, 712), posted
    for (OID oid : {0x2C2, 0x2C3, 0x2C4, 0x2C5, 0x2C7, 0x2C6, 0x2C8}) RemoveLo(psw, PloNew(0x75, psw, nullptr, oid, -1));
    // orig 0x1A3260: a default light (oid 392) when no light of kind 0 (+0x160) exists (TODO: the kind; the
    // worlds' files carry their lights)
    InitSwNav(psw);  // orig 0x1C10C8: populate and bind the default navigation mesh when no group loaded a BSP
    psw->fNavReady = true;
    // TODO(M3): 0x210940 (the object 0x620 at SW +0x4638), 0x1957C0
    // vt slot 15 of each loaded object (0x1A5C48 and the classes' starts)
    const std::vector<LO*> aploLoad = psw->aploLoad;
    for (LO* plo : aploLoad)
    {
        const CID cid = plo->Cid();
        if (FCidDerivedFrom(cid, 0x33))
        {
            // orig 0x203A38 (the POs' start): their 0x0A object oid 713 (0x1E94B0(713, 16)); TODO(M4): +0x58/+0x5C
            // links, the body part +0x264, 0x1EEC70
            PloNew(0x0A, psw, nullptr, 0x2C9, -1);
            // (the guards' 0x71 object oid 715 of 0x2061D8: PostLoadGuard's home list, guardstart.cpp)
            // (the players' 0x0F object oid 328 of 0x1400B8 -> 0x1910F8: their interaction's camera zone, made by
            // PostLoadInteract, interact.cpp)
        }
        // TODO(M4): 0x1733F8 (cid 0x77: a 0x71 per entry), 0x1BDA00 (0x0E oid 717), the actors' emitters
    }
    // vt slot 15 (0x1A5C48) of each: the deferred callbacks (0x1A5D98: option 26's RemoveLo), the "start" splice
    // event (vt slot 3 (0, 1, ...)), then an object with an update script (a binding of symid 1) gets the
    // activation kind 0 (when 2) and the update flag; the others never get the update event (+0x30 = FLT_MAX)
    InitTasks(psw);
    InitWorldAnimationRecords(psw);  // (anim.h: records, joints, bones, ASEG / SM tables)
    // orig: 0x0020E740 calls 0x001D9520 before pending post-load calls and checkpoint/player selection.
    // Offline model/animation probes do not start gameplay widgets.
    if (fInitHud) InitHudWidgets(psw);
    for (LO* plo : aploLoad) StartLoadedLo(psw, plo);
    psw->aploLoad.clear();
    // orig: 0x001400B8 -> 0x001440D0 during 0x0020E740's slot-15 batch:
    // bind counters before deferred starts and broadcast 4 changes a pending
    // task request from 8 to 4. Native animation records must be ready first.
    if (pfnPostLoadPlayers) pfnPostLoadPlayers(psw);
    ResetGuardsAtStart(psw);  // (the guards' deferred 0x206170, guardstart.cpp)
    // orig: 0x0020E828 (0x149C80 after the slot-15 batch): start scripts finish before broadcast 4 and the
    // transition's broadcast 5 activates tasks whose handlers use bindings installed by those scripts.
    RunDeferredSpliceEvents(psw);
    // TODO(M3): 0x210940, 0x1957C0, 0x1D9520 (the HUD), 0x149C80, 0x1C1260, 0x14D470
    BroadcastTaskEvent(psw, 4, nullptr);  // orig 0x20E868
}

// orig 0x00226550 UpdateSw
void MurrayTeleportPreUpdate(SW* psw);  // gang.cpp (debug)
void UpdateSw(SW* psw)
{
    MurrayTeleportPreUpdate(psw);
    RunDeferredSpliceEvents(psw);
    Pass(0x149C80, psw);
    UpdateSwActivation(psw);  // orig 0x147F40
    Pass(0x147F40, psw);
    if (g_clock.dt <= 0.0f)
    {
        // the paused update (0x2267C0): vt slot 16 of the entries with flags 0x102 (e.g. a script on the real clock),
        // then the passes (the ASEGAs and records on the real clock: anim.cpp)
        for (psw->iupdCur = 0; psw->iupdCur < static_cast<int>(psw->aupd.size()); ++psw->iupdCur)
        {
            const UPDENT& e = psw->aupd[psw->iupdCur];
            if ((e.grf & (GRFUPD_Update | GRFUPD_RealTime)) != (GRFUPD_Update | GRFUPD_RealTime) || !e.plo) continue;
            if (FCoopSkipUpdate(e.plo)) continue;
            if (PFNUPDATE pfn = PfnUpdateForClass(e.plo->pcls)) pfn(e.plo);
        }
        psw->iupdCur = -1;
        Pass(0x2267C0, psw);
    }
    else
    {
        // (debug: SLY2N_UPDORDER=oid,oid..: those objects' update-table places when they change)
        if (static const char* s_szUo = std::getenv("SLY2N_UPDORDER"); s_szUo)
        {
            static std::string s_strLast;
            std::string str;
            for (const char* q = s_szUo; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                if (LO* plo = PloFindSwObjectByOid(psw, std::atoi(q)))
                    str += " " + std::to_string(plo->oid) + ":" + std::to_string(plo->iupd);
            if (str != s_strLast) std::printf("updorder t %.3f:%s (of %zu)\n", g_clock.t, str.c_str(), psw->aupd.size());
            s_strLast = str;
        }
        Pass(0x1C4468, psw);  // per SW +0x3EAC list entry
        Pass(0x1F77C0, psw);
        for (psw->iupdCur = 0; psw->iupdCur < static_cast<int>(psw->aupd.size()); ++psw->iupdCur)
        {
            const UPDENT& e = psw->aupd[psw->iupdCur];
            if (!(e.grf & GRFUPD_Update) || !e.plo) continue;
            if (FDebugSkipUpdate(psw, e.plo)) continue;   // (debugmenu.h: the noclip's player, placed by the menu)
            if (FCoopSkipUpdate(e.plo)) continue;         // (coop.h: the joiner's guards show the host's)
            if (PFNUPDATE pfn = PfnUpdateForClass(e.plo->pcls)) pfn(e.plo);  // vt slot 16
            else if (std::getenv("SLY2N_UPDMISS"))
            {
                // (debug: the classes whose update (vt slot 16) is not native, once each)
                static std::unordered_map<uint32_t, bool> s_mp;
                const uint32_t pfnOrig = e.plo->pcls ? e.plo->pcls->PfnOrig(16) : 0;
                if (!s_mp[pfnOrig])
                {
                    s_mp[pfnOrig] = true;
                    std::fprintf(stderr, "update: cid 0x%X's slot 16 0x%X not native (object %d)\n", e.plo->Cid(), pfnOrig, e.plo->oid);
                }
            }
        }
        psw->iupdCur = -1;
        Pass(0x19F120, psw);
        Pass(0x130918, psw);  // per SW +0x3D88 list entry
        Pass(0x156728, psw);  // per SW +0x3FFC list entry
        RunDeferredSpliceEvents(psw);
        Pass(0x149C80, psw);
        Pass(0x1F8DD8, psw);
        Pass(0x1BE1E8, psw);
        Pass(0x1EC968, psw);
        RunDeferredSpliceEvents(psw);
        Pass(0x149C80, psw);
    }
    Pass(0x1AB1A0, psw);
    Pass(0x1FD830, psw);  // the music
    Pass(0x14D478, psw);  // the camera (when 0x2DDED0 +0x10 > 0)
    Pass(0x20F7E0, psw);
    Pass(0x128148, psw);
}

// orig: 0x001F7598's vt slot 3 calls (0x1A61E0 -> 0x1331F8, deferred, two arguments): event 0xA to the sender with
// (the receiver, the filter) and 0xB to the receiver with (the sender, the filter): e.g. the Ep4 Contessa's event 11
// asks her job (1694) for the next listen-in task when the filter is the dart's 0x80000
namespace
{
void LoContactEvent(LO* plo, int n, int fDeferred, int carg, void* pv, LO* ploOther)
{
    if (!plo || carg != 2 || !pv) return;
    const CTMSG* pctm = static_cast<const CTMSG*>(pv);
    CRef aref[2];
    if (ploOther) aref[0].SetBasic(ploOther);
    aref[1].SetS32(static_cast<int>(pctm->grfFilter));
    HandleLoSpliceEvent(plo, static_cast<uint32_t>(n), fDeferred != 0, 2, aref);
}
struct INITCONTACTEVENT
{
    INITCONTACTEVENT() { g_pfnLoEvent = LoContactEvent; }
} s_initcontactevent;
}  // namespace
