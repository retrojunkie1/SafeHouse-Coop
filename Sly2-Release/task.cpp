#include "coop.h"
#include "po.h"
#include "actor.h"
#include "booty.h"
#include "task.h"
#include "replay.h"
#include "guard.h"
#include "animfind.h"
#include "joy.h"
#include "screenrt.h"
void PauseGame(int grf);   // ai.cpp: orig 0x20EFC0
void ResumeGame(int grf);  // ai.cpp: orig 0x20F030

#include "activate.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "objmethod.h"
#include "savedata.h"
#include "smrun.h"
#include "sw.h"
#include "transition.h"
#include "update.h"
#include "frontend.h"
#include "hudwidget.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

TASKMGR g_taskmgr;

void RequestLoTaskState(LO* plo, int n, LO* ploContext);  // (below)
void PrintScreenObjState(LO* plo);
void TriggerExit(LO* plo);  // (frontend.cpp, orig 0x2283D0)
namespace
{
SW* s_psw = nullptr;

TASK* PtaskFromLo(LO* plo) { return plo && plo->Cid() == 0x85 ? static_cast<TASK*>(plo) : nullptr; }
JOB* PjobFromLo(LO* plo) { return plo && plo->Cid() == 0x84 ? static_cast<JOB*>(plo) : nullptr; }

// orig 0x15E938 (kinds 5 / 0x105 / 0x405: global; 4: under the owner): natively by oid
LO* PloFind(OID oid, LO* ploOwner = nullptr)
{
    if (oid == -1 || !s_psw) return nullptr;
    if (ploOwner)
    {
        std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
            for (LO* pc : p->aploOwned)
            {
                if (pc->oid == oid || pc->oidInstance == oid) return pc;
                if (LO* q = find(pc)) return q;
            }
            return nullptr;
        };
        if (LO* p = find(ploOwner)) return p;
    }
    if (LO* p = PloFindSwObjectByOid(s_psw, oid)) return p;
    // an instance's oid (a proxy's, 0x1C99D8's namespace node) names the instance's first object
    for (LO* const p : AploOfOid(s_psw, oid))
        if (p->oidInstance == oid) return p;
    return nullptr;
}
TASK* PtaskFind(OID oid) { return PtaskFromLo(PloFind(oid)); }

std::vector<TASK*> AptaskAll()
{
    std::vector<TASK*> a;
    if (s_psw)
        for (LO* plo : s_psw->mpcidaplo[0x85]) a.push_back(static_cast<TASK*>(plo));
    return a;
}

// orig 0x0020E040: a world change is under way
bool FTransitionBusy() { return g_transition.fPending || g_transition.wipes == WIPES_Out; }

// orig 0x001637E8 / 0x00162670: the task / job reacts (not filtered out, not being restored, no world change)
bool FTaskLive(const TASK* p) { return !p->fMission && p->nMode != 2 && p->nMode != 3 && !FTransitionBusy(); }
bool FJobLive(const JOB* p) { return !p->fMission && p->nMode != 2 && p->nMode != 3 && !FTransitionBusy(); }
bool FJobCur(const JOB* p) { return s_psw && s_psw->tasks.pjobCur == p; }     // orig 0x001633E8
bool FTaskCur(const TASK* p) { return s_psw && s_psw->tasks.ptaskCur == p; }  // orig 0x00164018

// orig 0x00165DC0 / 0x00165DE8 / 0x00165E10
void SetJobCur(JOB* p)
{
    s_psw->tasks.pjobCur = p;
    g_taskmgr.oidJob = p ? p->oid : -1;
}
void SetTaskCur(TASK* p)
{
    s_psw->tasks.ptaskCur = p;
    g_taskmgr.oidTask = p ? p->oid : -1;
}
void SetTaskChk(TASK* p)
{
    s_psw->tasks.ptaskChk = p;
    g_taskmgr.oidQueued = p ? p->oid : -1;
}

// CSave: a task's (mode, context) and its job's set for the time of a call
struct CSAVE
{
    TASK* ptask = nullptr;
    JOB* pjob = nullptr;
    int n0 = 0;
    LO* plo0 = nullptr;
    CSAVE(TASK* p, int nMode, LO* ploContext) : ptask(p)
    {
        if (!p) return;
        n0 = p->nMode;
        plo0 = p->ploContext;
        p->nMode = nMode;
        p->ploContext = ploContext;
    }
    CSAVE(JOB* p, int nMode, LO* ploContext) : pjob(p)
    {
        if (!p) return;
        n0 = p->nMode;
        plo0 = p->ploContext;
        p->nMode = nMode;
        p->ploContext = ploContext;
    }
    ~CSAVE()
    {
        if (ptask) { ptask->nMode = n0; ptask->ploContext = plo0; }
        if (pjob) { pjob->nMode = n0; pjob->ploContext = plo0; }
    }
};

// vt slot 3 (0x1A61E0 -> 0x1331F8): an event to the object's script; ints and objects as the event's table
// (0x27A1D8 / 0x27A10C) types them
CRef RefS32(int n) { CRef r; r.SetS32(n); return r; }
CRef RefLo(LO* plo) { CRef r; r.SetBasic(plo); return r; }
void Msg(LO* plo, uint32_t symid, std::vector<CRef> aref = {})
{
    HandleLoSpliceEvent(plo, symid, false, static_cast<int>(aref.size()), aref.data());
}

// orig 0x00161FC8 / 0x00161FF0
uint32_t GrfState(int n) { return n == -1 ? 0u : (1u << n); }

// orig 0x002581C0 / 0x00258200: a progress reference read / written in the save
int NProgress(const PROGREF& pr, int nDefault = 0)
{
    const CRef r = RefGetProgressKey(pr.akey);
    if (r.m_tagk == TAGK_S32) return r.m_n;
    if (r.m_tagk == TAGK_Bool) return r.m_bool;
    if (r.m_tagk == TAGK_F32) return static_cast<int>(r.m_g);
    return nDefault;
}
float GProgress(const PROGREF& pr)
{
    const CRef r = RefGetProgressKey(pr.akey);
    return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? static_cast<float>(r.m_n) : 0.0f;
}
void SetProgressInt(const PROGREF& pr, int n) { if (pr.F()) FSetProgressKey(pr.akey, RefS32(n)); }
void SetProgressBool(const PROGREF& pr, bool f)
{
    if (!pr.F()) return;
    CRef r;
    r.SetBool(f);
    FSetProgressKey(pr.akey, r);
}

// orig 0x001620B0: the objects of the state n's group (0..3 or 4..5) added (in the state) or removed
void ApplyTaskObjects(int n, const std::vector<TASKOBJE>& ae)
{
    const uint32_t grfState = GrfState(n);
    const uint32_t grfAll = (n == 4 || n == 5) ? (GrfState(4) | GrfState(5)) : 0xFu;
    for (const TASKOBJE& e : ae)
    {
        if (!(e.grf & grfAll)) continue;
        if (e.iwrec != -1 && e.iwrec != g_transition.iwrecCur) continue;
        LO* ploOwner = PloFind(e.oidB);
        LO* plo = PloFind(e.oidA, ploOwner);
        if (std::getenv("SLY2N_TASKDEBUG"))
            std::fprintf(stderr, "    objects n %d: oid %d owner %d grf 0x%x world %d -> %s %s\n", n, e.oidA, e.oidB, e.grf,
                         e.iwrec, plo ? "found" : "none", plo && (e.grf & grfState) ? "add" : "remove");
        if (!plo) continue;
        if (e.grf & grfState) AddLo(s_psw, plo);  // vt slot 8
        else RemoveLo(s_psw, plo);               // vt slot 9
    }
}

// orig 0x001623D0 / 0x00162250: the state machines' goals for the state n (an entry's own state, or by the state:
// 0x612..0x615), at once in the modes 1 / 4 (0x1F0948), else run to (0x1F0660)
void ApplyTaskSms(int n, const std::vector<TASKSME>& ae, int nMode)
{
    for (const TASKSME& e : ae)
    {
        if (std::getenv("SLY2N_TASKDEBUG"))
            std::fprintf(stderr, "    sme n %d mode %d: state %d object %d sm %d id %d instant %d world %d\n", n, nMode, e.nState,
                         e.oid, e.iSm, e.idState, e.fInstant, e.iwrec);
        if (e.nState != -1 && e.nState != n) continue;
        if (e.fInstant && !nMode) continue;
        if (e.iwrec != -1 && e.iwrec != g_transition.iwrecCur) continue;
        LO* plo = PloFind(e.oid);
        if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        SMA* psma = nullptr;  // orig 0x1F67E0: the object's SMA (of the SM oid, -1: any)
        for (SMA* p : g_apsma)
            if (p->palo == plo && (e.iSm == -1 || (p->psm && p->psm->oid == e.iSm))) { psma = p; break; }
        if (std::getenv("SLY2N_TASKDEBUG"))
            std::fprintf(stderr, "    sm n %d: object %d sm %d state %d -> %s\n", n, e.oid, e.iSm, e.idState, psma ? "found" : "no sma");
        if (!psma) continue;
        int id = e.idState;
        if (e.nState == -1 && n >= 0 && n <= 3) id = 0x612 + n;
        if (id == -1) continue;
        if (nMode == 1 || nMode == 4) SnapSmaGoal(psma, id);
        else SetSmaGoal(psma, id);
    }
}

// orig 0x001643B0 / 0x00163460: the task's / job's state into the save
bool FTaskDone(TASK* p);
void SaveTask(TASK* p);
void SaveJob(JOB* p)
{
    SetProgressInt(p->prD0, p->cPlayed);
    if (p->prD8.F()) SetProgressBool(p->prD8, p->ptaskLast && FTaskDone(p->ptaskLast));  // 0x163400
    if (p->prE0.F())
    {
        CRef r;
        r.SetF32(p->gA4);
        FSetProgressKey(p->prE0.akey, r);
    }
}
void SaveTask(TASK* p)
{
    if (p->nMode == 1 || p->nState == -1) return;
    SetProgressInt(p->prC8, p->cPlayed);
    SetProgressInt(p->prD0, p->cDone);
    SetProgressBool(p->prD8, FTaskDone(p));
    SetProgressInt(p->prE0, p->nState);
    SetProgressBool(p->prE8, false);
    if (p->pjob) SaveJob(p->pjob);
}

// orig 0x00164030
bool FTaskDone(TASK* p)
{
    if (p->nState == -1 && p->prD8.F()) return NProgress(p->prD8) != 0;
    return p->nState >= 2;
}

// orig 0x001645D0: the prerequisites are done (or available and +0x6C)
bool FTaskPrereqs(TASK* p)
{
    for (TASK* px : p->aptaskPrereq)
        if (!(px->nState >= 2 || (px->nState == 1 && px->f6c))) return false;
    return true;
}

void ActivateTask(TASK* p);
void DeactivateTaskCur(TASK* p);
void ActivateJob(JOB* p);
void JobBegin(JOB* p);
void JobEndIfCur(JOB* p);
void JobSwitch(JOB* p);
void JobTaskReached(JOB* pjob, TASK* ptask);

// orig 0x001657D0
void SetTaskState(TASK* p, int n)
{
    if (p->nState == n) return;
    if (n == 1 && std::getenv("SLY2N_TASKDEBUG")) std::printf("task %d: available (%d -> 1) t %.3f\n", p->oid, p->nState, g_clock.t);
    if (FTaskLive(p))
    {
        BroadcastTaskEvent(s_psw, 0x26, p);
        Msg(p, 0x16);
    }
    auto& aact = s_psw->tasks.aptaskActive;
    if (p->nState == 1) aact.erase(std::remove(aact.begin(), aact.end(), p), aact.end());
    if (FTaskLive(p))
    {
        ApplyTaskObjects(n, p->aobje);
        ApplyTaskSms(n, p->asme, p->nMode);
    }
    switch (n)
    {
    case 0:
        if (p->nMode == 4)
        {
            p->cPlayed = 0;
            p->cDone = 0;
            if (p->pjob && p->pjob->ptaskFirst == p) p->pjob->cPlayed = 0;  // (orig: the job's +0x7C)
        }
        break;
    case 1: aact.push_back(p); break;
    case 2:
        if (p->nMode == 0) ++p->cDone;
        break;
    }
    p->nState = n;
    p->tState = g_clock.t;
    SaveTask(p);
    if (FTaskLive(p))
    {
        BroadcastTaskEvent(s_psw, 0x25, p);
        Msg(p, 0x15);
        // vt slot 4 (0x11, p): the BASIC notification (TODO)
    }
    switch (p->nMode)
    {
    case 1:
        if (p->nState == 1 && p->f74) g_taskmgr.oidTask = p->oid;
        break;
    case 0: case 2: case 4:
        if (p->nState == 1)
        {
            if (p->f70) ActivateTask(p);
        }
        else if (p->nState == 0 || p->nState == 3) DeactivateTaskCur(p);
        break;
    default: break;
    }
}

// orig 0x00165A60 / 0x00165B40: the state, then the successors' from it
void SetTaskStateTree(TASK* p, int n);
void SuccessorState(TASK* p, int n)
{
    if (p->nPending != -1) return;
    int nNew = p->nState;
    if (n >= 0)
    {
        if (n < 2) nNew = 0;
        else if (n < 4)
        {
            bool f = true;
            for (TASK* px : p->aptaskPrereq)
                if (px->nState < 2) { f = false; break; }
            if (!f) nNew = 0;
            else if (p->nState == 0) nNew = 1;
            else if (p->nState == 3) nNew = n;
        }
    }
    SetTaskStateTree(p, nNew);
}
void SetTaskStateTree(TASK* p, int n)
{
    if (p->nState == n) return;
    SetTaskState(p, n);
    for (TASK* px : p->aptaskNext)
    {
        CSAVE save(px, p->nMode, p->ploContext);
        SuccessorState(px, p->nState);
    }
}

// orig 0x00164CE8: the successors that change with a state change to n
void CollectTasks(TASK* p, int n, std::vector<TASK*>& a)
{
    for (TASK* px : p->aptaskPrereq)
    {
        int nChild = n;
        if (n == 2 && (!px->pjob || (px->pjob != p->pjob && !px->pjob->f78))) nChild = 3;
        CollectTasks(px, nChild, a);
    }
    if (p->nPending == -1 && p->nState < n)
    {
        a.push_back(p);
        p->nPending = n;
    }
}

// orig 0x00164DF0
void ChangeTaskState(TASK* p, int n)
{
    const int nOld = p->nState;
    if (n == nOld || g_taskmgr.fBusy) return;
    CoopOnTaskState(p->oid, nOld, n);   // (coop.h: a step started or done here is told to the other game)
    g_taskmgr.fBusy = true;
    int nFilter = -1;
    switch (n)
    {
    case 1: case 2: nFilter = 2; break;
    case 3: nFilter = 3; break;
    default: break;
    }
    if (nFilter != -1)
    {
        std::vector<TASK*> a;
        p->nPending = n;
        CollectTasks(p, nFilter, a);
        for (TASK* px : a)
        {
            CSAVE save(px, p->nMode, p->ploContext);
            SetTaskStateTree(px, px->nPending);
        }
        for (TASK* px : a) px->nPending = -1;
        p->nPending = -1;
    }
    SetTaskStateTree(p, n);
    g_taskmgr.fBusy = false;
    // the episode's percentage into the progress (SW +0x3B7C[episode]: "episode <name> percent_complete", 0x161660)
    {
        static const char* const s_aszEpisode[9] = {"intro", "nightclub", "palace", "temple", "prison", "castle", "train", "sawmill", "blimp"};
        const int iep = IepisodeCur();   // (0x210AE0)
        if (iep >= 0 && iep < 9)
        {
            CRef ref;
            ref.SetS32(NTasksPercentDone(s_psw));
            FSetProgress({"episode", s_aszEpisode[iep], "percent_complete"}, ref);
        }
    }
    if (std::getenv("SLY2N_TASKDEBUG") && p->nState >= 2 && nOld < 2)
        std::printf("task %d: %d -> %d (live %d mission %d mode %d transition %d job %d) t %.3f\n", p->oid, nOld, p->nState, FTaskLive(p),
                    int(p->fMission), p->nMode, int(FTransitionBusy()), p->pjob ? p->pjob->oid : -1, g_clock.t);
    if (FTaskLive(p) && nOld != -1 && nOld < 2 && p->nState >= 2)
    {
        if (p->pjob)
        {
            JobTaskReached(p->pjob, p);
            if (p->pjob) return;
        }
        // prerequisites (+0x94) and every successor in a job: the save (0x1ABD98)
        if (!p->aptaskPrereq.empty())
        {
            bool fSave = true;
            for (TASK* px : p->aptaskNext) fSave &= px->pjob != nullptr;
            if (fSave) HudAutoSave(false);
        }
    }
}

// orig 0x00163E88: the task refused the change n (message 0x12)
void TaskRefused(TASK* p, int n)
{
    if (FTaskLive(p))
    {
        g_taskmgr.n8 = n;
        BroadcastTaskEvent(s_psw, 0x27, p);
        Msg(p, 0x12, {RefS32(n)});
    }
    if (p->pjob && FJobLive(p->pjob)) Msg(p->pjob, 0x12, {RefS32(n)});
}

// orig 0x00163F48 / 0x00163CE0: the current task left
void DeactivateTask(TASK* p)
{
    SetTaskCur(nullptr);
    if (FTaskLive(p))
    {
        BroadcastTaskEvent(s_psw, 0x24, p);
        ApplyTaskObjects(5, p->aobje);
        ApplyTaskSms(5, p->asme, p->nMode);
        if (p->idDialog5C != -1 && p->f60) HudPromptEnd(p->idDialog5C);   // 0x1DA578 on 0x3DA0C0
        Msg(p, 0x14);
    }
}
void DeactivateTaskCur(TASK* p)
{
    if (FTaskCur(p)) DeactivateTask(p);
}

// orig 0x00163D18 / 0x00163C28: the task becomes the current one
void ActivateTaskNow(TASK* p)
{
    if (std::getenv("SLY2N_TASKDEBUG")) std::printf("task %d: activated (t %.3f) exit %d state %d\n", p->oid, g_clock.t,
                                                    p->ploC4 ? p->ploC4->oid : -1, p->ploC4 ? p->ploC4->nState : -9);
    SetTaskCur(p);
    if (p->idChk != -1) SetTaskChk(p);
    if (p->pjob && !FJobCur(p->pjob)) JobBegin(p->pjob);  // orig 0x162D50
    if (FTaskLive(p))
    {
        ++p->cPlayed;
        SaveTask(p);
        BroadcastTaskEvent(s_psw, 0x23, p);
        ApplyTaskObjects(4, p->aobje);
        ApplyTaskSms(4, p->asme, p->nMode);
        if (p->idDialog5C != -1 && p->f60) HudPrompt(p->idDialog5C, -1.0f);   // 0x1DA4B8 on 0x3DA0C0
        Msg(p, 0x13);
        // the task's exit (+0xC4, e.g. chalktalk 1799's back to the safe house after Recon): updated on the real
        // clock (+0x40 |= 0x100: its update entry's flags) and fired (0x2283D0), as JobCompleteNext's job exit
        if (LO* pexit = p->ploC4)
        {
            (pexit->iupd >= 0 ? s_psw->aupd[static_cast<size_t>(pexit->iupd)].grf : pexit->grfUpd) |= 0x100;
            TriggerExit(pexit);
        }
    }
}
void ActivateTask(TASK* p)
{
    if (FTaskCur(p)) return;
    if (TASK* pcur = s_psw->tasks.ptaskCur)
    {
        CSAVE save(pcur, p->nMode, p->ploContext);
        DeactivateTaskCur(pcur);
    }
    ActivateTaskNow(p);
}

// orig 0x00164640 (method 1280): ask for the state n (-1 / 4: activate, 5: deactivate)
void RequestTaskState(TASK* p, int n, LO* ploContext)
{
    CSAVE save(p, p->nMode, ploContext);
    CSAVE saveJob(p->pjob, p->nMode, ploContext);
    if (n == -1) n = 4;
    const int nCur = p->nState;
    if (std::getenv("SLY2N_TASKDEBUG")) std::printf("task %d: state %d requested (now %d)\n", p->oid, n, nCur);
    if (nCur == -1) return;
    switch (n)
    {
    case 4:
        if (nCur == 0)
        {
            if (FTaskPrereqs(p))
            {
                ChangeTaskState(p, 1);
                ActivateTask(p);
            }
            else TaskRefused(p, 4);
        }
        else if (nCur != 3) ActivateTask(p);
        break;
    case 5: DeactivateTaskCur(p); break;
    case 0: case 1: case 2: case 3:
    {
        bool f = n - nCur == 1;
        if (nCur == 0 && n == 1) f = FTaskPrereqs(p);
        else if (nCur == 1 && n == 3) f = true;
        else if (nCur == 3) return;
        if (f) ChangeTaskState(p, n);
        else TaskRefused(p, n);
        break;
    }
    default: break;
    }
}

// orig 0x00164858 (method 1281): the state n without the checks
void ForceTaskState(TASK* p, int n, LO* ploContext)
{
    CSAVE save(p, p->nMode, ploContext);
    CSAVE saveJob(p->pjob, p->nMode, ploContext);
    if (n == -1) n = 4;
    if (p->nState == -1) return;
    switch (n)
    {
    case 4:
        if (p->nState == 0 && FTaskPrereqs(p)) ChangeTaskState(p, 1);
        ActivateTask(p);
        break;
    case 5: DeactivateTaskCur(p); break;
    case 0: case 1: case 2: case 3: ChangeTaskState(p, n); break;
    default: break;
    }
}

// orig 0x001649D0: the current job: the one whose first task is done and last not
void ChooseJob()
{
    JOB* pjobFound = nullptr;
    for (LO* plo : s_psw->mpcidaplo[0x84])
    {
        JOB* p = static_cast<JOB*>(plo);
        if (p->ptaskFirst && FTaskDone(p->ptaskFirst) && p->ptaskLast && !FTaskDone(p->ptaskLast)) pjobFound = p;
    }
    if (pjobFound != s_psw->tasks.pjobCur)
    {
        if (s_psw->tasks.pjobCur) JobEndIfCur(s_psw->tasks.pjobCur);
        if (pjobFound) JobSwitch(pjobFound);
    }
}

// orig 0x00164100 / 0x00165C98: the tasks in order (each after its prerequisites)
void VisitTask(TASK* p, std::vector<TASK*>& a, int nDepth, int nJob)
{
    p->nDepth = std::max(nDepth, p->nDepth);
    p->nJobDepth = std::max(nJob, p->nJobDepth);
    ++p->cVisit;
    if (p->cVisit < static_cast<int>(p->aptaskPrereq.size())) return;
    a.push_back(p);
    ++nDepth;
    if (p->pjob && p->pjob->ptaskLast == p) ++nJob;
    for (TASK* px : p->aptaskNext)
    {
        CSAVE save(px, p->nMode, p->ploContext);
        VisitTask(px, a, nDepth, nJob);
    }
}
std::vector<TASK*> AptaskOrdered()
{
    std::vector<TASK*> a;
    const std::vector<TASK*> aAll = AptaskAll();
    for (TASK* p : aAll) p->cVisit = 0;
    for (TASK* p : aAll)
        if (p->aptaskPrereq.empty()) VisitTask(p, a, 0, 0);
    return a;
}

// orig 0x00164270: the task's state from the save
void RestoreTask(TASK* p)
{
    int n = p->prE0.F() ? NProgress(p->prE0) : 0;
    const int f = p->prE8.F() ? NProgress(p->prE8) : 0;
    if (f && p->pjob && n < 3 && n > 0)
    {
        p->cPlayed = 0;
        n = 1;
        for (TASK* px : p->aptaskPrereq)
            if (px->nState < 2) { n = 0; break; }
    }
    if (!n) n = p->aptaskPrereq.empty() ? 1 : 0;
    SetTaskState(p, n);
}

// --- jobs ---

// orig 0x001628B8: the job becomes the current one
void JobSwitch(JOB* p)
{
    if (FJobCur(p)) return;
    if (JOB* pcur = s_psw->tasks.pjobCur) JobEndIfCur(pcur);
    ActivateJob(p);
}
// orig 0x00162D50: the job begins (its first task reached)
void JobBegin(JOB* p)
{
    if (FJobCur(p)) return;
    BroadcastTaskEvent(s_psw, 0x1C, p);
    // the current player's booty dropped (0x1C4840 / 0x143380, booty.cpp)
    if (ACTOR* pa = PactorFromLo(PloPoCurrent(s_psw))) DropActorBooty(pa);
    JobSwitch(p);
}
void ActivateJob(JOB* p)
{
    SetJobCur(p);
    if (FJobLive(p))
    {
        ++p->cPlayed;
        SaveJob(p);
        BroadcastTaskEvent(s_psw, 0x21, p);
        ApplyTaskObjects(4, p->aobje);
        ApplyTaskSms(4, p->asme, p->nMode);
        Msg(p, 0x13);
    }
}

// orig 0x00162A60 / 0x00162908: the current job ends
void DeactivateJob(JOB* p)
{
    SetJobCur(nullptr);
    if (s_psw->tasks.ptaskChk && s_psw->tasks.ptaskChk->pjob == p) SetTaskChk(nullptr);
    // in the episode's main world: restarts at its point 0x189 again (SW +0x3C88 / +0x3C8C)
    if (const WREC* pw = PwrecEpisodeMain(IepisodeCur()))
        if (g_transition.iwrecCur >= 0 && g_transition.iwrecCur < static_cast<int>(AwrecWorlds().size()) &&
            &AwrecWorlds()[g_transition.iwrecCur] == pw)
        {
            g_transition.oidPointCur = 0x189;
            g_transition.oidKeyCur = -1;
        }
    if (FJobLive(p))
    {
        BroadcastTaskEvent(s_psw, 0x22, p);
        ApplyTaskObjects(5, p->aobje);
        ApplyTaskSms(5, p->asme, p->nMode);
        Msg(p, 0x14);
    }
}
void JobEndIfCur(JOB* p)
{
    if (FJobCur(p)) DeactivateJob(p);
}

// orig 0x00162940: the job's tasks and counts reset
void ResetJob(JOB* p)
{
    for (TASK* pt : AptaskAll())
        if (pt->pjob == p)
        {
            pt->cPlayed = 0;
            SaveTask(pt);
        }
    p->cPlayed = 0;
    SaveJob(p);
}

// orig 0x00162EA0 (method 1250's job reset, the restart): the job over again
void RestartJob(JOB* p)
{
    ResetJob(p);
    if (p->ptaskFirst) ChangeTaskState(p->ptaskFirst, 1);
    if (s_psw->tasks.ptaskCur && s_psw->tasks.ptaskCur->pjob == p) DeactivateTaskCur(s_psw->tasks.ptaskCur);
    BroadcastTaskEvent(s_psw, 0x1F, p);
    JobEndIfCur(p);
}

void RestartTask(TASK* p);

// orig 0x00162DD0 / 0x00162DF8 / 0x00162E38
void JobEvent1D(JOB* p) { BroadcastTaskEvent(s_psw, 0x1D, p); }
void JobRestartAtChk(JOB* p)
{
    if (s_psw->tasks.ptaskChk) RestartTask(s_psw->tasks.ptaskChk);
    else if (p->ptaskFirst) RestartTask(p->ptaskFirst);
}
void JobEnd(JOB* p)
{
    s_psw->tasks.pjobEnded = p;
    if (FReplayJobEnd(s_psw, p, false)) return;   // (QoL Replay Jobs: back to the episode menu)
    // orig: back to the episode's main world at its point 0x189 (unless SW +0x3C84)
    if (!FTransitionBusy() && !s_psw->ploExitPending)
        if (const WREC* pw = PwrecEpisodeMain(IepisodeCur()))
            RequestTransition(s_psw, pw->abRecord, 0x189, -1, 0, -1);
}

// orig 0x00162F60 / 0x00162FD0 / 0x00162F10: the job complete (the last task done)
void JobCompleteClose(JOB* p)
{
    if (!p->f78 && p->ptaskLast) ChangeTaskState(p->ptaskLast, 3);
    BroadcastTaskEvent(s_psw, 0x20, p);
    JobEndIfCur(p);
    if (!p->f78) HudAutoSave(false);   // 0x1ABD98
}
void JobCompleteNext(JOB* p)
{
    if (FReplayJobEnd(s_psw, p, true)) return;   // (QoL Replay Jobs: not the story's next task: back to the episode menu)
    // TODO(M4): 0x1A4E70 / 0x1A4E80 (the game's pending state)
    if (p->ptaskNext) RequestTaskState(p->ptaskNext, 4, p);
    else if (LO* pexit = p->ploCC)
    {
        // orig 0x162FD0: the job's exit (option 1259; e.g. the episode's last job: its briefing movie, then the
        // safe house): updated on the real clock (+0x40 |= 0x100: its update entry's flags) and fired (0x2283D0)
        (pexit->iupd >= 0 ? s_psw->aupd[static_cast<size_t>(pexit->iupd)].grf : pexit->grfUpd) |= 0x100;
        TriggerExit(pexit);
    }
}
void SetJobSeq(JOB* p, int n);
void JobComplete(JOB* p)
{
    if (std::getenv("SLY2N_TASKDEBUG")) std::printf("job %d: complete (t %.3f, sprite %d)\n", p->oid, g_clock.t, p->ploB8 ? p->ploB8->oid : -1);
    if (p->ploB8)
    {
        SetJobSeq(p, 1);
        return;
    }
    JobCompleteClose(p);
    JobCompleteNext(p);
}

// orig 0x00163170: the closing sequence's state (0 none, 1 the job's sprite, 2 the next one, 3 the wipe)
void SetJobSeq(JOB* p, int n)
{
    if (p->nSeq == n) return;
    if (p->nSeq == 0)
    {
        p->ptaskNext = nullptr;
        if (p->ptaskLast)
            for (TASK* px : p->ptaskLast->aptaskNext)
                if (px->ploC4) { p->ptaskNext = px; break; }
        if (p->ptaskNext)
            for (TASK* px : p->ptaskNext->aptaskPrereq)
                if (px != p->ptaskLast && !FTaskDone(px)) { p->ptaskNext = nullptr; break; }
        SetLoSt(s_psw, p, 1);
        uint32_t& grf = p->iupd >= 0 ? s_psw->aupd[static_cast<size_t>(p->iupd)].grf : p->grfUpd;
        grf |= 0x102;  // orig 0x163170: update the live entry on the real clock
        PauseGame(2);  // 0x20EFC0
    }
    if (std::getenv("SLY2N_TASKDEBUG")) std::printf("job %d: closing sequence %d -> %d (t %.3f, real %.3f)\n", p->oid, p->nSeq, n, g_clock.t, g_clock.t3C);
    p->nSeq = n;
    p->tSeq = g_clock.t;
    switch (n)
    {
    case 0:
        if (p->ploBC) HideScreenObj(p->ploBC);  // vt slot 50
        if (p->ploB8) HideScreenObj(p->ploB8);
        SetLoSt(s_psw, p, 2);
        (p->iupd >= 0 ? s_psw->aupd[static_cast<size_t>(p->iupd)].grf : p->grfUpd) &= ~0x102u;
        ResumeGame(2);  // 0x20F030
        break;
    case 1:
        if (p->ploB8)
        {
            ShowScreenObj(p->ploB8, -1);           // vt slot 49
            SelectScreenObjPart(p->ploB8, p->nB0);  // 0x213B00
        }
        break;
    case 2:
        if (p->ploB8) SelectScreenObjPart(p->ploB8, p->nB4);
        break;
    case 3:
        if (p->ploBC) ShowScreenObj(p->ploBC, -1);
        break;
    default: break;
    }
}

// orig 0x00162CD0: a task of the job reached a done state
void JobTaskReached(JOB* pjob, TASK* ptask)
{
    if (!FJobLive(pjob)) return;
    if (ptask == pjob->ptaskFirst) JobBegin(pjob);
    if (ptask == pjob->ptaskLast) JobComplete(pjob);
    if (ptask->idChk != -1) SetTaskChk(ptask);
}

// orig 0x00162BB8: a broadcast event in the current job
void JobOnEvent(JOB* p, int n, LO* plo)
{
    if (FJobLive(p))
    {
        Msg(p, 0x11, {RefS32(n), RefLo(plo)});
        if (std::find(p->anEnd1.begin(), p->anEnd1.end(), n) != p->anEnd1.end()) JobEvent1D(p);
        if (std::find(p->anEnd2.begin(), p->anEnd2.end(), n) != p->anEnd2.end()) JobEnd(p);
    }
    else if (n == 0x10 && p->fMission && !FTransitionBusy()) JobRestartAtChk(p);
}

// orig 0x001D8B20 with "oidPoCurrent" (0x2A5238): the task's player character (+0xC0) into the progress
void SetProgressPoCurrent(OID oid)
{
    if (std::getenv("SLY2_HUDDBG")) std::printf("task: oidPoCurrent 0x%X\n", oid);
    if (oid == -1 || std::getenv("SLY2_NOPOCUR")) return;
    CRef ref;
    ref.SetS32(oid);
    FSetProgress({"oidPoCurrent"}, ref);
}

// orig 0x00165538: the task again from its checkpoint (its world loaded, at +0xB8)
void RestartTask(TASK* p)
{
    CSAVE save(p, 2, nullptr);
    CSAVE saveJob(p->pjob, 2, nullptr);
    if (p->pjob != s_psw->tasks.pjobCur)
    {
        if (JOB* pcur = s_psw->tasks.pjobCur)
        {
            CSAVE saveCur(pcur, 2, nullptr);
            RestartJob(pcur);
        }
        if (p->pjob) JobSwitch(p->pjob);
    }
    ChangeTaskState(p, 1);
    ActivateTask(p);
    SetProgressPoCurrent(p->oidPo);
    const OID oidPoint = p->idChk == -1 ? 0x189 : p->idChk;
    const auto& awrec = AwrecWorlds();
    const uint8_t* pchk = p->iwrec >= 0 && p->iwrec < static_cast<int>(awrec.size()) ? awrec[p->iwrec].abRecord : nullptr;
    RequestTransition(s_psw, pchk, oidPoint, -1, 4, 0x30A);
}

// orig 0x00165280 (broadcast 4 with the request's flag 8): the queued task started in the new world
void StartQueuedTask()
{
    if (!(s_psw->tasks.grfReq & 8)) return;
    const std::vector<TASK*> a = AptaskOrdered();
    for (TASK* p : a)
    {
        CSAVE save(p, 3, nullptr);
        RestoreTask(p);
    }
    TASK* px = PtaskFind(g_taskmgr.oidQueued);
    if (px)
    {
        g_taskmgr.oidJob = px->pjob ? px->pjob->oid : -1;
        g_taskmgr.oidTask = px->oid;
        CSAVE save(px, 3, nullptr);
        ChangeTaskState(px, 1);
        SetProgressPoCurrent(px->oidPo);
    }
    for (TASK* p : a)
    {
        CSAVE save(p, 3, nullptr);
        SetTaskState(p, -1);
    }
    if (px) s_psw->tasks.oidPointNext = px->idChk;
    s_psw->tasks.grfReq = (s_psw->tasks.grfReq & ~8) | 4;
}

// orig 0x00161A58: the task manager's own events
void TaskMgrOnEvent(int n)
{
    switch (n)
    {
    case 4: StartQueuedTask(); break;
    case 5:
    {
        // the world's tasks from the save, the current job / task / checkpoint task again, then saved
        const std::vector<TASK*> a = AptaskOrdered();
        for (TASK* p : a)
        {
            CSAVE save(p, 1, nullptr);
            RestoreTask(p);
        }
        SetJobCur(PjobFromLo(PloFind(g_taskmgr.oidJob)));
        SetTaskCur(PtaskFind(g_taskmgr.oidTask));
        SetTaskChk(PtaskFind(g_taskmgr.oidQueued));
        if (s_psw->tasks.pjobCur) ActivateJob(s_psw->tasks.pjobCur);
        if (s_psw->tasks.ptaskCur) ActivateTaskNow(s_psw->tasks.ptaskCur);
        if (s_psw->tasks.grfReq & 4)
        {
            if (s_psw->tasks.ptaskChk) BroadcastTaskEvent(s_psw, 0x1E, s_psw->tasks.ptaskChk->pjob);  // 0x1657A8
            else if (s_psw->tasks.pjobCur) BroadcastTaskEvent(s_psw, 0x1E, s_psw->tasks.pjobCur->ptaskFirst
                                                                               ? s_psw->tasks.pjobCur->ptaskFirst->pjob
                                                                               : nullptr);
        }
        for (TASK* p : a)
        {
            CSAVE save(p, 0, nullptr);
            SaveTask(p);
        }
        if (std::getenv("SLY2N_TASKDEBUG"))
        {
            std::fprintf(stderr, "tasks: %zu ordered of %zu; job %d task %d chk %d\n", a.size(), AptaskAll().size(),
                         g_taskmgr.oidJob, g_taskmgr.oidTask, g_taskmgr.oidQueued);
            for (TASK* p : AptaskAll())
                std::fprintf(stderr, "  task %d state %d job %d prereq %zu next %zu objects %zu sms %zu mode %d prE0 %zu %s\n",
                             p->oid, p->nState, p->pjob ? p->pjob->oid : -1, p->aptaskPrereq.size(), p->aptaskNext.size(),
                             p->aobje.size(), p->asme.size(), p->nMode, p->prE0.akey.size(), StrProgressKey(p->prE0.akey).c_str());
            if (std::getenv("SLY2N_TASKDEBUG")[0] == '2')  // (the tasks' prerequisites)
                for (TASK* p : AptaskAll())
                {
                    std::string str;
                    for (TASK* px : p->aptaskPrereq) str += " " + std::to_string(px->oid) + "(" + std::to_string(px->nState) + ")";
                    std::fprintf(stderr, "    task %d prereqs%s\n", p->oid, str.c_str());
                }
            if (std::getenv("SLY2N_TASKDEBUG")[0] == '2')  // (the tasks' objects: oid, owner, flags, and where they are)
                for (TASK* p : AptaskAll())
                    for (const TASKOBJE& e : p->aobje)
                    {
                        LO* po = PloFindSwAny(e.oidA);
                        const glm::vec3 pos = po ? PosAi(po) : glm::vec3(0.0f);
                        std::fprintf(stderr, "    task %d object %d owner %d grf 0x%X cid 0x%X at %.0f %.0f %.0f\n", p->oid, e.oidA, e.oidB, e.grf,
                                     po ? po->Cid() : 0, pos.x, pos.y, pos.z);
            if (std::getenv("SLY2N_TASKDEBUG")[0] == '2')  // (each task's prerequisites and successors)
                for (TASK* p : AptaskAll())
                {
                    std::fprintf(stderr, "    task %d prereqs", p->oid);
                    for (TASK* px : p->aptaskPrereq) std::fprintf(stderr, " %d", px->oid);
                    std::fprintf(stderr, " next");
                    for (TASK* px : p->aptaskNext) std::fprintf(stderr, " %d", px->oid);
                    std::fprintf(stderr, "\n");
                }
                    }
        }
        break;
    }
    case 16:
        // the player lost: the current world again at the last checkpoint when no job runs
        if (!s_psw->tasks.pjobCur)
            RequestTransition(s_psw, nullptr, g_transition.oidPointCur, g_transition.oidKeyCur, 0, 0x30A);
        break;
    case 3: OnGameRecordEvent(g_transition.req.flags); break;   // (the slots' read / the settings: frontend.cpp)
    default: break;  // TODO(M4): 1 (the hideout's HUD)
    }
}

// --- the method bodies (objmethod.h): the thunks 0x10E910.. / 0x10EE60.. ---
CRef RefVoid() { return CRef(TAGK_Void); }
TASKOBJE ObjeFrom(int oidA, uint32_t grf, int oidB, int iwrec)
{
    TASKOBJE e;
    e.oidA = oidA;
    e.oidB = oidB;
    e.grf = grf;
    e.iwrec = iwrec;
    return e;
}
uint32_t GrfFromList(const std::vector<int>& an)
{
    uint32_t grf = 0;
    for (int n : an) grf |= GrfState(n);
    return grf;
}

struct INITTASKMETHODS
{
    INITTASKMETHODS()
    {
        // tasks
        RegisterObjectMethod(0x10EE60, [](LO* plo, const std::vector<CRef>& a) {  // 1280: 0x164640
            if (TASK* p = PtaskFromLo(plo)) RequestTaskState(p, NArg(a, 0, -1), PloArg(a, 1));
            return RefVoid();
        });
        RegisterObjectMethod(0x10EF08, [](LO* plo, const std::vector<CRef>& a) {  // 1281: 0x164858
            if (TASK* p = PtaskFromLo(plo)) ForceTaskState(p, NArg(a, 0, -1), PloArg(a, 1));
            return RefVoid();
        });
        RegisterObjectMethod(0x10EFB0, [](LO* plo, const std::vector<CRef>& a) {  // 1284: 0x165118
            if (TASK* p = PtaskFromLo(plo); p && FTaskLive(p)) Msg(p, 0x17, {RefS32(NArg(a, 0))});
            return RefVoid();
        });
        RegisterObjectMethod(0x10F030, [](LO* plo, const std::vector<CRef>& a) {  // 1287: 0x163838
            TASK* p = PtaskFromLo(plo);
            if (p && !p->fMission && p->aobje.size() < 32)
                p->aobje.push_back(ObjeFrom(NArg(a, 0, -1), GrfFromList(AnArgList(a, 1)), NArg(a, 2, -1), NArg(a, 3, -1)));
            return RefVoid();
        });
        RegisterObjectMethod(0x10F118, [](LO* plo, const std::vector<CRef>& a) {  // 1288: 0x1638A0
            TASK* p = PtaskFromLo(plo);
            if (p && !p->fMission && p->asme.size() < 32)
                p->asme.push_back(TASKSME{NArg(a, 0, -1), NArg(a, 1, -1), NArg(a, 2), NArg(a, 3, -1), NArg(a, 4), NArg(a, 5, -1)});
            return RefVoid();
        });
        static const uint32_t s_apfnForce[4] = {0x10F208, 0x10F280, 0x10F2F8, 0x10F370};  // 1290..1293
        RegisterObjectMethod(s_apfnForce[0], [](LO* plo, const std::vector<CRef>&) {
            if (TASK* p = PtaskFromLo(plo)) { CSAVE save(p, 4, nullptr); ForceTaskState(p, 0, nullptr); ChooseJob(); }
            return RefVoid();
        });
        RegisterObjectMethod(s_apfnForce[1], [](LO* plo, const std::vector<CRef>&) {
            if (TASK* p = PtaskFromLo(plo)) { CSAVE save(p, 4, nullptr); ForceTaskState(p, 1, nullptr); ChooseJob(); }
            return RefVoid();
        });
        RegisterObjectMethod(s_apfnForce[2], [](LO* plo, const std::vector<CRef>&) {
            if (TASK* p = PtaskFromLo(plo)) { CSAVE save(p, 4, nullptr); ForceTaskState(p, 2, nullptr); ChooseJob(); }
            return RefVoid();
        });
        RegisterObjectMethod(s_apfnForce[3], [](LO* plo, const std::vector<CRef>&) {
            if (TASK* p = PtaskFromLo(plo)) { CSAVE save(p, 4, nullptr); ForceTaskState(p, 3, nullptr); ChooseJob(); }
            return RefVoid();
        });
        RegisterObjectMethod(0x10F3E8, [](LO* plo, const std::vector<CRef>&) {  // 1294: 0x163C28
            if (TASK* p = PtaskFromLo(plo)) ActivateTask(p);
            return RefVoid();
        });
        // a task's job (option 1093: EOPID 1264, the field +0x7C)
        RegisterObjectGetter(1264, [](LO* plo) {
            TASK* p = PtaskFromLo(plo);
            return RefLo(p ? p->pjob : nullptr);  // (an object reference, null when none: null-obj?)
        });
        // the change's mode and context saved around calls (CSAVE): a task's 1282 / 1283 (+0xF4 / +0xF8), a job's
        // 1253 / 1254 (+0xF0 / +0xF4); the job's proc 1829 in world 9 asks for state 2 only when the context is null
        RegisterObjectGetter(1282, [](LO* plo) { CRef r; if (TASK* p = PtaskFromLo(plo)) r.SetS32(p->nMode); return r; });
        RegisterObjectGetter(1283, [](LO* plo) { TASK* p = PtaskFromLo(plo); return RefLo(p ? p->ploContext : nullptr); });
        RegisterObjectGetter(1253, [](LO* plo) { CRef r; if (JOB* p = PjobFromLo(plo)) r.SetS32(p->nMode); return r; });
        RegisterObjectGetter(1254, [](LO* plo) { JOB* p = PjobFromLo(plo); return RefLo(p ? p->ploContext : nullptr); });
        // jobs
        RegisterObjectMethod(0x10E910, [](LO* plo, const std::vector<CRef>& a) {  // 1236: 0x162600
            if (JOB* p = PjobFromLo(plo); p && p->anEnd1.size() < 32) p->anEnd1.push_back(NArg(a, 0));
            return RefVoid();
        });
        RegisterObjectMethod(0x10E998, [](LO* plo, const std::vector<CRef>& a) {  // 1237: 0x162630
            if (JOB* p = PjobFromLo(plo); p && p->anEnd2.size() < 32) p->anEnd2.push_back(NArg(a, 0));
            return RefVoid();
        });
        RegisterObjectMethod(0x10EA20, [](LO* plo, const std::vector<CRef>&) {  // 1238: 0x162DD0
            if (JOB* p = PjobFromLo(plo)) JobEvent1D(p);
            return RefVoid();
        });
        RegisterObjectMethod(0x10EA98, [](LO* plo, const std::vector<CRef>&) {  // 1239: 0x162DF8
            if (JOB* p = PjobFromLo(plo)) JobRestartAtChk(p);
            return RefVoid();
        });
        RegisterObjectMethod(0x10EB10, [](LO* plo, const std::vector<CRef>&) {  // 1240: 0x162E38
            if (JOB* p = PjobFromLo(plo)) JobEnd(p);
            return RefVoid();
        });
        RegisterObjectMethod(0x10EB88, [](LO* plo, const std::vector<CRef>&) {  // 1250: 0x162940
            if (JOB* p = PjobFromLo(plo)) ResetJob(p);
            return RefVoid();
        });
        RegisterObjectMethod(0x10EC00, [](LO* plo, const std::vector<CRef>& a) {  // 1255: 0x1626C0
            JOB* p = PjobFromLo(plo);
            if (p && !p->fMission && p->aobje.size() < 32)
                p->aobje.push_back(ObjeFrom(NArg(a, 0, -1), GrfState(NArg(a, 1, -1)), NArg(a, 2, -1), NArg(a, 3, -1)));
            return RefVoid();
        });
        RegisterObjectMethod(0x10ECC8, [](LO* plo, const std::vector<CRef>& a) {  // 1256: 0x162728
            JOB* p = PjobFromLo(plo);
            if (p && !p->fMission && p->asme.size() < 32)
                p->asme.push_back(TASKSME{NArg(a, 0, -1), NArg(a, 1, -1), NArg(a, 2), NArg(a, 3, -1), 0, NArg(a, 4, -1)});
            return RefVoid();
        });
        RegisterObjectMethod(0x10EDA8, [](LO* plo, const std::vector<CRef>& a) {  // 1258: 0x162660
            if (JOB* p = PjobFromLo(plo))
            {
                p->nAC = NArg(a, 0, -1);
                p->nB0 = NArg(a, 1, -1);
                p->nB4 = NArg(a, 2, -1);
            }
            return RefVoid();
        });
        // getters: 1286 the state (+0x54), 1268 done (0x164030), 1269 current (0x164018), 1270 the state's id
        // (0x164098); the job's 1251 (0x163400: its last task done), 1252 current (0x1633E8)
        RegisterObjectGetter(1286, [](LO* plo) { CRef r; if (TASK* p = PtaskFromLo(plo)) r.SetS32(p->nState); return r; });
        // 1267: the times played (+0x64; media code: the boot world's task 1583 starts its briefing on the first)
        RegisterObjectGetter(1267, [](LO* plo) { CRef r; if (TASK* p = PtaskFromLo(plo)) r.SetS32(p->cPlayed); return r; });
        RegisterObjectGetter(1268, [](LO* plo) { CRef r; if (TASK* p = PtaskFromLo(plo)) r.SetBool(FTaskDone(p)); return r; });
        RegisterObjectGetter(1269, [](LO* plo) { CRef r; if (TASK* p = PtaskFromLo(plo)) r.SetBool(FTaskCur(p)); return r; });
        RegisterObjectGetter(1270, [](LO* plo) {
            CRef r;
            if (TASK* p = PtaskFromLo(plo)) r.SetS32(p->nState >= 0 && p->nState <= 3 ? 0x612 + p->nState : -1);
            return r;
        });
        RegisterObjectGetter(1251, [](LO* plo) {
            CRef r;
            if (JOB* p = PjobFromLo(plo)) r.SetBool(p->ptaskLast && FTaskDone(p->ptaskLast));
            return r;
        });
        RegisterObjectGetter(1252, [](LO* plo) { CRef r; if (JOB* p = PjobFromLo(plo)) r.SetBool(FJobCur(p)); return r; });
    }
} s_inittaskmethods;

// The field options (grf 0x80 / 0x90) at their offsets, the progress references (kind 22) by EOPID
const PROGREF PrFromOption(const LO* plo, int eopid)
{
    PROGREF pr;
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) pr.akey = o.aval[0].an;
    return pr;
}
int NField(const LO* plo, uint32_t ib, int nDefault) { return static_cast<int>(plo->U32Field(0, ib, static_cast<uint32_t>(nDefault))); }
float GField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t u = 0;
    std::memcpy(&u, &gDefault, 4);
    u = plo->U32Field(0, ib, u);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// orig 0x00163B48 (TASK vt slot 16): the time limit (message 0x17 when it runs out), the object shown while the
// task is available
void UpdateTask(LO* plo)
{
    TASK* p = static_cast<TASK*>(plo);
    if (p->tNextUpdate <= g_clock.t) HandleLoSpliceEvent(p, 1, true, 0, nullptr);  // 0x1A5E28
    if (p->nState == 1 && p->dtLimit > 0.0f)
    {
        const float dt = g_clock.t - p->tState;
        if (dt < p->dtLimit && p->dtLimit + g_clock.dt <= dt && FTaskLive(p)) Msg(p, 0x17, {RefS32(0)});
    }
    if (p->ploShown)
    {
        if (std::getenv("SLY2N_TASKMARKERS") && p->nState == 1 && !p->ploShown->fInWorld)
        {
            const glm::vec3 pos = PosAi(p->ploShown);
            std::printf("taskmarker %d object %d at %.3f %.3f %.3f checkpoint %d world %d hero %d\n",
                        p->oid, p->ploShown->oid, pos.x, pos.y, pos.z, p->idChk, p->iwrec, p->oidPo);
        }
        if (p->nState == 1 && !s_psw->tasks.pjobCur) AddLo(s_psw, p->ploShown);
        else RemoveLo(s_psw, p->ploShown);
    }
}

// orig 0x00163078 (JOB vt slot 16): the closing sequence (TODO(M4): the sprites' animations end it at once)
void UpdateJob(LO* plo)
{
    JOB* p = static_cast<JOB*>(plo);
    if (p->tNextUpdate <= g_clock.t) HandleLoSpliceEvent(p, 1, true, 0, nullptr);
    if (static const bool s_f = std::getenv("SLY2N_JOBSEQPRINT") != nullptr; s_f && p->nSeq)
        std::printf("jobseq %d: seq %d t %.3f b8 %d busy %d bc %d next %d cc %d\n", p->oid, p->nSeq, g_clock.t, p->ploB8 ? p->ploB8->oid : -1,
                    p->ploB8 ? int(FScreenObjBusy(p->ploB8, p->nSeq == 1 ? p->nB0 : p->nB4)) : -1, p->ploBC ? p->ploBC->oid : -1,
                    p->ptaskNext ? p->ptaskNext->oid : -1, p->ploCC ? p->ploCC->oid : -1);
    if (std::getenv("SLY2N_JOBSEQPRINT") && p->nSeq == 3 && p->ploBC) ::PrintScreenObjState(p->ploBC);
    // each step once its sprite's part has played (0x2121E8: the group of the part busy)
    switch (p->nSeq)
    {
    case 1:
        if (p->ploB8 && FScreenObjBusy(p->ploB8, p->nB0)) break;
        if (std::getenv("SLY2N_TASKDEBUG"))
            std::printf("job %d: sprite %d part 0x%X not busy (t %.3f)\n", p->oid, p->ploB8 ? p->ploB8->oid : -1, p->nB0, g_clock.t);
        JobCompleteClose(p);  // 0x162F60
        SetJobSeq(p, (p->ptaskNext || p->ploCC) ? 3 : 2);
        break;
    case 2:
        if (p->ploB8 && FScreenObjBusy(p->ploB8, p->nB4)) break;
        JobCompleteNext(p);  // 0x162FD0
        SetJobSeq(p, 0);
        break;
    case 3:
        if (p->ploBC && FScreenObjBusy(p->ploBC, 783)) break;
        JobCompleteNext(p);
        break;
    default: break;
    }
}

struct INITTASKUPDATES
{
    INITTASKUPDATES()
    {
        RegisterLoUpdate(0x163B48, UpdateTask);
        RegisterLoUpdate(0x163078, UpdateJob);
        // (debug: SLY2N_JOBCOMPLETE=job@frame completes that job's last task at that update: its closing sequence)
        RegisterSwPass(0x1F77C0, [](SW* psw) {
            static int s_c = 0;
            const char* sz = std::getenv("SLY2N_JOBCOMPLETE");
            if (sz && !std::strcmp(sz, "list") && s_c++ == 2)
                for (LO* plo : psw->mpcidaplo[0x84])
                    if (JOB* p = PjobFromLo(plo))
                        std::printf("job %d: last %d exit %d\n", p->oid, p->ptaskLast ? p->ptaskLast->oid : -1, p->ploCC ? p->ploCC->oid : -1);
            // (debug: SLY2N_TASKSET="task:state@frame[,task:state@frame...]" requests those task states at those updates,
            // as a script would)
            static int s_cSet = 0;
            if (const char* szSet = std::getenv("SLY2N_TASKSET"))
            {
                ++s_cSet;
                for (const char* q = szSet; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                {
                    const char* c = std::strchr(q, ':');
                    const char* a = std::strchr(q, '@');
                    if (!c || !a || s_cSet != std::atoi(a + 1)) continue;
                    if (LO* ptask = PloFindSwObjectByOid(psw, std::atoi(q)))
                    {
                        s_psw = psw;
                        RequestLoTaskState(ptask, std::atoi(c + 1), nullptr);
                    }
                }
            }
            if (!sz || !std::strchr(sz, '@') || ++s_c != std::atoi(std::strchr(sz, '@') + 1)) return;
            if (JOB* p = PjobFromLo(PloFindSwObjectByOid(psw, std::atoi(sz))))
            {
                std::printf("job %d: forced complete (next %d, exit %d)\n", p->oid, p->ptaskNext ? p->ptaskNext->oid : -1,
                            p->ploCC ? p->ploCC->oid : -1);
                s_psw = psw;
                if (p->ptaskLast) ChangeTaskState(p->ptaskLast, 2);  // (0x162CD0: the job's last task reached)
                else JobComplete(p);
            }
        });
    }
} s_inittaskupdates;
}

void InitTasks(SW* psw)
{
    s_psw = psw;
    psw->tasks.grfReq = g_transition.req.flags;
    psw->tasks.oidPointNext = -1;
    // the fields of the options (grf 0x80/0x90 at the original offsets)
    for (LO* plo : psw->mpcidaplo[0x85])
    {
        TASK* p = static_cast<TASK*>(plo);
        p->idDialog5C = NField(p, 0x5C, -1);
        p->f60 = NField(p, 0x60, 0);
        p->f6c = NField(p, 0x6C, 0);
        p->f70 = NField(p, 0x70, 0);
        p->f74 = NField(p, 0x74, 0);
        p->dtLimit = GField(p, 0x78, 0.0f);
        p->pjob = PjobFromLo(PloFind(NField(p, 0x7C, -1)));
        p->ploShown = PloFind(NField(p, 0x80, -1));
        p->idChk = NField(p, 0xB8, -1);
        p->iwrec = NField(p, 0xBC, -1);
        p->oidPo = NField(p, 0xC0, -1);
        if (std::getenv("SLY2N_TASKMARKERS") && p->ploShown)
        {
            const glm::vec3 pos = PosAi(p->ploShown);
            std::printf("taskentry %d object %d at %.3f %.3f %.3f checkpoint %d world %d hero %d\n",
                        p->oid, p->ploShown->oid, pos.x, pos.y, pos.z, p->idChk, p->iwrec, p->oidPo);
        }
        p->ploC4 = PloFind(NField(p, 0xC4, -1));
        p->nMode = NField(p, 0xF4, 0);
        p->ploContext = PloFind(NField(p, 0xF8, -1));
        p->prC8 = PrFromOption(p, 1295);
        p->prD0 = PrFromOption(p, 1296);
        p->prD8 = PrFromOption(p, 1297);
        p->prE0 = PrFromOption(p, 1298);
        p->prE8 = PrFromOption(p, 1299);
    }
    for (LO* plo : psw->mpcidaplo[0x84])
    {
        JOB* p = static_cast<JOB*>(plo);
        p->f74 = NField(p, 0x74, 0);
        p->f78 = NField(p, 0x78, 0);
        p->ptaskFirst = PtaskFind(NField(p, 0x64, -1));
        p->ptaskLast = PtaskFind(NField(p, 0x68, -1));
        p->n6c = NField(p, 0x6C, -1);
        p->n70 = NField(p, 0x70, -1);
        p->cPlayed = NField(p, 0x7C, 0);
        p->nMode = NField(p, 0xF0, 0);
        p->ploContext = PloFind(NField(p, 0xF4, -1));
        p->ploCC = PloFind(NField(p, 0xCC, -1));
        p->prD0 = PrFromOption(p, 1262);
        p->prD8 = PrFromOption(p, 1263);
        p->prE0 = PrFromOption(p, 1260);
        p->prE8 = PrFromOption(p, 1261);
    }
}

// orig 0x00162780 (JOB vt slot 15)
void StartJob(SW* psw, JOB* p)
{
    s_psw = psw;
    if (!FJobCur(p) && FJobLive(p))
    {
        ApplyTaskObjects(5, p->aobje);
        ApplyTaskSms(5, p->asme, 1);
    }
    if (p->prD0.F()) p->cPlayed = NProgress(p->prD0);
    if (p->prE0.F()) p->gA4 = GProgress(p->prE0);
    // TODO(M4): +0xA8 = 0x1D7E80 (the progress block of +0xE8)
    if (p->nAC != -1) p->ploB8 = PloFind(p->nAC);
    p->ploBC = PloFind(0x379);
}

// orig 0x001638F8 (TASK vt slot 15)
void StartTask(SW* psw, TASK* p)
{
    s_psw = psw;
    p->aptaskPrereq.clear();
    for (int32_t oid : p->an90)
        if (TASK* pt = PtaskFind(oid)) p->aptaskPrereq.push_back(pt);
    p->aptaskNext.clear();
    for (int32_t oid : p->an9C)
        if (TASK* pt = PtaskFind(oid)) p->aptaskNext.push_back(pt);
    if (!FTaskCur(p) && FTaskLive(p))
    {
        ApplyTaskObjects(5, p->aobje);
        ApplyTaskSms(5, p->asme, 1);
    }
    if (p->prC8.F()) p->cPlayed = NProgress(p->prC8);
    if (p->prD0.F()) p->cDone = NProgress(p->prD0);
    if (p->dtLimit != 0.0f || p->ploShown)
    {
        if (p->st == 2) SetLoSt(psw, p, 0);
        if (p->iupd >= 0) psw->aupd[p->iupd].grf |= GRFUPD_Update;
        else p->grfUpd |= GRFUPD_Update;
    }
}

// ---- The suck (D_002DE578: the game's dynamic difficulty, 0 .. 1): the current job's "<job>.suck_value" (its
// +0xA8, the progress +0xE8), else the mean of "suck_history.mission_0..2". Damage to the players is scaled by
// 1 - g / 2 (0x155228), the drops' counters lean on it (0x154E10).
namespace
{
CRef RefSuckHistory(int i)
{
    static const char* s_asz[3] = {"mission_0", "mission_1", "mission_2"};
    return RefGetProgress({"suck_history", s_asz[i]});
}
void SetSuckHistory(int i, const CRef& r)
{
    static const char* s_asz[3] = {"mission_0", "mission_1", "mission_2"};
    FSetProgress({"suck_history", s_asz[i]}, r);
}
float GOfRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? static_cast<float>(r.m_n) : 0.0f; }
// orig 0x00154DC8 (the current job's value, limited to [0, 1] by D_002BDC00 +0x0 / +0x4)
void SetSuck(float g)
{
    JOB* pj = s_psw ? s_psw->tasks.pjobCur : nullptr;
    if (!pj || !pj->prE8.F()) return;
    CRef r;
    r.SetF32(std::clamp(g, 0.0f, 1.0f));
    FSetProgressKey(pj->prE8.akey, r);
}
}  // namespace

// orig 0x00154C90
float GSuck()
{
    if (JOB* pj = g_psw ? g_psw->tasks.pjobCur : nullptr)  // (D_003E0F9C)
        return pj->prE8.F() ? GProgress(pj->prE8) : 0.0f;
    float g = 0.0f;
    for (int i = 0; i < 3; ++i) g += GOfRef(RefSuckHistory(i));
    return g / 3.0f;
}

// orig 0x00154A90 (D_002DE578's handler; its message 3 binds the history at load): 0x22 the job ends (the history
// shifts in its value), 0x25 a task of the current job first done (-0.2, D_002BDC00 +0xC), 0x10 / 0x1D a player's
// death / the job's restart (+0.1, +0x8)
void SuckOnEvent(int n, LO* plo)
{
    static const bool s_fPrint = std::getenv("SLY2N_SUCKPRINT") != nullptr;  // (debug: the suck's changes)
    if (s_fPrint && (n == 0x10 || n == 0x1D || n == 0x22 || n == 0x25))
        std::printf("suck: event 0x%X t %.3f value %.3f\n", n, g_clock.t, GSuck());
    switch (n)
    {
    case 0x22:
    {
        SetSuckHistory(0, RefSuckHistory(1));
        SetSuckHistory(1, RefSuckHistory(2));
        JOB* pj = dynamic_cast<JOB*>(plo);
        CRef r;
        r.SetF32(pj && pj->prE8.F() ? GProgress(pj->prE8) : 0.0f);
        SetSuckHistory(2, r);
        break;
    }
    case 0x25:
        if (TASK* pt = dynamic_cast<TASK*>(plo); pt && s_psw && pt->pjob == s_psw->tasks.pjobCur && pt->nState == 2 && pt->cDone == 1)
            SetSuck(GSuck() - 0.2f);
        break;
    case 0x10: case 0x1D: SetSuck(GSuck() + 0.1f); break;
    default: break;
    }
}

// orig 0x001619A0
void BroadcastTaskEvent(SW* psw, int n, LO* plo)
{
    if (!psw) return;
    s_psw = psw;
    TaskMgrOnEvent(n);
    SuckOnEvent(n, plo);  // 0x154A90 (D_002DE578's handler)
    JOB* pjob = psw->tasks.pjobCur;
    if ((std::getenv("SLY2N_TASKDEBUG") && n == 0x3B) || (std::getenv("SLY2N_TASKEVENTPRINT") && n == 0x1A))
    {
        std::printf("task event 0x%X (object %d, job %d): active", n, plo ? plo->oid : -1, pjob ? pjob->oid : -1);
        for (TASK* p : psw->tasks.aptaskActive) std::printf(" %d(job %d live %d)", p->oid, p->pjob ? p->pjob->oid : -1, int(FTaskLive(p)));
        std::printf("\n");
    }
    for (TASK* p : std::vector<TASK*>(psw->tasks.aptaskActive))
        if (p->pjob == pjob && FTaskLive(p)) Msg(p, 0x11, {RefS32(n), RefLo(plo)});  // 0x165170
    if (psw->tasks.pjobCur) JobOnEvent(psw->tasks.pjobCur, n, plo);
}

// orig 0x00161F48: a checkpoint's task states (method 794's entries)
void ApplyCheckpointTasks(SW* psw, LO* pchk, const std::vector<std::pair<OID, int>>& aent)
{
    s_psw = psw;
    for (const auto& [oid, n] : aent)
        if (TASK* p = PtaskFind(oid)) RequestTaskState(p, n, pchk);
}

// orig 0x001651D8
void RequestTaskStartByOid(SW* psw, const uint8_t* pchk, OID oidTask)
{
    s_psw = psw;
    if (TASK* p = psw ? PtaskFind(oidTask) : nullptr)
    {
        SetTaskChk(p);
        RestartTask(p);
        return;
    }
    g_taskmgr.oidJob = -1;
    g_taskmgr.oidTask = -1;
    g_taskmgr.oidQueued = oidTask;
    RequestTransition(psw, pchk, 0x189, -1, 8, -1);
}

// orig 0x00161660
int NTasksPercentDone(SW* psw)
{
    int c = 0, cDone = 0;
    for (LO* plo : psw->mpcidaplo[0x85])
    {
        const TASK* p = static_cast<const TASK*>(plo);
        if (!p->aptaskNext.empty())  // (orig +0xA0)
        {
            ++c;
            if (p->nState == 3) ++cDone;
        }
    }
    if (!cDone) return 0;
    if (cDone == c) return 100;
    return std::max(1, std::min(cDone * 100 / c, 99));
}

void EndWorldTasks(SW* psw)
{
    s_psw = psw;
    BroadcastTaskEvent(psw, 1, nullptr);
    if (JOB* p = psw->tasks.pjobEnded)
    {
        RestartJob(p);
        psw->tasks.pjobEnded = nullptr;
    }
    s_psw = nullptr;
}

void EndJob(SW* psw, JOB* pjob)
{
    s_psw = psw;
    JobEnd(pjob);
}

// orig: 0x00162DF8: restart the current checkpoint, or the job's first task.
void RestartJobAtCheckpoint(SW* psw, JOB* pjob)
{
    s_psw = psw;
    JobRestartAtChk(pjob);
}

// (for the binocucom's goal text, binoc.cpp) 0x164030
bool FTaskDoneExt(TASK* p) { return FTaskDone(p); }

// (for the job help, shop.cpp) the world's tasks in order (0x165C98)
std::vector<TASK*> AptaskInOrder(SW* psw)
{
    if (psw) s_psw = psw;
    return AptaskOrdered();
}

// (for the pickups: a task object asked for the state n, as its method 1280 / 0x164640; pickup.cpp)
void RequestLoTaskState(LO* plo, int n, LO* ploContext)
{
    if (TASK* p = dynamic_cast<TASK*>(plo)) RequestTaskState(p, n, ploContext);
}

// orig: 0x00161478 func_00161478 (its end): SW +0x3B0C's +0xA4 += dt while (float)JOY +0x1020 < 10; past +0x9C the
// suck history's progress (0x154D88 on 0x2DE578 with dt / (+0xA0 - +0x9C): TODO(world), the suck history)
void UpdateJobTimer(float dt)
{
    if (!g_psw) return;
    JOB* p = g_psw->tasks.pjobCur;
    if (!p) return;
    if (!(static_cast<float>(g_joyps2.dtIdle) < 10.0f)) return;   // (0x2E0C20: the first pad's JOY +0x1020)
    p->gA4 = p->gA4 + dt;
}

// (for the task counters, screenrt.cpp 0x211F90) a task by oid: its state +0x54 (-2 none) and done (0x164030)
int NTaskStateOid(OID oid, bool* pfDone)
{
    TASK* p = PtaskFind(oid);
    if (pfDone) *pfDone = p && FTaskDone(p);
    return p ? p->nState : -2;
}

// (debug, SLY2N_TASKREQ in game.cpp: a task's state requested by its oid; false when there is no such task)
bool FRequestTaskStateOid(int oid, int n)
{
    TASK* p = PtaskFind(oid);
    if (!p) return false;
    RequestTaskState(p, n, nullptr);
    return true;
}

// (debug key F9 / SLY2N_COMPLETEJOBAT, game.cpp) the current job completed as if each of its remaining tasks were done:
// its last task forced done through the task manager (method 1281, ForceTaskState: ChangeTaskState collects every
// prerequisite not yet done, in order, so the job's earlier tasks are done first and its successors unlock), then the
// job's own end runs (JobTaskReached: the "Job Completed!" sprite, the closing sequence, the autosave and the next
// task / the exit). Nothing when no job is current, its end has begun or a world change is under way.
std::string StrLocalized(const SW* psw, int id);   // splayer.cpp
bool FDebugCompleteJob()
{
    JOB* pjob = s_psw ? s_psw->tasks.pjobCur : nullptr;
    if (!pjob || !pjob->ptaskLast)
    {
        std::printf("F9 complete job: not in a job (world %d)\n", g_transition.iwrecCur);
        return false;
    }
    if (FTransitionBusy() || pjob->nSeq != 0 || FTaskDone(pjob->ptaskLast))
    {
        std::printf("F9 complete job: job %d is already ending\n", pjob->oid);
        return false;
    }
    int cLeft = 0;
    for (TASK* pt : AptaskAll())
        if (pt->pjob == pjob && pt->nState >= 0 && pt->nState < 2) ++cLeft;
    const TASK* pcur = s_psw->tasks.ptaskCur;
    std::printf("F9 complete job: skipped job %d \"%s\" (episode %d (0 the prologue), world %d, current task %d, last task %d, %d tasks left)\n",
                pjob->oid, pjob->n6c != -1 ? StrLocalized(s_psw, pjob->n6c).c_str() : "?", IepisodeCur(), g_transition.iwrecCur,
                pcur ? pcur->oid : -1, pjob->ptaskLast->oid, cLeft);
    std::fflush(stdout);
    TASK* plast = pjob->ptaskLast;
    ForceTaskState(plast, 2, nullptr);
    // (a last task of another world (filtered out here, +0x50: e.g. Aerial Assault's, played in its own level) is done
    // without its job reacting (ChangeTaskState's JobTaskReached needs it live): the job's own end run here instead, as
    // the task's world would (the sprite, the closing sequence, the autosave and the next task / the exit))
    if (FTaskDone(plast) && !FTaskLive(plast) && pjob->nSeq == 0 && FJobCur(pjob) && FJobLive(pjob) && !FTransitionBusy())
    {
        std::printf("F9 complete job: last task %d is not in this world: job %d's end run here\n", plast->oid, pjob->oid);
        JobComplete(pjob);
    }
    return true;
}

// (QoL Replay Jobs, replay.cpp) the jobs of a world loaded but not started (a temporary SW): the tasks' and jobs'
// fields as InitTasks reads them, the task graph as StartTask links it, the order as the job help's 0x1DCED8
std::string StrLocalized(const SW* psw, int id);   // splayer.cpp
extern std::unordered_map<int, int> g_mpoidiwrecTask;   // brx.cpp / autoplay.cpp: a filtered-out task's first world
std::vector<JOBINFO> AjobinfoOfLoadedWorld(SW* psw)
{
    std::vector<JOBINFO> ajob;
    if (!psw) return ajob;
    SW* const pswSaved = s_psw;
    const TASKSW tasksSaved = psw->tasks;
    InitTasks(psw);   // (s_psw = psw)
    for (TASK* p : AptaskAll())
    {
        p->aptaskPrereq.clear();
        for (int32_t oid : p->an90)
            if (TASK* pt = PtaskFind(oid)) p->aptaskPrereq.push_back(pt);
        p->aptaskNext.clear();
        for (int32_t oid : p->an9C)
            if (TASK* pt = PtaskFind(oid)) p->aptaskNext.push_back(pt);
        p->nDepth = p->nJobDepth = 0;
    }
    std::vector<TASK*> a = AptaskOrdered();
    std::stable_sort(a.begin(), a.end(), [](const TASK* x, const TASK* y) {
        if (x->nJobDepth != y->nJobDepth) return x->nJobDepth < y->nJobDepth;
        const int n1 = x->pjob ? x->pjob->oid : -1, n2 = y->pjob ? y->pjob->oid : -1;
        if (n1 != n2) return n1 < n2;
        if (x->nDepth != y->nDepth) return x->nDepth < y->nDepth;
        return x->oid < y->oid;
    });
    for (TASK* pt : a)
    {
        JOB* pj = pt->pjob;
        if (!pj || pj->n6c == -1 || !pj->ptaskFirst) continue;
        auto it = std::find_if(ajob.begin(), ajob.end(), [&](const JOBINFO& j) { return j.oidJob == pj->oid; });
        if (it == ajob.end())
        {
            JOBINFO j;
            j.oidJob = pj->oid;
            j.idName = pj->n6c;
            j.idDesc = pj->n70;
            j.strName = StrLocalized(psw, pj->n6c);
            j.strDesc = pj->n70 != -1 ? StrLocalized(psw, pj->n70) : std::string();
            const TASK* pf = pj->ptaskFirst;
            j.oidTaskFirst = pf->oid;
            j.oidTaskLast = pj->ptaskLast ? pj->ptaskLast->oid : -1;
            if (pj->ptaskLast)
                for (TASK* px : pj->ptaskLast->aptaskNext)
                    if (px->pjob && px->pjob != pj) { j.oidTaskNext = px->oid; break; }
            j.akeyFirst = pf->prE0.akey;
            if (pj->ptaskLast) j.akeyLast = pj->ptaskLast->prE0.akey;
            j.iwrecFirst = pf->iwrec;
            if (j.iwrecFirst < 0)
                if (auto itw = g_mpoidiwrecTask.find(pf->oid); itw != g_mpoidiwrecTask.end()) j.iwrecFirst = itw->second;
            ajob.push_back(std::move(j));
            it = ajob.end() - 1;
        }
        it->aoidTask.push_back(pt->oid);
        for (const PROGREF* pr : {&pt->prC8, &pt->prD0})
            if (pr->F()) it->aprCount.push_back(*pr);
        if (pt->prD8.F()) it->aprFlag.push_back(pt->prD8);
        if (it->aoidTask.size() == 1)
        {
            if (pj->prD0.F()) it->aprCount.push_back(pj->prD0);
            if (pj->prD8.F()) it->aprFlag.push_back(pj->prD8);
        }
    }
    if (const char* szf = std::getenv("SLY2N_STRFIND"))   // (debug: the localized strings containing the text)
        for (int id = 0; id < 4000; ++id)
            if (const std::string str = StrLocalized(psw, id); str.find(szf) != std::string::npos)
                std::printf("strfind: %d '%s'\n", id, str.substr(0, 120).c_str());
    if (std::getenv("SLY2N_REPLAYPRINT"))   // (debug: every job object of the world, its first / last task, its tasks)
        for (LO* plo : psw->mpcidaplo[0x84])
            if (JOB* pj = PjobFromLo(plo))
            {
                int c = 0;
                for (TASK* pt : AptaskAll()) c += pt->pjob == pj;
                std::printf("replay-job: %d name %d '%s' first %d last %d tasks %d exit %d\n", pj->oid, pj->n6c,
                            pj->n6c != -1 ? StrLocalized(psw, pj->n6c).c_str() : "", pj->ptaskFirst ? pj->ptaskFirst->oid : -1,
                            pj->ptaskLast ? pj->ptaskLast->oid : -1, c, pj->ploCC ? pj->ploCC->oid : -1);
                if (pj->ptaskLast)
                    for (TASK* px : pj->ptaskLast->aptaskNext)
                        std::printf("replay-job:   next %d (job %d) c4 %d\n", px->oid, px->pjob ? px->pjob->oid : -1, px->ploC4 ? px->ploC4->oid : -1);
            }
    psw->tasks = tasksSaved;
    s_psw = pswSaved;
    return ajob;
}

// (co-op, coop.h) the other game's step started / done, applied here (its own notification suppressed by coop.cpp)
bool FCoopApplyTaskState(SW* psw, OID oid, int n)
{
    TASK* p = PtaskFind(oid);
    if (!p || !psw || p->nState == n) return false;
    s_psw = psw;
    ChangeTaskState(p, n);
    return p->nState == n;
}

// (co-op, coop.h) the objects the current job's steps place or use (TASKOBJE): its characters (Dimitri), run only by
// the host's game, where the job's scripts run
void CoopCollectJobObjects(SW* psw, std::unordered_set<int>& setoid)
{
    if (!psw) return;
    JOB* pjob = psw->tasks.pjobCur;
    for (LO* plo : psw->mpcidaplo[0x85])
    {
        TASK* p = PtaskFromLo(plo);
        if (!p) continue;
        if (!(pjob && p->pjob == pjob) && p->nState != 1) continue;
        for (const TASKOBJE& o : p->aobje) setoid.insert(o.oidA);
    }
    if (pjob)
        for (const TASKOBJE& o : pjob->aobje) setoid.insert(o.oidA);
}
