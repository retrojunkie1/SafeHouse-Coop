#include "scenesw.h"
#include "paint.h"
#include "cosmetic.h"
#include "simdmat.h"
#include "drawdist.h"
#include "display.h"
#include "pccharacter.h"
#include "rope.h"

#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "bone.h"
#include "booty.h"
#include "brx.h"
#include "cm.h"
#include "clock.h"
#include "step.h"
#include "scene.h"
#include "worldaccess.h"
#include "sw.h"
#include "trail.h"
#include "vehiclehooks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

namespace
{
void PlaceGlobset(ALO* palo, SCENEGLOBSET& sgs)
{
    const GLOBSET& gs = *sgs.pglobset;
    sgs.oid = palo->oid;
    sgs.amatGlob.resize(gs.aglob.size(), glm::mat4(1.0f));
    sgs.afVisible.resize(gs.aglob.size(), 1);
    const glm::mat4 matAlo = palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    // orig: 0x1686AC: the whole sphere uses ALO +0x60 (the object record).
    // Actors move this record independently of record zero; group placement below is separate.
    const glm::mat4 matBounds = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
    sgs.fBounds = true;
    sgs.posBounds = glm::vec3(matBounds * glm::vec4(gs.posCenter, 1.0f));
    sgs.sBoundsRadius = gs.sRadius;
    sgs.sMRD = palo->sRadius;
    // (QoL draw distance above 1x: a tree with a character appears at the original's activation radius (activate.cpp
    // NDrawDistanceRoot), so it fades in there as the original's, not at the radius x the option)
    sgs.fMrdOriginal = g_display.drawDistance > 1.0f && [&] {
        const LO* proot = palo;
        while (proot->paloParent) proot = proot->paloParent;
        return FDrawDistanceActorTree(proot);
    }();
    // the groups' owner records (the S16 through vt slot 51; the object's records do not change in a world)
    if (!sgs.fOwners)
    {
        sgs.apxfOwner.resize(gs.aglob.size());
        for (size_t i = 0; i < gs.aglob.size(); ++i)
            sgs.apxfOwner[i] = gs.aglob[i].oidOwner >= 0 ? PxfFindAlo(palo, gs.aglob[i].oidOwner) : nullptr;
        sgs.fOwners = true;
    }
    for (size_t i = 0; i < gs.aglob.size(); ++i)
    {
        const GLOB& glob = gs.aglob[i];
        sgs.afVisible[i] = FAloGlobShown(palo, glob.n0, !(glob.grfGroup & 0x800)) ? 1 : 0;  // (alo.h: the run time's flags)
        static const char* s_szShow = std::getenv("SLY2_FORCESHOW");   // debugging: these oids' groups all shown
        if (s_szShow && std::strstr(s_szShow, (std::string(",") + std::to_string(palo->oid) + ",").c_str())) sgs.afVisible[i] = 1;
        if (sgs.afVisible[i] && FCosmeticHidesGlob(palo, i)) sgs.afVisible[i] = 0;   // (QoL, cosmetic.h)
        // the group's owner record, else the object's own placement
        const XF* pxf = sgs.apxfOwner[i];
        glm::mat4 m = pxf ? pxf->matWorld : matAlo;
        if (glob.fMatrix) m = MulMat4(m, glob.dmat);  // an instance's own matrix (simdmat.h: glm's product exactly)
        sgs.amatGlob[i] = m;
    }
    // skinned groups: their bone matrices (animation code, bone.cpp; empty before InitWorldAnimation binds them)
    static const bool s_fNoSkin = std::getenv("SLY2_NOSKIN") != nullptr;   // debugging: bind pose
    sgs.aamatBone.resize(gs.aglob.size());
    for (size_t i = 0; i < gs.aglob.size() && !s_fNoSkin; ++i)
    {
        const GLOB& glob = gs.aglob[i];
        const GLOB& geom = glob.iglobTemplate >= 0 && glob.abone.empty() ? gs.aglob[size_t(glob.iglobTemplate)] : glob;
        if (geom.abone.empty() || !FGlobBoneMatrices(palo, static_cast<int>(&geom - gs.aglob.data()), &sgs.aamatBone[i]))
            sgs.aamatBone[i].clear();
    }
    // orig: 0x13C000 -> 0x1A5EE8: a carried booty's scale affects drawing only. Bone matrices already transform
    // vertices to world space (bone.cpp 0x1684D0), replacing the group matrix in the shader, so both need the delta.
    glm::mat4 dmat;
    if (FBootyDrawDelta(palo, &dmat))
    {
        sgs.posBounds = glm::vec3(dmat * glm::vec4(sgs.posBounds, 1.0f));
        sgs.sBoundsRadius *= std::sqrt(std::max({glm::dot(glm::vec3(dmat[0]), glm::vec3(dmat[0])),
                                                glm::dot(glm::vec3(dmat[1]), glm::vec3(dmat[1])),
                                                glm::dot(glm::vec3(dmat[2]), glm::vec3(dmat[2]))}));
        for (glm::mat4& mat : sgs.amatGlob) mat = MulMat4(dmat, mat);
        if (!s_fNoSkin)
            for (auto& amat : sgs.aamatBone)
                for (glm::mat4& mat : amat) mat = MulMat4(dmat, mat);
    }
    // the draw alphas (0x1F3C10: gFade, gFade; 0x1425B0: gFade x +0x10D4, gFade x +0x10D8)
    sgs.gAlpha = sgs.gAlpha2 = palo->gFade;
    sgs.ipaint = IPaintProfileOfAlo(palo);
    if (FIsLoDerivedFrom(palo, 0x34))   // (PactorFromLo would make a missing actor: only an existing one)
        if (const ACTOR* pa = dynamic_cast<const ACTOR*>(PstepFromLo(palo)))
        {
            sgs.gAlpha *= pa->g10D4;
            sgs.gAlpha2 *= pa->g10D8;
            sgs.gFlash = pa->dmg.gFlash;
            static const char* s_szFlash = std::getenv("SLY2N_FLASHTEST");   // debugging: every actor's damage flash
            if (s_szFlash) sgs.gFlash = float(std::atof(s_szFlash));
            static const char* s_test = std::getenv("SLY2_FADETEST");   // debugging: every actor drawn at this alpha
            if (s_test) sgs.gAlpha = sgs.gAlpha2 = float(std::atof(s_test));
        }
    static const bool s_fDbg = std::getenv("SLY2_ALPHADBG") != nullptr;   // debugging: the faded objects
    if (s_fDbg && (sgs.gAlpha != 1.0f || sgs.gAlpha2 != 1.0f))
        std::printf("alpha oid %d cid %d: %.3f %.3f\n", palo->oid, int(palo->Cid()), sgs.gAlpha, sgs.gAlpha2);
    PlaceTrailScene(palo, sgs);
    PlaceRopeMeshScene(palo, sgs);
    sgs.fTransform = true;
    // debug: SLY2N_SCENELIST=t: every object placed in the scene in the frame at clock t: its bounds' screen point
    if (static const char* s_szList = std::getenv("SLY2N_SCENELIST"); s_szList)
        if (const float t = float(std::atof(s_szList)); g_clock.t >= t && g_clock.t < t + 0.0166f)
        {
            int cVis = 0;
            for (size_t i = 0; i < sgs.afVisible.size(); ++i) cVis += sgs.afVisible[i];
            const glm::vec3 scr = ConvertCmWorldToScreen(&g_cm, sgs.posBounds);
            std::printf("scenelist %d cid 0x%X parent %d vis %d/%zu at %.0f %.0f %.0f r %.0f screen %.3f %.3f %.0f alpha %.2f\n", palo->oid, palo->Cid(),
                        palo->paloParent ? palo->paloParent->oid : -1, cVis, sgs.afVisible.size(), sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z,
                        sgs.sBoundsRadius, scr.x, scr.y, scr.z, sgs.gAlpha);
        }
    // debug: SLY2N_SCENEOID=oid: that object's scene entries (each copy): its groups' places, shown flags, first bones
    static const char* s_szOid = std::getenv("SLY2N_SCENEOID");
    if (s_szOid && palo->oid == std::atoi(s_szOid))
    {
        const glm::vec3 pos = matAlo[3];
        std::printf("sceneoid %d %p clone %d in %d at %.0f %.0f %.0f fade %.6f rate %.6f:", palo->oid, (void*)palo, palo->ploCloneOf != nullptr, palo->fInWorld, pos.x, pos.y, pos.z, palo->gFade, palo->dgFade);
        for (size_t i = 0; i < gs.aglob.size() && i < 6; ++i)
        {
            const glm::vec3 p = sgs.amatGlob[i][3];
            const glm::vec3 b = sgs.aamatBone[i].empty() ? glm::vec3(0.0f) : glm::vec3(sgs.aamatBone[i][0][3]);
            std::printf(" [%d %.0f %.0f %.0f bone %.0f %.0f %.0f]", sgs.afVisible[i], p.x, p.y, p.z, b.x, b.y, b.z);
        }
        int cVis = 0, cBone = 0;
        for (size_t i = 0; i < gs.aglob.size(); ++i) { cVis += sgs.afVisible[i]; cBone += !sgs.aamatBone[i].empty(); }
        std::printf(" | %zu groups, %d shown, %d skinned:", gs.aglob.size(), cVis, cBone);
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            std::printf(" %d%s%s", gs.aglob[i].n0, sgs.afVisible[i] ? "+" : "", sgs.aamatBone[i].empty() ? "" : "s");
            if (std::getenv("SLY2N_SCENEOIDSHD"))
                for (const SUBGLOB& sg : gs.aglob[i].asubglob)
                    if (sg.pshd)
                        std::printf("<rp %d mesh 0x%X shd %d k%d grf 0x%X rgba %08X %08X %08X>", gs.aglob[i].rp, gs.aglob[i].grfMesh, sg.pshd->id,
                                    sg.pshd->shdk, sg.pshd->grfshd, sg.pshd->argba[0], sg.pshd->argba[1], sg.pshd->argba[2]);
            if (!sgs.aamatBone[i].empty())
            {
                const glm::mat4& m = sgs.aamatBone[i][0];
                std::printf("(%zu bones, b0 %.0f %.0f %.0f scale %.2f)", sgs.aamatBone[i].size(), m[3][0], m[3][1], m[3][2], glm::length(glm::vec3(m[0])));
            }
        }
        std::printf("\n");
    }
}
}

void PlaceSceneEntry(ALO* palo, SCENEGLOBSET& sgs) { PlaceGlobset(palo, sgs); }

void UpdateSceneFromSw(SCENE& scene, const std::vector<ALO*>& apalo, const SW* pswDrawn)
{
    // (QoL draw distance above 1x: the inactive trees the option draws placed with their spinning / animated parts
    // posed, the records restored after the placement; drawdist.h)
    struct POSES { bool f; ~POSES() { if (f) EndDrawDistancePoses(); } } poses{pswDrawn != nullptr};
    if (pswDrawn) BeginDrawDistancePoses(pswDrawn, apalo);
    for (size_t i = 0; i < scene.aglobset.size() && i < apalo.size(); ++i)
    {
        SCENEGLOBSET& sgs = scene.aglobset[i];
        // (with the world: what it will not draw this frame keeps its last placement, hidden: ApplyDrawPass's test
        // done first; the original places only what it draws)
        static const char* s_szForce = std::getenv("SLY2_FORCEDRAW");
        const bool fForce = s_szForce && std::strstr(s_szForce, (std::string(",") + std::to_string(apalo[i]->oid) + ",").c_str());
        const int nPc = NPcCharDrawOverride(apalo[i]);   // (PC addition, debug menu only: pccharacter.h)
        if (nPc == 0 || (nPc < 0 && pswDrawn && sgs.fTransform && !fForce && !FAloDrawn(pswDrawn, apalo[i])))
        {
            std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);
            continue;
        }
        DrawVehiclePose(apalo[i]); // orig0x1D41F8, wheel transforms before base placement
        PlaceGlobset(apalo[i], sgs);
    }
}

void ApplyDrawPass(const SW* psw, SCENE& scene, const std::vector<ALO*>& apalo)
{
    static const char* s_szForce = std::getenv("SLY2_FORCEDRAW");   // debugging: these oids drawn regardless
    for (size_t i = 0; i < scene.aglobset.size() && i < apalo.size(); ++i)
    {
        if (s_szForce && std::strstr(s_szForce, (std::string(",") + std::to_string(apalo[i]->oid) + ",").c_str())) continue;
        const int nPc = NPcCharDrawOverride(apalo[i]);   // (PC addition, debug menu only: pccharacter.h)
        if (nPc == 1) continue;
        if (nPc == 0 || !FAloDrawn(psw, apalo[i]))
        {
            std::fill(scene.aglobset[i].afVisible.begin(), scene.aglobset[i].afVisible.end(), 0);
            static const bool s_fDbg = std::getenv("SLY2_DRAWDBG") != nullptr;   // debugging: the objects not drawn
            if (s_fDbg)
            {
                static bool s_f = false;
                if (!s_f) { s_f = true; std::printf("draw pass: camera mask %u, current object %d\n", g_actview.grfCm, g_cm.ploCur ? g_cm.ploCur->oid : -1); }
                const ALO* pa = apalo[i];
                const glm::vec3 pos = pa->axf.empty() ? glm::vec3(0.0f) : glm::vec3(pa->axf[0]->matWorld[3]);
                const uint32_t grf = pa->iupd >= 0 ? psw->aupd[size_t(pa->iupd)].grf : 0xFFFFFFFFu;
                std::printf("not drawn %d cid 0x%X iupd %d grf 0x%X draw 0x%X mask 0x%X fade %.2f inworld %d at %.0f %.0f %.0f\n", pa->oid, int(pa->Cid()), pa->iupd, grf,
                            pa->pcls ? pa->pcls->PfnOrig(19) : 0, pa->bMask, pa->gFade, pa->fInWorld, pos.x, pos.y, pos.z);
            }
        }
    }
}

int CBuildSceneFromSw(SW* psw, SCENE& scene, std::vector<ALO*>* papalo, const std::unordered_set<const ALO*>* psetHave)
{
    int c = 0;
    std::vector<ALO*> apaloPc;   // (PC addition, debug menu only: a guest character's objects last, pccharacter.h)
    AppendPcCharSceneAlos(apaloPc);
    const std::vector<LO*> aploPc(apaloPc.begin(), apaloPc.end());
    for (int cid = 0x30; cid <= 0x6D; ++cid)
        for (LO* plo : *(cid < 0x6D ? &psw->mpcidaplo[cid] : &aploPc))
        {
            ALO* palo = static_cast<ALO*>(plo);
            PostLoadTrail(palo);  // runtime clones own their output mesh before scene construction
            PostLoadRopeMesh(palo);
            if (!palo->pglobset || palo->pglobset->aglob.empty()) continue;
            if (psetHave && psetHave->count(palo)) continue;
            SCENEGLOBSET sgs;
            // A glob set shared by clones is shared by their placements too, unless the lighting bakes into its
            // vertices per placement (lit or unbaked groups): then each placement gets its own copy.
            bool fLit = false;
            for (const GLOB& g : palo->pglobset->aglob)
                fLit = fLit || (g.grfMesh & 1) || std::any_of(g.avtx.begin(), g.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; });
            sgs.pglobset = fLit && palo->Cid() != 0x69 && palo->pglobset.use_count() > 1 ? std::make_shared<GLOBSET>(*palo->pglobset) : palo->pglobset;
            PlaceGlobset(palo, sgs);
            scene.aglobset.push_back(std::move(sgs));
            if (papalo) papalo->push_back(palo);
            ++c;
        }
    return c;
}

// The objects made after the scene's build (the spawners' clones, PloCloneLo at run time: a guard spawned from a
// placed one and its parts): appended to the scene (no new lighting: their glob sets' as loaded). Scans only when
// the world's class lists grew. Returns how many were added (then BuildSceneGpu builds their geometry).
int CAddNewAlosToScene(SW* psw, SCENE& scene, std::vector<ALO*>* papalo)
{
    static const SW* s_psw = nullptr;
    static size_t s_c = 0;
    size_t c = 0;
    for (int cid = 0x30; cid < 0x6D; ++cid) c += psw->mpcidaplo[cid].size();
    if (s_psw == psw && s_c == c) return 0;
    s_psw = psw;
    s_c = c;
    const std::unordered_set<const ALO*> set(papalo->begin(), papalo->end());
    const size_t c0 = papalo->size();
    const int cAdd = CBuildSceneFromSw(psw, scene, papalo, &set);
    if (cAdd > 0 && std::getenv("SLY2N_SPAWNDEBUG"))
        for (size_t i = c0; i < papalo->size(); ++i)
            std::printf("scene: added %d (cid 0x%X)%s\n", (*papalo)[i]->oid, int((*papalo)[i]->Cid()), (*papalo)[i]->fInWorld ? "" : " (out of the world)");
    return cAdd;
}
