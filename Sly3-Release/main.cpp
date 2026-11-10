// Sly2-Release: Sly 2: Band of Thieves as a native PC port (see PLAN.md).
// Development commands for now; the game loop comes with milestone M2/M3.

#include <set>
#include "rumble.h"
#include "bis.h"
#include "display.h"
#include "brx.h"
#include "cls.h"
#include "disc.h"
#include "convert.h"
#include "texmod.h"
#include "region.h"
#include "readtrace.h"
#include "sw.h"
#include "game.h"
#include "scene.h"
#include "view.h"
#include "moviegl.h"
#include "fmv.h"
#include "sound.h"
#include "splicecheck.h"
#include "animtest.h"
#include "game/sly3.h"
#include "game/cond3.h"
#include "../platform/platform.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <map>
#include <string>

int CrvCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& world);  // cmcheck.cpp
int ActorCheck(const std::string& pathTrace, const std::string& ranges, const std::string& pathRam);  // cmcheck.cpp
int CmCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& ranges, bool fFree, bool fVerbose);  // cmcheck.cpp

namespace
{
// The game's data (disc.h): --disc PATH (a disc image, or a folder holding disc.iso), else the data directory (the
// launcher's --data-dir: installations/sly3, which holds disc.iso and the system files; platform/install.h).
std::string DiscPath(const std::string& explicitDisc)
{
    const std::filesystem::path dir = explicitDisc.empty() ? cane::dataDirectory() : cane::path(explicitDisc);
    if (std::filesystem::is_directory(dir) && std::filesystem::is_regular_file(dir / "disc.iso")) return cane::utf8(dir / "disc.iso");
    return explicitDisc.empty() ? cane::utf8(dir) : explicitDisc;
}

int ListKeys()
{
    // the worlds (orig3: names 0x2EDE48, episodes 0x2EDF10, keys 0x2EDFD8) and the executable's other keys (movies)
    for (int i = 0; i < sly3::CWORLD; ++i)
    {
        const FK fk = g_disc.FkWorld(i);
        std::printf("world %2d %-16s %-11s sector 0x%06X cb 0x%08X id %u%s\n", i, g_disc.SzWorldElf(i).c_str(),
                    sly3::SzEpisode(g_disc.IepisodeOfWorld(i)), fk.isector, fk.cb, fk.id, fk.cb ? "" : " (no file)");
    }
    for (const auto& [va, fk] : g_disc.ElfKeys())
        if (va >= sly3::VA_MOVIE_KEYS)
            std::printf("movie 0x%06X id %5u sector 0x%06X cb 0x%08X\n", va, fk.id, fk.isector, fk.cb);
    return 0;
}

std::string g_ramPath;

int TestLoad(const std::string& worldPath, const std::string& mapPath, bool fVerbose)
{
    std::ifstream in(cane::path(worldPath), std::ios::binary);
    std::vector<uint8_t> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SCENE scene;
    std::string error;
    const bool fOk = FLoadSceneFromMap(ab, mapPath, scene, fVerbose, error);
    std::printf("texture blocks %d, bulk blocks %d: CLUTs %d BMPs %d SAAs %d shaders %d (filtered %d, shared %d)\n",
                scene.cblockShd, g_shdstats.cbulk, g_shdstats.cclut, g_shdstats.cbmp, g_shdstats.csaa, g_shdstats.cshd,
                g_shdstats.cfiltered, g_shdstats.cshared);
    std::printf("globsets %d: globs %d (types %d/%d/%d/%d, filtered %d), instances %d, skinned %d, vertices %d, "
                "triangles %d, cel edges %d, effect strips %d, entries without shader %d\n",
                g_globstats.cglobset, g_globstats.cglob, g_globstats.cmeshk[0], g_globstats.cmeshk[1], g_globstats.cmeshk[2],
                g_globstats.cmeshk[3], g_globstats.cfiltered, g_globstats.cinstance, g_globstats.cskinned, g_globstats.cvtx,
                g_globstats.ctri, g_globstats.cedge, g_globstats.cfx, g_globstats.cnoshd);
    {
        int csub = 0, csaa = 0; std::map<int, int> mpcid;
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob) for (const SUBGLOB& sub : g.asubglob) { ++csub; if (sub.psaa) { ++csaa; ++mpcid[sub.psaa->cid]; } }
        std::printf("entries %d, animated %d:", csub, csaa);
        for (auto [cid, c] : mpcid) std::printf(" cid %d x%d", cid, c);
        std::printf("\n");
        std::map<int, int> mpall; for (const auto& psaa : g_shds.asaa) ++mpall[psaa->cid];
        std::map<std::string, int> mpk;
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob)
        {
            char sz[96];
            if (g.meshk >= 2) for (const SUBFX& fx : g.asubfx) { std::snprintf(sz, sizeof(sz), "meshk %d rp %d shdk %d grfshd 0x%X rgba %08X %08X", g.meshk, g.rp, fx.pshd ? fx.pshd->shdk : -1, fx.pshd ? fx.pshd->grfshd : 0, fx.pshd ? fx.pshd->argba[0] : 0, fx.pshd ? fx.pshd->argba[1] : 0); ++mpk[sz]; }
            else for (const SUBGLOB& sub : g.asubglob) if (sub.pshd && (sub.pshd->shdk == 2 || sub.pshd->shdk == 3 || g.rp == 11 || g.rp == 13 || g.rp == 15 || g.rp == 18 || g.rp == 7 || g.rp == 8)) { std::snprintf(sz, sizeof(sz), "meshk %d rp %d shdk %d grfshd 0x%X rgba %08X %08X %08X tex %zu", g.meshk, g.rp, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2], sub.pshd->atex.size()); ++mpk[sz]; }
        }
        for (auto& [k, c] : mpk) std::printf("  %s x%d\n", k.c_str(), c);
        int crp[32] = {};
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob) ++crp[g.rp & 31];
        std::printf("rp histogram:"); for (int i = 0; i < 32; ++i) if (crp[i]) std::printf(" %d:%d", i, crp[i]); std::printf("\n");
        std::printf("SAA classes:"); for (auto [cid, c] : mpall) std::printf(" %d x%d", cid, c); std::printf("\n");
    }
    {
        int celem = 0, cext = 0;
        for (const auto& pso : scene.aso) { celem += int(pso->aelem.size()); cext += pso->fExternal; }
        std::printf("screen objects %zu (external %d), elements %d\n", scene.aso.size(), cext, celem);
        int cq = 0, cqShd = 0, cqBmp = 0;
        std::function<void(const std::vector<SELEM>&)> walk = [&](const std::vector<SELEM>& ae) {
            for (const SELEM& e : ae)
            {
                if (e.kind == SELEMK_Image || e.kind == SELEMK_Quad)
                {
                    ++cq;
                    bool fShd = false;
                    for (const auto& pshd : g_shds.ashd) if (pshd->id == e.idTexture) fShd = true;
                    cqShd += fShd;
                    cqBmp += g_shds.mpidbmp.count(e.idTexture) ? 1 : 0;
                    if (cq <= 5) std::printf("  quad kind %u id 0x%X rect %d %d %d %d shd %d\n", e.kind, e.idTexture, e.x0, e.y0, e.x1, e.y1, int(fShd));
                }
                if (e.kind == SELEMK_Set) walk(e.asub);
            }
        };
        for (const auto& pso : scene.aso) walk(pso->aelem);
        std::printf("  quads %d: with a shader of that id %d, a bitmap %d\n", cq, cqShd, cqBmp);
        for (const FONTBRX& f : FindFonts(scene.aso))
        {
            std::printf("  font: object 0x%X element %d, %zu glyphs:", f.pso->oid, f.ifont, f.pset->aul.size());
            if (f.pset->aul.size() < 40) for (uint32_t c : f.pset->aul) std::printf(" %c", char(c));
            std::printf("\n");
            std::map<uint32_t, int> mprgba; std::map<int, int> mpprim;
            for (const SELEM& g : f.pset->asub) for (const SPOLY& p : g.apoly) { ++mprgba[p.rgba]; ++mpprim[p.primitive * 1000 + p.control]; }
            for (auto [k, c] : mprgba) std::printf("    rgba %08X x%d\n", k, c);
            for (auto [k, c] : mpprim) std::printf("    prim %d control %d x%d\n", k / 1000, k % 1000, c);
            const SELEM* pa = f.PglyphFromCh('A');
            if (pa) for (const SPOLY& p : pa->apoly) { std::printf("    A poly prim %d cv %zu:", p.primitive, p.axy.size()); for (auto& v : p.axy) std::printf(" %d,%d", v[0], v[1]); std::printf("\n"); }
            std::printf("    width A %.1f\n", f.DxFromCh('A'));
        }
    }
    std::printf("strips %d (starting at an odd vertex: %d)\n", g_globstats.cstrip, g_globstats.cstripOdd);
    if (!fOk) { std::printf("FAILED: %s\n", error.c_str()); return 1; }
    std::printf("all blocks end where the map says\n");
    if (!g_ramPath.empty())
    {
        const int c = CApplyRamTransforms(scene, g_ramPath);
        std::printf("reference RAM: %d of %zu globsets located\n", c, scene.aglobset.size());
        if (fVerbose)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                if (sgs.fTransform)
                    std::printf("  glob at %9zu -> 0x%06X pos %.1f %.1f %.1f\n", sgs.ib, sgs.vaGlob, sgs.amatGlob[0][3][0],
                                sgs.amatGlob[0][3][1], sgs.amatGlob[0][3][2]);
    }
    return 0;
}

// --dump-world W|all: the world loader (orig3 0x14F440) in its walk mode (brx.h WALK): every block read as the original
// reads it, the objects' bodies skipped by their sizes; each group must end exactly at the size its record carries.
int DumpWorldOne(int iworld, FILE* pf, bool fObjects, bool fGroups, const std::string& out)
{
    FK fk = g_disc.FkWorld(iworld);
    if (!fk.cb) return 0;
    std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    if (!out.empty())
        std::ofstream(cane::path(out), std::ios::binary).write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
    const std::size_t cbFile = ab.size();
    auto psw = std::make_unique<SW>();
    psw->filter = iworld;
    g_walk = WALK{};
    g_walk.fOn = true;
    CBinaryInputStream bis;
    bis.OpenBuffer(std::move(ab));
    std::string error;
    try { LoadSwFromBrx(psw.get(), &bis); }
    catch (const std::exception& e) { error = e.what(); }
    g_walk.fOn = false;
    int cskipped = 0;
    for (const WE& we : psw->awe) cskipped += we.cinst == 0;
    int cunnamed = 0;
    for (auto [cid, c] : g_walk.mpcidc) cunnamed += !sly3::SzClassName(cid);
    std::fprintf(pf, "world %2d %-14s %9zu bytes: %s at %zu (%zu after); %zu groups (%d without instances), %zu objects of %zu "
                     "classes (%d unnamed), %d splice modules, %zu texts, option set %d of %zu (condition %d)\n",
                 iworld, g_disc.SzWorldElf(iworld).c_str(), cbFile, error.empty() ? "ok" : "FAILED", bis.m_ib, cbFile - bis.m_ib,
                 psw->awe.size(), cskipped, g_walk.aobj.size(), g_walk.mpcidc.size(), cunnamed, g_walk.cmodule,
                 psw->atext.size(), psw->ioptsetChosen, psw->aidOptset.size(), g_condset3.cond);
    if (!error.empty()) std::fprintf(pf, "  error: %s\n", error.c_str());
    if (fGroups)
        for (std::size_t i = 0; i < psw->awe.size(); ++i)
        {
            const WE& we = psw->awe[i];
            std::fprintf(pf, "  group %3zu %-40s shared %d x60 %d x10 %d x14 %d instances %d cb %u objects %d\n", i,
                         we.StrName().c_str(), we.x5C, we.x60, we.x10, we.x14, we.cinst, we.cb, we.cloLoaded);
        }
    if (fGroups || fObjects)
    {
        std::fprintf(pf, "  classes (top-level objects; Sly 3 cid, count, name):\n");
        for (auto [cid, c] : g_walk.mpcidc)
            std::fprintf(pf, "    0x%02X %5d %s\n", cid, c, g_acls[cid].name);
    }
    if (fObjects)
        for (const WALKOBJ& o : g_walk.aobj)
        {
            std::fprintf(pf, "  obj at %9zu group %3d mode %d %-16s oid %7d module %4d cb %8d", o.ib, o.igrp, o.mode,
                         StrClassName(o.cid).c_str(), int(o.oid), o.n, o.cb);
            if (!o.afilter.empty() || !o.acond.empty())
            {
                std::fprintf(pf, " filters");
                for (int16_t f : o.afilter) std::fprintf(pf, " %d", f);
                std::fprintf(pf, " conditions");
                for (int32_t c : o.acond) std::fprintf(pf, " %d", c);
            }
            std::fprintf(pf, "\n");
        }
    return error.empty() ? 0 : 1;
}

int DumpWorld(const std::string& w, const std::string& out, bool fObjects, bool fGroups)
{
    InitClasses();
    int cfail = 0, cworld = 0;
    if (w == "all")
    {
        for (int i = 0; i < sly3::CWORLD; ++i)
            if (g_disc.FkWorld(i).cb) { ++cworld; cfail += DumpWorldOne(i, stdout, fObjects, fGroups, ""); }
        std::printf("%d worlds, %d failed\n", cworld, cfail);
        return cfail ? 1 : 0;
    }
    FK fk;
    int iworld = -1;
    std::string error;
    if (!FResolveWorld(w, fk, iworld, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
    return DumpWorldOne(iworld, stdout, fObjects, fGroups, out);
}

// Per ALO: oid and each transform record's world translation (to compare with the reference's records +0x70)
void DumpXfs(const SW* psw, FILE* pf)
{
    for (int cid = 0x30; cid < 0x6D; ++cid)
        for (const LO* plo : psw->mpcidaplo[cid])
        {
            const ALO* palo = static_cast<const ALO*>(plo);
            std::fprintf(pf, "%d %zu", plo->oid, palo->axf.size());
            if (std::getenv("DUMPXF_OPTS"))
            {
                std::fprintf(pf, " opts");
                for (const OPT& o : plo->aopt)
                {
                    std::fprintf(pf, " %d:%d", o.eopid, o.aval.empty() ? 0 : o.aval[0].n);
                    if (!o.aval.empty() && !o.aval[0].an.empty())
                    {
                        std::fprintf(pf, "[");
                        for (int16_t k : o.aval[0].an) std::fprintf(pf, "%d,", k);
                        std::fprintf(pf, "]");
                    }
                }
                std::fprintf(pf, " parent %d", plo->paloParent ? plo->paloParent->oid : -1);
            }
            for (const auto& pxf : palo->axf)
                std::fprintf(pf, " %.1f,%.1f,%.1f", pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z);
            std::fputc('\n', pf);
        }
}

void DumpObjects(const SW* psw, FILE* pf)
{
    std::fprintf(pf, "# cid class oid parent-oid group name\n");
    for (int cid = 0; cid < CID_Max; ++cid)
        for (const LO* plo : psw->mpcidaplo[cid])
        {
            // (world files carry no object names: objects are known by oid; the class name is printed)
            const std::string name = "-";
            std::fprintf(pf, "0x%02X %s %d %d %d %s", cid, g_acls[cid].name, plo->oid,
                         plo->paloParent ? plo->paloParent->oid : -1, plo->igrp, name.c_str());
            if (std::getenv("SLY2_DUMP_EXTRA"))
            {
                const LO* proot = plo;
                while (proot->paloParent) proot = proot->paloParent;
                const WE* pwe = proot->igrp >= 0 && proot->igrp < int(psw->awe.size()) ? &psw->awe[proot->igrp] : nullptr;
                std::fprintf(pf, " root %d clone %d x5c %d used %d cinst %d frame %d", proot->oid, proot->iclone,
                             pwe ? pwe->x5C : -9, pwe ? pwe->cUsed : -9, pwe ? pwe->cinst : -9, plo->pframe ? 1 : 0);
                std::fprintf(pf, " grfLoad 0x%x st %d removed %d inworld %d mask 0x%x radius %g opts", plo->grfLoad, plo->st, plo->fRemoved,
                             plo->fInWorld, plo->bMask, plo->sRadius);
                for (const OPT& o : plo->aopt)
                {
                    std::fprintf(pf, " %d:%d", o.eopid, o.aval.empty() ? 0 : o.aval[0].n);
                    if (!o.aval.empty() && !o.aval[0].an.empty())
                    {
                        std::fprintf(pf, "[");
                        for (int16_t k : o.aval[0].an) std::fprintf(pf, "%d,", k);
                        std::fprintf(pf, "]");
                    }
                }
            }
            std::fputc('\n', pf);
        }
}
}

int PhysCheckMain(const std::vector<std::string>& args);  // physcheck.cpp

#include "pcguard.h"

#if defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
namespace
{
// a crash's stack on stderr (the release has no debugger): the addresses for addr2line
void OnCrashSignal(int sig)
{
    void* apv[64];
    const int c = backtrace(apv, 64);
    const char sz[] = "Sly3-Release: crashed, stack:\n";
    (void)!write(2, sz, sizeof(sz) - 1);
    backtrace_symbols_fd(apv, c, 2);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
}
#endif

int main(int argc, char** argv)
{
#if defined(__linux__) || defined(__APPLE__)
    std::signal(SIGSEGV, OnCrashSignal);
    std::signal(SIGABRT, OnCrashSignal);
#endif
    // the mods' audio tools (audiomod.cpp: --make-music, --make-vpk, --make-vag, --export-wav): no game data needed
    {
        int AudioToolMain(int, char**);
        if (const int rc = AudioToolMain(argc, argv); rc >= 0) return rc;
    }
    std::string dumpXf;
    GAMEOPTS go;
    bool fGame = false;
    bool fExportTextures = false;   // (--export-textures: without --convert, the export command)
    bool fWorldGiven = false;   // (no --world with the launcher's --data-dir: the game as released, from the boot)
    std::string disc, out, trace, dumpObjects, dirMaps = "build/loadmaps";
    bool fVerbose = false;
    int idSong = -1, istem = 1;
    double tSeconds = -1.0;  // (unset: 30 s for the sound commands, the whole movie)
    std::string modsDir, texturePack;
    bool fModsGiven = false;
    const char* command = nullptr;
    const char* arg = nullptr;
    int filter = -2;
    bool fDumpObjects = false;
    bool fDumpGroups = false;
    const char* arg2 = nullptr;
    int animRefB = 0;
    VIEWOPTS vo;
    std::string cmRanges = "2DDED0+6A0,2DFBC0+2240,FB1580+1100,FB37E0+80,140C9F0+600,1486000+3400";
    bool fCmFree = false;
    struct { bool fAspect = false; DISPASPECT aspect = DISPASPECT_Fit; int internalHeight = -1, frameRate = -1, vsync = -1, window = -1, wideBackgrounds = -1, hqLogo = -1, longShadows = -1; float drawDistance = -1; } dispo;
    std::vector<std::string> rest, physArgs;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!std::strcmp(argv[i], "--mods") && i + 1 < argc) { modsDir = argv[++i]; fModsGiven = true; }
        else if (!std::strcmp(argv[i], "--texture-pack") && i + 1 < argc) texturePack = argv[++i];
        // the launcher's runtime contract: --data-dir <installations/sly2> --user-dir <profile>/sly2
        else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) cane::dataOverride = std::filesystem::absolute(cane::path(argv[++i]));
        else if (!std::strcmp(argv[i], "--user-dir") && i + 1 < argc) cane::userOverride = std::filesystem::absolute(cane::path(argv[++i]));
        // display options for this run (over system_settings.dat; display.h)
        else if (!std::strcmp(argv[i], "--aspect") && i + 1 < argc)
        {
            if (!FParseAspect(argv[++i], dispo.aspect)) { std::fprintf(stderr, "bad --aspect (fit, 4:3, 16:9, 16:10, ps2-4:3, ps2-16:9)\n"); return 2; }
            dispo.fAspect = true;
        }
        else if (!std::strcmp(argv[i], "--internal-res") && i + 1 < argc) dispo.internalHeight = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frame-rate") && i + 1 < argc) dispo.frameRate = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--draw-distance") && i + 1 < argc) dispo.drawDistance = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--vsync") && i + 1 < argc) dispo.vsync = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--wide-backgrounds") && i + 1 < argc) dispo.wideBackgrounds = std::atoi(argv[++i]);   // (the launcher's option)
        else if (!std::strcmp(argv[i], "--hq-logo") && i + 1 < argc) dispo.hqLogo = std::atoi(argv[++i]);   // (QoL: the launcher's logo in the front end)
        else if (!std::strcmp(argv[i], "--long-shadows") && i + 1 < argc) dispo.longShadows = std::atoi(argv[++i]);   // (QoL: display.h fLongShadows)
        else if (!std::strcmp(argv[i], "--window-mode") && i + 1 < argc) dispo.window = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--list-keys") || !std::strcmp(argv[i], "--list-files")) command = "list-keys";
        else if (!std::strcmp(argv[i], "--vt-dump") && i + 1 < argc) { command = "vt-dump"; arg = argv[++i]; }  // (vtaudit.cpp)
        else if (!std::strcmp(argv[i], "--maps") && i + 1 < argc) dirMaps = argv[++i];
        else if (!std::strcmp(argv[i], "-v")) fVerbose = true;
        else if (!std::strcmp(argv[i], "--blends") || !std::strcmp(argv[i], "--ai") || !std::strcmp(argv[i], "--ai-carry")) {}  // (anim-ref)
        else if (!std::strcmp(argv[i], "--splice-selftest")) command = "splice-selftest";
        else if (!std::strcmp(argv[i], "--rumble-selftest")) command = "rumble-selftest";
        else if (!std::strcmp(argv[i], "--parse-splice") && i + 1 < argc) { command = "parse-splice"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--eval-splice") && i + 1 < argc) { command = "eval-splice"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--dump-world") && i + 1 < argc) { command = "dump-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--groups")) fDumpGroups = true;
        else if (!std::strcmp(argv[i], "--load-world") && i + 1 < argc) { command = "load-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--skeleton-report") && i + 1 < argc)
        {
            static std::string s_szRep;
            s_szRep = std::string("SLY3_SKELREPORT=") + argv[++i];
            putenv(s_szRep.data());   // (the report's file: as the environment variable)
        }
        else if (!std::strcmp(argv[i], "--load-trace") && i + 1 < argc) trace = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-xf") && i + 1 < argc) dumpXf = argv[++i];
        else if (!std::strcmp(argv[i], "--world") && i + 1 < argc)
        {
            fWorldGiven = true;
            // W or W:TASK (a job start, like the reference's --warp)
            std::string w = argv[++i];
            const size_t colon = w.find(':');
            if (colon != std::string::npos) { go.task = std::atoi(w.c_str() + colon + 1); w = w.substr(0, colon); }
            go.world = w;
            fGame = true;
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) { go.cframe = std::atoi(argv[++i]); fGame = true; }
        else if (!std::strcmp(argv[i], "--headless")) { go.fHeadless = true; fGame = true; }
        else if (!std::strcmp(argv[i], "--fixed-dt")) go.fFixedDt = true;
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) go.save = argv[++i];
        else if (!std::strcmp(argv[i], "--print-progress") && i + 1 < argc) { go.aprintProgress.push_back(argv[++i]); fGame = true; }
        else if (!std::strcmp(argv[i], "--frame-dump") && i + 1 < argc) { go.frameDump = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--sly-start") && i + 1 < argc) { go.slyStart = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--sly-ref") && i + 1 < argc) go.slyRef = argv[++i];
        else if (!std::strcmp(argv[i], "--gang-start") && i + 1 < argc) { go.gangStart = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--gang-ref") && i + 1 < argc) go.gangRef = argv[++i];
        else if (!std::strcmp(argv[i], "--press-vblank-base") && i + 1 < argc) go.vbBase = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc)
        {
            if (!FParseJoyPresses(argv[++i], go.apress)) { std::fprintf(stderr, "bad --press\n"); return 2; }
        }
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump-objects"))
        {
            fDumpObjects = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') dumpObjects = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--test-load") && i + 2 < argc) { command = "test-load"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) g_ramPath = argv[++i];
        else if (!std::strcmp(argv[i], "--anim-test") && i + 1 < argc) { command = "anim-test"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--anim-pose") && i + 2 < argc) { command = "anim-pose"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ai-free") && i + 3 < argc) { command = "ai-free"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if (!std::strcmp(argv[i], "--anim-run") && i + 3 < argc) { command = "anim-run"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if (!std::strcmp(argv[i], "--anim-ref") && i + 3 < argc) { command = "anim-ref"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if ((!std::strcmp(argv[i], "--frames") || !std::strcmp(argv[i], "--dump") || !std::strcmp(argv[i], "--then")) && i + 1 < argc) ++i;  // (anim-test / anim-ref)
        else if (!std::strcmp(argv[i], "--cm-check") && i + 1 < argc) { command = "cm-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--cm-ranges") && i + 1 < argc) cmRanges = argv[++i];
        else if (!std::strcmp(argv[i], "--cm-free")) fCmFree = true;
        else if (!std::strcmp(argv[i], "--crv-check") && i + 1 < argc) { command = "crv-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--actor-check") && i + 1 < argc) { command = "actor-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--view-world") && i + 1 < argc) { command = "view-world"; vo.iworld = std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) vo.screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--camera") && i + 1 < argc) vo.camera = argv[++i];
        else if (!std::strcmp(argv[i], "--camera-ram") && i + 1 < argc) vo.cameraRam = argv[++i];
        else if (!std::strcmp(argv[i], "--loadmaps") && i + 1 < argc) vo.loadmaps = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &vo.width, &vo.height);
        else if (!std::strcmp(argv[i], "--msaa") && i + 1 < argc) vo.msaa = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-cel")) vo.fNoCel = true;
        else if (!std::strcmp(argv[i], "--from-map")) vo.fFromMap = true;
        else if (!std::strcmp(argv[i], "--no-fog")) vo.fNoFog = true;
        else if (!std::strcmp(argv[i], "--no-shadow")) vo.fNoShadow = true;
        else if (!std::strcmp(argv[i], "--time") && i + 1 < argc) vo.tAnim = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--text-test") && i + 2 < argc) { command = "view-world"; vo.iworld = std::atoi(argv[++i]); vo.textTest = argv[++i]; }
        else if (!std::strcmp(argv[i], "--text-size") && i + 1 < argc) vo.textSize = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--hud-ram") && i + 1 < argc) vo.hudRam = argv[++i];
        else if (!std::strcmp(argv[i], "--screens") && i + 1 < argc) vo.screens = argv[++i];
        else if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) vo.cframeBench = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--fade") && i + 1 < argc) vo.fade = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--letterbox") && i + 1 < argc) vo.letterbox = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--wipe") && i + 1 < argc) vo.wipe = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--approx-light")) vo.fApproxLight = true;
        else if (!std::strcmp(argv[i], "--ram-lights")) vo.fRamLights = true;
        else if (!std::strcmp(argv[i], "--particles") && i + 1 < argc) vo.tParticles = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--burst") && i + 1 < argc) vo.oidBurst = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list-sounds") && i + 1 < argc) { command = "list-sounds"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-music") && i + 1 < argc) { command = "play-music"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-sfx") && i + 1 < argc) { command = "play-sfx"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-dialog") && i + 1 < argc) { command = "play-dialog"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-movie") && i + 1 < argc) { command = "play-movie"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--list-movies")) command = "list-movies";
        else if (!std::strcmp(argv[i], "--convert") && i + 1 < argc) { command = "convert"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--convert-list")) command = "convert-list";
        else if (!std::strcmp(argv[i], "--ps2-formats")) g_fConvertPs2 = true;
        else if (!std::strcmp(argv[i], "--flac")) g_nConvertFlac = 1;
        else if (!std::strcmp(argv[i], "--no-flac")) g_nConvertFlac = 0;
        else if (!std::strcmp(argv[i], "--export-textures")) { g_fConvertTextures = true; fExportTextures = true; }
        else if (!std::strcmp(argv[i], "--no-export-textures")) g_fConvertTextures = false;
        else if (!std::strcmp(argv[i], "--dump-text") && i + 1 < argc) { command = "dump-text"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--dump-textures") && i + 2 < argc) { command = "dump-textures"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ps3-hd-textures") && i + 2 < argc) { command = "ps3-hd-textures"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--phys-check"))  // physcheck.cpp: FUNC PREFIX [COUNT] (the rest of the line)
        {
            command = "phys-check";
            while (i + 1 < argc) physArgs.emplace_back(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--view-movie") && i + 1 < argc) { command = "view-movie"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--song") && i + 1 < argc) idSong = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--stem") && i + 1 < argc) istem = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) tSeconds = std::atof(argv[++i]);
        else rest.emplace_back(argv[i]);
    }
    if (filter != -2) g_nFilterCur = filter;  // (--test-load)
    cane::configureRuntime(rest);
    if (command && !std::strcmp(command, "test-load")) return TestLoad(arg, arg2, fVerbose);
    if (command && !std::strcmp(command, "splice-selftest")) return SpliceSelfTestCommand();
    if (command && !std::strcmp(command, "rumble-selftest")) return RumbleSelfTestCommand();
    std::string error;
    // the mods folder: --mods DIR, SLY2_MODS, else <user dir>/Mods; empty: none
    if (!fModsGiven)
    {
        if (const char* sz = std::getenv("SLY2_MODS")) modsDir = sz;
        else modsDir = cane::utf8(cane::userDirectory() / "Mods");
    }
    g_disc.SetModsDirectory(modsDir);
    SetTexturePackDirectory(texturePack);
    if (!g_disc.FOpen(DiscPath(disc), error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    InitLanguage();   // (region.h: the disc's languages, the PC's language)
    if (std::getenv("SLY2_PS3WORLD")) g_fPs3World = true;  // (exploration: a PS3 world through --load-world)

    if (command && !std::strcmp(command, "list-keys")) return ListKeys();
    if (command && !std::strcmp(command, "convert")) return ConvertCommand(arg);
    // --export-textures without --convert: into the installation (or --out DIR); texmod.h
    if (!command && fExportTextures)
    {
        std::string dir = out;
        if (dir.empty())
        {
            const std::filesystem::path data = cane::path(DiscPath(disc));
            if (!std::filesystem::is_directory(data))
            {
                std::fprintf(stderr, "--export-textures: a disc image has no installation folder: give --out DIR\n");
                return 2;
            }
            dir = cane::utf8(data / "Textures");
        }
        return ExportTexturesCommand(dir);
    }
    if (command && !std::strcmp(command, "convert-list")) return ConvertListCommand();
    int DumpTextCommand(const std::string&);  // textmod.cpp
    if (command && !std::strcmp(command, "dump-text")) return DumpTextCommand(arg);
    int DumpTexturesCommand(const std::string&, const std::string&);  // texmod.cpp
    if (command && !std::strcmp(command, "dump-textures")) return DumpTexturesCommand(arg, arg2);
    int Ps3HdTexturesCommand(const std::string&, const std::string&);  // ps3hd.cpp
    if (command && !std::strcmp(command, "ps3-hd-textures")) return Ps3HdTexturesCommand(arg, arg2);
    int VtDumpCommand(const char*);  // vtaudit.cpp
    if (command && !std::strcmp(command, "vt-dump")) return VtDumpCommand(arg);
    if (command && !std::strcmp(command, "phys-check")) return PhysCheckMain(physArgs);
    if (command && !std::strcmp(command, "actor-check")) { InitClasses(); return ActorCheck(arg, cmRanges, g_ramPath); }
    if (command && !std::strcmp(command, "crv-check"))
    {
        InitClasses();
        return CrvCheck(arg, g_ramPath, go.world);
    }
    if (command && !std::strcmp(command, "cm-check"))
    {
        InitClasses();
        return CmCheck(arg, g_ramPath, cmRanges, fCmFree, fVerbose);
    }
    auto applyDisplayOverrides = [&]() {
        if (dispo.fAspect) g_display.aspect = dispo.aspect;
        if (dispo.internalHeight >= 0) g_display.internalHeight = dispo.internalHeight;
        if (dispo.frameRate >= 30) g_display.frameRate = dispo.frameRate;
        if (dispo.drawDistance > 0) g_display.drawDistance = dispo.drawDistance;
        if (dispo.vsync >= 0) g_display.fVsync = dispo.vsync != 0;
        if (dispo.wideBackgrounds >= 0) g_display.fWideBackgrounds = dispo.wideBackgrounds != 0;
        if (dispo.hqLogo >= 0) g_display.fHqLogo = dispo.hqLogo != 0;
        if (dispo.longShadows >= 0) g_display.fLongShadows = dispo.longShadows != 0;
        if (dispo.window >= 0 && dispo.window <= 2) g_display.window = DISPWINDOW(dispo.window);
    };
    applyDisplayOverrides();
    if (command && !std::strcmp(command, "view-world")) return ViewWorld(vo);
    if (command && !std::strcmp(command, "anim-test")) return AnimTestCommand(arg, argc, argv);  // animtest.cpp
    if (command && !std::strcmp(command, "anim-pose")) return AnimPoseCommand(arg, arg2, argc, argv);  // animref.cpp
    int AiFreeCommand(const char*, const char*, const char*, int, char**);  // animref.cpp
    if (command && !std::strcmp(command, "ai-free")) return AiFreeCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "anim-run")) return AnimRunCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "anim-ref")) return AnimRefCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "parse-splice")) return ParseSpliceCommand(arg, dirMaps, fVerbose);
    if (command && !std::strcmp(command, "eval-splice")) return EvalSpliceCommand(arg, dirMaps, fVerbose, out);
    if (command && !std::strcmp(command, "list-sounds")) return ListSoundsCommand(arg, dirMaps);
    if (command && !std::strcmp(command, "play-music")) return PlayMusicCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds, istem);
    if (command && !std::strcmp(command, "play-sfx")) return PlaySfxCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds);
    if (command && !std::strcmp(command, "play-dialog")) return PlayDialogCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds);
    if (command && !std::strcmp(command, "play-movie")) return PlayMovieCommand(arg, out, tSeconds < 0 ? 1e9 : tSeconds);
    if (command && !std::strcmp(command, "list-movies")) return ListMoviesCommand();
    if (command && !std::strcmp(command, "view-movie")) return ViewMovieCommand(arg, vo.screenshot, vo.tAnim, vo.width, vo.height);
    if (command && !std::strcmp(command, "dump-world")) return DumpWorld(arg, out, fDumpObjects, std::getenv("SLY3_DUMP_GROUPS") != nullptr || fDumpGroups);
    if (command && !std::strcmp(command, "load-world"))
    {
        InitClasses();
        // W: a world record index (0..43; the records come from the boot world), or "boot" (id 93) / "frontend"
        // (id 57), the two worlds keyed in the executable.
        const std::string w = arg;
        FK fk;
        int filterDefault = 0;
        std::string err;
        if (!FResolveWorld(w, fk, filterDefault, err))
        {
            std::fprintf(stderr, "%s\n", err.c_str());
            return err.find("nothing to load") != std::string::npos ? 0 : 2;
        }
        auto psw = std::make_unique<SW>();
        psw->filter = filter != -2 ? filter : filterDefault;
        std::size_t cbRead = 0, cbFile = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const bool fOk = FLoadWorldFile(psw.get(), fk, trace, &cbRead, &cbFile);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr,
                     "world %s (id %u, filter %d): %s, read %zu bytes of %zu, %zu objects (%d loaded, %d filtered out, "
                     "%d missions kept, shared (k -1/-2) %d/%d skipped %d, %d proxies, %d size mismatches), %zu groups, %.0f ms\n",
                     w.c_str(), fk.id, psw->filter, fOk ? "loaded" : "FAILED", cbRead, cbFile,
                     psw->aploAll.size(), g_brxstats.cobj, g_brxstats.cskipFilter, g_brxstats.cmission,
                     g_brxstats.cshared1, g_brxstats.cshared2, g_brxstats.cskipShared, g_brxstats.cproxy,
                     g_brxstats.ccbMismatch, psw->awe.size(), ms);
        {
            // (Sly 3 development: the classes read without a checked loader, brx.cpp)
            auto print = [](const char* sz, const std::map<int, int>& mp) {
                if (mp.empty()) return;
                std::fprintf(stderr, "%s:", sz);
                for (auto [cid, c] : mp) std::fprintf(stderr, " %s x%d", StrClassName(cid).c_str(), c);
                std::fprintf(stderr, "\n");
            };
            print("no loader (made, body skipped)", g_brxstats.mpcidcNoLoader);
            print("ALO part only", g_brxstats.mpcidcAloOnly);
            print("rest of the body skipped / misread", g_brxstats.mpcidcSkipped);
            print("read failed", g_brxstats.mpcidcError);
        }
        // (PC tool, the character repainter's cross-game test: SLY3_SKELREPORT=FILE writes each skinned object tree's
        // structure (its objects, their parents, transform counts and the bone objects its meshes bind to): numbers
        // only, no mesh, texture or animation data)
        if (const char* pszRep = std::getenv("SLY3_SKELREPORT"); pszRep && fOk)
        {
            std::ofstream rep(cane::path(pszRep));
            for (const auto& up : psw->aploAll)
            {
                if (!FCidDerivedFrom(up->Cid(), S3CID(0x30)) || up->paloParent) continue;
                const ALO* proot = static_cast<const ALO*>(up.get());
                std::vector<const ALO*> apalo{proot};
                for (size_t i = 0; i < apalo.size(); ++i)
                    for (const LO* pc : apalo[i]->aploChild)
                        if (pc && FCidDerivedFrom(pc->Cid(), S3CID(0x30))) apalo.push_back(static_cast<const ALO*>(pc));
                size_t cbone = 0;
                for (const ALO* pa : apalo)
                    if (pa->pglobset)
                        for (const GLOB& g : pa->pglobset->aglob) cbone += g.abone.size();
                if (cbone == 0) continue;   // (not skinned: no skeleton to compare)
                rep << "tree " << proot->oid << " class " << StrClassName(proot->Cid()) << " objects " << apalo.size() << "\n";
                for (const ALO* pa : apalo)
                {
                    rep << "  alo " << pa->oid << " parent " << (pa->paloParent ? pa->paloParent->oid : -1) << " class "
                        << StrClassName(pa->Cid()) << " xfs " << pa->axf.size();
                    if (pa->pglobset)
                    {
                        std::set<int> setBone;
                        size_t cskin = 0;
                        for (const GLOB& g : pa->pglobset->aglob)
                        {
                            cskin += !g.abone.empty();
                            for (const GLOBBONE& b : g.abone) setBone.insert(b.oid);
                        }
                        rep << " groups " << pa->pglobset->aglob.size() << " skinned " << cskin << " bones";
                        for (int o : setBone) rep << ' ' << o;
                    }
                    rep << "\n";
                }
            }
            std::fprintf(stderr, "skeleton report written to %s\n", pszRep);
        }
        if (std::getenv("SLY3_CENSUS"))   // (debug: objects made per class, as the reference's tools/sly3/census.py lists a RAM)
        {
            std::map<int, int> mpcidc;
            for (const auto& plo : psw->aploAll) ++mpcidc[plo->Cid()];
            for (auto [cid, c] : mpcidc) std::printf("census 0x%02X %-20s %d\n", cid, StrClassName(cid).c_str(), c);
        }
        {
            std::size_t cframe = 0;
            for (const auto& plo : psw->aploAll) cframe += plo->pframe != nullptr;
            std::fprintf(stderr, "%zu objects with a splice frame\n", cframe);
        }
        if (const char* pszOid = std::getenv("SLY2_OWNER_OF"))
        {
            const OID oid = std::atoi(pszOid);
            for (const auto& plo : psw->aploAll)
                for (LO* pc : plo->aploOwned)
                    if (pc->oid == oid)
                        std::fprintf(stderr, "oid %d (%s) owned by %d (%s), clone %d\n", oid, g_acls[pc->Cid()].name, plo->oid,
                                     g_acls[plo->Cid()].name, plo->iclone);
        }
        if (!dumpXf.empty())
            if (FILE* pf = std::fopen(dumpXf.c_str(), "w")) { DumpXfs(psw.get(), pf); std::fclose(pf); }
        if (fDumpObjects)
        {
            FILE* pf = dumpObjects.empty() ? stdout : std::fopen(dumpObjects.c_str(), "w");
            if (pf)
            {
                DumpObjects(psw.get(), pf);
                if (pf != stdout) std::fclose(pf);
            }
        }
        if (std::getenv("SLY2N_GUARDSURVEY")) SurveyGuards(psw.get(), stdout);   // (PC debug: pcguard.h)
        return fOk ? 0 : 1;
    }
    if (!command)
    {
        // the launcher's start (--data-dir, no --world and no harness): the disc's boot sequence (logo, title,
        // menus) with the user's own cards only
        if (!fWorldGiven && !cane::dataOverride.empty() && go.slyStart.empty() && go.gangStart.empty() && go.frameDump.empty())
        {
            go.world = "boot";
            g_fDevCard = false;
        }
        // the game (default: the Paris hub): the PC options from <user-dir>/Saves/system_settings.dat
        if (!go.fHeadless)
        {
            std::string message;
            FLoadDisplaySettings(cane::userDirectory(), &message);
            std::printf("settings: %s\n", message.c_str());
            // the debug keys and the debug menu: the launcher's Settings > Advanced > Debug mode (<user-dir>/Debug
            // Mode.ini, which the launcher copies there at each start; not headless, where SLY2N_PCOPT=debugkeys=1)
            void ApplyPcDebugMode();   // pcinput.h
            ApplyPcDebugMode();
        }
        applyDisplayOverrides();
        {
            void ApplyPcInputEnv();   // pcinput.h (tests: SLY2N_PCOPT)
            ApplyPcInputEnv();
        }
        (void)fGame;
        go.screenshot = vo.screenshot;
        go.camera = vo.camera;
        go.cameraRam = vo.cameraRam;
        go.width = vo.width;
        go.height = vo.height;
        go.loadmaps = vo.loadmaps;
        for (JOYPRESS& press : go.apress) press.frame -= go.vbBase;  // (--press-vblank-base: the reference's vblanks)
        return RunGame(go);
    }
    std::fprintf(stderr,
                 "usage: Sly2-Release [--disc disc.iso | --data-dir dir] --list-keys | --dump-world VA|SECTOR:CB [--out file]\n"
                 "       | --load-world W|boot|frontend [--filter N] [--load-trace map.txt] [--dump-objects [file]]\n"
                 "       | --parse-splice W | --eval-splice W [--maps dir] [-v] | --splice-selftest | --rumble-selftest\n"
                 "       | --list-sounds W | --play-music W [--song ID] [--stem 0|1|2] | --play-sfx W [--song SOUNDID] | --play-dialog W [--song ID]\n"
                 "       | --list-movies | --play-movie ID [--out frames-dir] [--seconds T]   (W: boot, frontend, a world record 2..43, SECTOR:CB)\n"
                 "       | --convert DIR [--no-export-textures] | --export-textures [--out DIR] | --dump-textures DIR W\n");
    return 2;
}
