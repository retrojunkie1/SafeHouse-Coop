// Online co-op: the other player drawn in your world, the joiner's world, progress and guards following the host's
// (coop.h, COOP.md).

#include "coop.h"

#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "bone.h"
#include "clock.h"
#include "cls.h"
#include "damage.h"
#include "gl.h"
#include "glob.h"
#include "aihit.h"
#include "guard.h"
#include "disc.h"
#include "lo.h"
#include "pcloader.h"
#include "po.h"
#include "savedata.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "sly2_version.h"

#include <enet/enet.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>


namespace
{
// ------------------------------------------------------------ the wire format
// Little-endian, packed by hand (both ends are x86-64 / ARM64 builds of this file).
constexpr uint32_t c_magic = 0x534C5932;       // "SLY2"
constexpr uint16_t c_protocol = 3;              // 3: the world, the save, the guards, hits
constexpr uint8_t c_chanReliable = 0, c_chanState = 1, c_chanActors = 2;
constexpr size_t c_cchan = 3;
constexpr size_t c_cpeerMax = 1;                // one other player
constexpr int c_cxfMax = 512;                   // records per player pose (Sly's tree has well under 200)
constexpr int c_cxfActorsMax = 6000;            // records per guards message
constexpr int c_cactorMax = 16;                 // guards sent per message (the nearest to the joiner)
constexpr float c_sActorRadius = 25000.0f;      // and within this distance of the joiner (cm)
constexpr int c_dstepActors = 2;                // the guards sent every 2nd step (30 a second)

enum MSG : uint8_t
{
    MSG_Hello = 1,      // reliable: magic, protocol, region, version string, name
    MSG_Refuse = 2,     // reliable: a reason, then the sender disconnects
    MSG_State = 3,      // unreliable sequenced: the sender's step, world, player object and its tree
    MSG_World = 4,      // reliable, host -> joiner: the host's world (its record, point, key, flags, job and task)
    MSG_Save = 5,       // reliable, host -> joiner: the host's saved state (progress, coins, items)
    MSG_Actors = 6,     // unreliable sequenced, host -> joiner: the guards' trees
    MSG_Hit = 7,        // reliable, joiner -> host: a hit on a guard
};

struct WRITER
{
    std::vector<uint8_t> ab;
    template <class T> void Put(const T& t) { const auto* pb = reinterpret_cast<const uint8_t*>(&t); ab.insert(ab.end(), pb, pb + sizeof(T)); }
    void PutBytes(const void* pv, size_t cb) { const auto* pb = static_cast<const uint8_t*>(pv); ab.insert(ab.end(), pb, pb + cb); }
    void PutStr(const std::string& s) { Put<uint16_t>(uint16_t(std::min<size_t>(s.size(), 255))); ab.insert(ab.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 255)); }
};

struct READER
{
    const uint8_t* pb;
    size_t cb, i = 0;
    bool fOk = true;
    template <class T> T Get()
    {
        T t{};
        if (i + sizeof(T) > cb) { fOk = false; return t; }
        std::memcpy(&t, pb + i, sizeof(T));
        i += sizeof(T);
        return t;
    }
    bool GetBytes(void* pv, size_t c)
    {
        if (i + c > cb) { fOk = false; return false; }
        std::memcpy(pv, pb + i, c);
        i += c;
        return true;
    }
    std::string GetStr()
    {
        const uint16_t c = Get<uint16_t>();
        if (!fOk || i + c > cb) { fOk = false; return {}; }
        std::string s(reinterpret_cast<const char*>(pb + i), c);
        i += c;
        return s;
    }
};

// ------------------------------------------------------------ poses (a tree of objects at one step)
// A record's world matrix as position + rotation + scale (the scale kept per axis: some records squash).
struct XFPOSE
{
    glm::vec3 pos{0.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct ALOPOSE
{
    int oid = -1;
    bool fDrawn = false;        // the sender's game draws it this step (FAloDrawn: effects, gadgets and props are hidden)
    float gFade = 1.0f;
    std::vector<std::pair<int32_t, bool>> aglobShown;   // its mesh groups shown / hidden at run time (alo.h)
    std::vector<XFPOSE> axf;
};
using TREEPOSE = std::vector<ALOPOSE>;

XFPOSE XfposeFromMat(const glm::mat4& m)
{
    XFPOSE x;
    x.pos = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    x.scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
    if (x.scale.x > 1e-6f) c0 /= x.scale.x;
    if (x.scale.y > 1e-6f) c1 /= x.scale.y;
    if (x.scale.z > 1e-6f) c2 /= x.scale.z;
    x.rot = glm::normalize(glm::quat_cast(glm::mat3(c0, c1, c2)));
    return x;
}

glm::mat4 MatFromXfpose(const XFPOSE& x)
{
    glm::mat3 r = glm::mat3_cast(x.rot);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(r[0] * x.scale.x, 0.0f);
    m[1] = glm::vec4(r[1] * x.scale.y, 0.0f);
    m[2] = glm::vec4(r[2] * x.scale.z, 0.0f);
    m[3] = glm::vec4(x.pos, 1.0f);
    return m;
}

XFPOSE Lerp(const XFPOSE& a, const XFPOSE& b, float u)
{
    XFPOSE x;
    x.pos = glm::mix(a.pos, b.pos, u);
    x.rot = glm::slerp(a.rot, b.rot, u);
    x.scale = glm::mix(a.scale, b.scale, u);
    return x;
}

// An object's tree: the root and its ALO children, depth first (the order both ends walk).
void CollectTree(ALO* palo, std::vector<ALO*>& apalo)
{
    apalo.push_back(palo);
    for (LO* pc : palo->aploChild)
        if (pc && FCidDerivedFrom(pc->Cid(), 0x30)) CollectTree(static_cast<ALO*>(pc), apalo);
}

// A tree on the wire: per object its oid, drawn flag, fade, shown groups, then its records (position as 3 floats,
// rotation as 4 shorts, the scale only when not 1).
void PutTree(WRITER& w, const SW* psw, ALO* proot, int& cxfLeft)
{
    std::vector<ALO*> apalo;
    CollectTree(proot, apalo);
    w.Put<uint16_t>(uint16_t(apalo.size()));
    for (ALO* pa : apalo)
    {
        w.Put<int32_t>(pa->oid);
        w.Put<uint8_t>(psw && FAloDrawn(psw, pa) ? 1 : 0);
        w.Put<float>(pa->gFade);
        const size_t cglob = std::min<size_t>(pa->aglobShown.size(), 255);
        w.Put<uint8_t>(uint8_t(cglob));
        for (size_t g = 0; g < cglob; ++g)
        {
            w.Put<int32_t>(pa->aglobShown[g].first);
            w.Put<uint8_t>(pa->aglobShown[g].second ? 1 : 0);
        }
        const int c = std::max(0, std::min<int>(int(pa->axf.size()), cxfLeft));
        cxfLeft -= c;
        w.Put<uint16_t>(uint16_t(c));
        for (int i = 0; i < c; ++i)
        {
            const XFPOSE x = XfposeFromMat(pa->axf[size_t(i)]->matWorld);
            w.Put<float>(x.pos.x), w.Put<float>(x.pos.y), w.Put<float>(x.pos.z);
            auto q16 = [](float g) { return int16_t(std::clamp(g, -1.0f, 1.0f) * 32767.0f); };
            w.Put<int16_t>(q16(x.rot.x)), w.Put<int16_t>(q16(x.rot.y)), w.Put<int16_t>(q16(x.rot.z)), w.Put<int16_t>(q16(x.rot.w));
            const bool fScale = glm::any(glm::greaterThan(glm::abs(x.scale - glm::vec3(1.0f)), glm::vec3(1e-3f)));
            w.Put<uint8_t>(fScale ? 1 : 0);
            if (fScale) w.Put<float>(x.scale.x), w.Put<float>(x.scale.y), w.Put<float>(x.scale.z);
        }
    }
}

bool FGetTree(READER& r, TREEPOSE& tree, int& cxfLeft)
{
    const uint16_t calo = r.Get<uint16_t>();
    for (int i = 0; i < calo && r.fOk; ++i)
    {
        ALOPOSE ap;
        ap.oid = r.Get<int32_t>();
        ap.fDrawn = (r.Get<uint8_t>() & 1) != 0;
        ap.gFade = r.Get<float>();
        const uint8_t cglob = r.Get<uint8_t>();
        for (int g = 0; g < cglob && r.fOk; ++g)
        {
            const int32_t n = r.Get<int32_t>();
            ap.aglobShown.emplace_back(n, r.Get<uint8_t>() != 0);
        }
        const uint16_t c = r.Get<uint16_t>();
        cxfLeft -= c;
        if (cxfLeft < 0) return false;
        ap.axf.resize(c);
        for (XFPOSE& x : ap.axf)
        {
            x.pos = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
            const float qx = r.Get<int16_t>() / 32767.0f, qy = r.Get<int16_t>() / 32767.0f;
            const float qz = r.Get<int16_t>() / 32767.0f, qw = r.Get<int16_t>() / 32767.0f;
            x.rot = glm::normalize(glm::quat(qw, qx, qy, qz));
            if (r.Get<uint8_t>()) x.scale = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
        }
        tree.push_back(std::move(ap));
    }
    return r.fOk;
}

// A tree's pose applied to its objects (mpoid: the receiving tree's objects by oid), between a and b at u
void ApplyTree(const TREEPOSE& a, const TREEPOSE* pb, float u, const std::unordered_map<int, ALO*>& mpoid,
               std::unordered_map<const ALO*, bool>& mpfDrawn, std::vector<std::pair<XF*, glm::mat4>>* pamat = nullptr)
{
    for (size_t i = 0; i < a.size(); ++i)
    {
        const ALOPOSE& apa = a[i];
        auto it = mpoid.find(apa.oid);
        if (it == mpoid.end()) continue;
        ALO* palo = it->second;
        const ALOPOSE* apb = (pb && i < pb->size() && (*pb)[i].oid == apa.oid) ? &(*pb)[i] : nullptr;
        const size_t c = std::min(apa.axf.size(), palo->axf.size());
        for (size_t k = 0; k < c; ++k)
        {
            const XFPOSE x = (apb && k < apb->axf.size()) ? Lerp(apa.axf[k], apb->axf[k], u) : apa.axf[k];
            palo->axf[k]->matWorld = MatFromXfpose(x);
            if (pamat) pamat->emplace_back(palo->axf[k].get(), palo->axf[k]->matWorld);
        }
        // drawn, fade and mesh groups: the nearer pose's (they switch, not blend), the fade blended
        const ALOPOSE& apn = (apb && u >= 0.5f) ? *apb : apa;
        mpfDrawn[palo] = apn.fDrawn;
        palo->gFade = apb ? glm::mix(apa.gFade, apb->gFade, u) : apa.gFade;
        palo->aglobShown = apn.aglobShown;
    }
}

// ------------------------------------------------------------ state
COOPOPTIONS s_opt;
ENetHost* s_phost = nullptr;
ENetPeer* s_ppeer = nullptr;       // the other player (connected and greeted)
ENetPeer* s_ppeerPending = nullptr;
bool s_fHelloOk = false;
std::string s_nameRemote;
std::string s_status = "";
uint32_t s_stepLocal = 0;
double s_tNextRetry = 0.0;

double TNow()
{
    using C = std::chrono::steady_clock;
    static const C::time_point s_t0 = C::now();
    return std::chrono::duration<double>(C::now() - s_t0).count();
}

// The step drawn for a stream: c_dstepDelay steps behind the newest received (the jitter buffer)
constexpr double c_dstepDelay = 3.0;
double StepShown(uint32_t stepNewest, double tRecv, double dstepExtra = 0.0)
{
    return double(stepNewest) + (TNow() - tRecv) * 59.94 - c_dstepDelay - dstepExtra;
}

// --- the other player
struct POSE
{
    uint32_t step = 0;
    int iwrec = -1;
    int oidPlayer = -1;
    TREEPOSE tree;
    double tRecv = 0.0;
};
std::deque<POSE> s_aposeRemote;
struct REMOTE
{
    int iwrec = -1;
    int oidPlayer = -1;
    uint32_t serialWorld = 0;          // the running world it was bound in
    ALO* proot = nullptr;              // the kept world's copy of the remote player's object
    std::vector<ALO*> apaloTree;       // its tree (CollectTree order)
    std::vector<ALO*> apaloDraw;       // the ones with meshes
    std::unordered_map<int, ALO*> mpoid;
    bool fShown = false;
} s_remote;
bool s_fSceneRebuild = false;
std::unordered_map<const ALO*, bool> s_mpfDrawnRemote;

// --- the host's world (joiner)
struct WORLDMSG
{
    bool fValid = false;
    int iwrec = -1;
    uint8_t abChk[64]{};
    OID oidPoint = -1, oidKey = -1;
    int flags = 0;
    OID oidJob = -1, oidTask = -1;
};
WORLDMSG s_worldHost;                  // joiner: the host's world, as last told
WORLDMSG s_worldSent;                  // host: the one last sent
uint32_t s_stepNextFollow = 0;         // joiner: the next step a world change may be asked for
int s_cFollowTries = 0;                // joiner: the changes asked for since the host's last world message (at most 3)
GAMESAVE s_gamesaveHost;               // joiner: the host's save, as last received
bool s_fHaveSaveHost = false;
uint64_t s_hashSaveSent = 0;           // host: the save last sent

// --- the guards (joiner: the host's, mirrored)
struct ACTORSNAP
{
    uint32_t step = 0;
    int iwrec = -1;
    double tRecv = 0.0;
    std::unordered_map<uint64_t, TREEPOSE> mpkey;   // per guard: (root oid, ordinal among the roots with that oid)
};
std::deque<ACTORSNAP> s_asnap;
struct MIRROR
{
    uint32_t serialWorld = 0;
    uint32_t stepBuilt = 0;
    std::unordered_map<uint64_t, ALO*> mpkeyRoot;              // the joiner's guards by key
    std::unordered_map<const ALO*, std::unordered_map<int, ALO*>> mpRootOid;   // each one's tree by oid
    std::unordered_set<const LO*> setFrozen;                   // every object of the guards' trees (no update)
    std::unordered_map<const ALO*, bool> mpfDrawn;             // per object: drawn in the pose shown
    std::unordered_map<const ALO*, uint64_t> mpRootKey;
    std::vector<std::pair<XF*, glm::mat4>> amat;            // the records posed this step (written again after the
                                                               // game rebuilds the worlds from the locals: CoopAfterWorlds)
    bool fActive = false;
} s_mirror;

int NRegion() { return static_cast<int>(g_disc.Region()); }
bool FConnected() { return s_ppeer && s_fHelloOk; }

std::string SzDefaultName()
{
    for (const char* sz : {"SLY2_COOP_NAME", "USERNAME", "USER"})
        if (const char* v = std::getenv(sz); v && *v) return v;
    return "Player";
}

uint64_t HashBytes(const uint8_t* pb, size_t cb)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < cb; ++i) h = (h ^ pb[i]) * 1099511628211ull;
    return h;
}

void Send(ENetPeer* ppeer, const WRITER& w, uint8_t chan, bool fReliable)
{
    if (!ppeer) return;
    ENetPacket* pkt = enet_packet_create(w.ab.data(), w.ab.size(), fReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(ppeer, chan, pkt);
}

void SendHello(ENetPeer* ppeer)
{
    WRITER w;
    w.Put<uint8_t>(MSG_Hello);
    w.Put<uint32_t>(c_magic);
    w.Put<uint16_t>(c_protocol);
    w.Put<int32_t>(NRegion());
    w.PutStr(SLY2_PORT_VERSION);
    w.PutStr(s_opt.name);
    Send(ppeer, w, c_chanReliable, true);
}

void Refuse(ENetPeer* ppeer, const std::string& reason)
{
    WRITER w;
    w.Put<uint8_t>(MSG_Refuse);
    w.PutStr(reason);
    Send(ppeer, w, c_chanReliable, true);
    enet_host_flush(s_phost);
    enet_peer_disconnect_later(ppeer, 0);
    std::printf("co-op: refused the other player: %s\n", reason.c_str());
}

void ResetRemote()
{
    if (!s_remote.apaloDraw.empty()) s_fSceneRebuild = true;
    s_remote = REMOTE{};
}

void ResetMirror()
{
    s_mirror = MIRROR{};
    s_asnap.clear();
}

// ------------------------------------------------------------ host: the world, the save, the guards
WORLDMSG WorldmsgCur()
{
    WORLDMSG m;
    m.fValid = g_transition.iwrecCur >= 0;
    m.iwrec = g_transition.iwrecCur;
    std::memcpy(m.abChk, g_transition.fHaveCur ? g_transition.abChkCur : g_transition.req.abChk, 64);
    m.oidPoint = g_transition.oidPointCur;
    m.oidKey = g_transition.oidKeyCur;
    m.flags = g_transition.req.flags & ~2;
    m.oidJob = g_taskmgr.oidJob;
    m.oidTask = g_taskmgr.oidTask;
    return m;
}

bool FSameWorldmsg(const WORLDMSG& a, const WORLDMSG& b)
{
    return a.fValid == b.fValid && a.iwrec == b.iwrec && !std::memcmp(a.abChk, b.abChk, 64) && a.oidJob == b.oidJob && a.oidTask == b.oidTask;
}

void SendSave(bool fForce)
{
    const uint64_t h = HashBytes(g_gamesave.ab, sizeof(g_gamesave.ab));
    if (!fForce && h == s_hashSaveSent) return;
    s_hashSaveSent = h;
    WRITER w;
    w.Put<uint8_t>(MSG_Save);
    w.PutBytes(g_gamesave.ab, sizeof(g_gamesave.ab));
    Send(s_ppeer, w, c_chanReliable, true);
}

void SendWorld(bool fForce)
{
    if (g_transition.fPending || g_transition.iwrecCur < 0) return;
    const WORLDMSG m = WorldmsgCur();
    if (!fForce && FSameWorldmsg(m, s_worldSent)) return;
    SendSave(true);   // (the progress first: the joiner's load reads it)
    s_worldSent = m;
    WRITER w;
    w.Put<uint8_t>(MSG_World);
    w.Put<int32_t>(m.iwrec);
    w.PutBytes(m.abChk, 64);
    w.Put<int32_t>(m.oidPoint), w.Put<int32_t>(m.oidKey), w.Put<int32_t>(m.flags);
    w.Put<int32_t>(m.oidJob), w.Put<int32_t>(m.oidTask);
    Send(s_ppeer, w, c_chanReliable, true);
    std::printf("co-op: world %d sent (job %d, task %d)\n", m.iwrec, m.oidJob, m.oidTask);
}

// The world's guards and NPCs (class 0x38 and its classes) whose parent is not one, each with its key: its oid and
// its ordinal among those roots with the same oid (proxied copies share an oid), in aploAll order.
// A pooled guard (an instance of a world group, handed to the streaming proxy nearest a player) is keyed by its
// proxy and its place in the group: the two games attach different instances to a proxy (the players stand in
// different places), so the instance's own oid names different guards on the two sides.
void CollectGuardRoots(SW* psw, std::vector<std::pair<uint64_t, ALO*>>& a)
{
    std::unordered_map<const LO*, uint64_t> mpinst;
    std::unordered_set<const LO*> setPooled;
    for (const WE& we : psw->awe)
    {
        if (we.cloLoaded <= 0) continue;
        for (const LO* plo : we.aplo) setPooled.insert(plo);
        for (size_t irow = 0; irow < we.aploRowOwner.size(); ++irow)
        {
            const LO* pproxy = we.aploRowOwner[irow];
            if (!pproxy) continue;
            for (int j = 0; j < we.cloLoaded; ++j)
            {
                const size_t i = irow * size_t(we.cloLoaded) + size_t(j);
                if (i < we.aplo.size() && we.aplo[i])
                    mpinst[we.aplo[i]] = (1ull << 63) | (uint64_t(uint32_t(pproxy->oid)) << 20) | uint64_t(j & 0xFFFFF);
            }
        }
    }
    std::unordered_map<int, uint32_t> mpc;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo || !FCidDerivedFrom(plo->Cid(), 0x38)) continue;
        if (plo->paloParent && FCidDerivedFrom(plo->paloParent->Cid(), 0x38)) continue;
        if (auto it = mpinst.find(plo); it != mpinst.end())
        {
            a.emplace_back(it->second, static_cast<ALO*>(plo));
            continue;
        }
        if (setPooled.count(plo)) continue;   // (a spare instance: no proxy, out of the world)
        const uint32_t ord = mpc[plo->oid]++;
        a.emplace_back((uint64_t(uint32_t(plo->oid)) << 32) | ord, static_cast<ALO*>(plo));
    }
}

void SendActors(SW* psw)
{
    if (!psw || g_transition.fPending || s_stepLocal % c_dstepActors) return;
    // the joiner's position: its newest pose's root (else the host's player)
    glm::vec3 posJoin(0.0f);
    bool fPos = false;
    if (!s_aposeRemote.empty() && !s_aposeRemote.back().tree.empty() && !s_aposeRemote.back().tree[0].axf.empty())
        posJoin = s_aposeRemote.back().tree[0].axf[0].pos, fPos = true;
    else if (LO* plo = PloPoCurrent(psw); plo && FCidDerivedFrom(plo->Cid(), 0x30) && !static_cast<ALO*>(plo)->axf.empty())
        posJoin = glm::vec3(static_cast<ALO*>(plo)->axf[0]->matWorld[3]), fPos = true;
    std::vector<std::pair<uint64_t, ALO*>> aroot;
    CollectGuardRoots(psw, aroot);
    std::vector<std::pair<float, std::pair<uint64_t, ALO*>>> asel;
    for (const auto& kr : aroot)
    {
        ALO* pa = kr.second;
        if (!pa->fInWorld || pa->axf.empty()) continue;
        const float d = fPos ? glm::distance(glm::vec3(pa->axf[0]->matWorld[3]), posJoin) : 0.0f;
        if (d > c_sActorRadius) continue;
        asel.push_back({d, kr});
    }
    std::sort(asel.begin(), asel.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (asel.size() > size_t(c_cactorMax)) asel.resize(c_cactorMax);
    WRITER w;
    w.Put<uint8_t>(MSG_Actors);
    w.Put<uint32_t>(s_stepLocal);
    w.Put<int32_t>(g_transition.iwrecCur);
    w.Put<uint16_t>(uint16_t(asel.size()));
    int cxfLeft = c_cxfActorsMax;
    for (const auto& s : asel)
    {
        w.Put<uint64_t>(s.second.first);
        PutTree(w, psw, s.second.second, cxfLeft);
    }
    Send(s_ppeer, w, c_chanActors, false);
    static uint32_t s_cLogged = 0;
    if (s_cLogged < 3 && !asel.empty()) { ++s_cLogged; std::printf("co-op: guards message: %zu guards, %zu bytes\n", asel.size(), w.ab.size()); }
}

void OnHit(READER& r)
{
    const uint64_t key = r.Get<uint64_t>();
    CTMSG ctm;
    ctm.grfFilter = r.Get<uint32_t>();
    ctm.nState = r.Get<int32_t>();
    ctm.pos = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
    ctm.vec = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
    ctm.grfAccepted = r.Get<uint32_t>();
    if (!r.fOk || !g_psw || g_transition.fPending) return;
    std::vector<std::pair<uint64_t, ALO*>> aroot;
    CollectGuardRoots(g_psw, aroot);
    for (const auto& kr : aroot)
        if (kr.first == key)
        {
            // (the sender: the host's own player stands in for the joiner's character; the hit's position and
            // direction are the joiner's)
            ctm.ploSender = PloPoCurrent(g_psw);
            ctm.ploReceiver = kr.second;
            ctm.paloAccepted = kr.second;
            ctm.t = g_clock.t;
            const bool f = FCoopGuardAcceptContact(kr.second, &ctm);
            std::printf("co-op: %s hit guard %d (filter 0x%X): %s\n", s_nameRemote.c_str(), kr.second->oid, ctm.grfFilter, f ? "taken" : "ignored");
            return;
        }
}

// ------------------------------------------------------------ joiner: following the host
void OnWorld(READER& r)
{
    WORLDMSG m;
    m.fValid = true;
    m.iwrec = r.Get<int32_t>();
    r.GetBytes(m.abChk, 64);
    m.oidPoint = r.Get<int32_t>(), m.oidKey = r.Get<int32_t>(), m.flags = r.Get<int32_t>();
    m.oidJob = r.Get<int32_t>(), m.oidTask = r.Get<int32_t>();
    if (!r.fOk) return;
    s_worldHost = m;
    s_stepNextFollow = 0;
    s_cFollowTries = 0;
    std::printf("co-op: the host is in world %d (job %d, task %d)\n", m.iwrec, m.oidJob, m.oidTask);
}

void OnSave(READER& r)
{
    if (!r.GetBytes(s_gamesaveHost.ab, sizeof(s_gamesaveHost.ab))) return;
    s_fHaveSaveHost = true;
    // the host's progress taken now, the joiner's own player's health kept (the save holds the player's counter)
    ACTOR* pa = g_psw ? PactorFromLo(PloPoCurrent(g_psw)) : nullptr;
    int* pn = pa ? DmgCounterOf(pa) : nullptr;
    const bool fKeep = pn && reinterpret_cast<uint8_t*>(pn) >= g_gamesave.ab && reinterpret_cast<uint8_t*>(pn) < g_gamesave.ab + sizeof(g_gamesave.ab);
    const int nHp = fKeep ? *pn : 0;
    std::memcpy(g_gamesave.ab, s_gamesaveHost.ab, sizeof(g_gamesave.ab));
    if (fKeep) *pn = nHp;
}

// each step: the joiner's world made the host's (its record and job)
void FollowHost(SW* psw)
{
    if (s_opt.mode != COOPMODE_Join || !FConnected() || !s_worldHost.fValid || !psw) return;
    if (g_transition.fPending || s_stepLocal < s_stepNextFollow) return;
    if (s_cFollowTries >= 3) return;   // (it did not take: wait for the host's next world)
    const bool fWorld = g_transition.iwrecCur != s_worldHost.iwrec;
    const bool fTask = !fWorld && s_worldHost.oidTask >= 0 && g_taskmgr.oidTask != s_worldHost.oidTask;
    if (!fWorld && !fTask) return;
    s_stepNextFollow = s_stepLocal + 300;   // (once every 5 s while it differs)
    ++s_cFollowTries;
    if (s_fHaveSaveHost) std::memcpy(g_gamesave.ab, s_gamesaveHost.ab, sizeof(g_gamesave.ab));
    if (s_worldHost.oidTask >= 0)
    {
        std::printf("co-op: following the host to world %d, task %d\n", s_worldHost.iwrec, s_worldHost.oidTask);
        RequestTaskStartByOid(psw, s_worldHost.abChk, s_worldHost.oidTask);
    }
    else
    {
        std::printf("co-op: following the host to world %d\n", s_worldHost.iwrec);
        RequestTransition(psw, s_worldHost.abChk, s_worldHost.oidPoint >= 0 ? s_worldHost.oidPoint : 0x189,
                          s_worldHost.oidKey, s_worldHost.flags & ~(2 | 8), 0x30A);
    }
}

void OnActors(READER& r)
{
    ACTORSNAP snap;
    snap.step = r.Get<uint32_t>();
    snap.iwrec = r.Get<int32_t>();
    const uint16_t c = r.Get<uint16_t>();
    int cxfLeft = c_cxfActorsMax;
    for (int i = 0; i < c && r.fOk; ++i)
    {
        const uint64_t key = r.Get<uint64_t>();
        TREEPOSE tree;
        if (!FGetTree(r, tree, cxfLeft)) return;
        snap.mpkey.emplace(key, std::move(tree));
    }
    if (!r.fOk) return;
    snap.tRecv = TNow();
    if (!s_asnap.empty() && snap.step <= s_asnap.back().step) return;
    s_asnap.push_back(std::move(snap));
    while (s_asnap.size() > 12) s_asnap.pop_front();
}

// the joiner's guards found and frozen (their updates skipped while the host's are shown)
void BuildMirror(SW* psw)
{
    s_mirror.serialWorld = psw->serial;
    s_mirror.stepBuilt = s_stepLocal;
    s_mirror.mpkeyRoot.clear();
    s_mirror.mpRootOid.clear();
    s_mirror.setFrozen.clear();
    s_mirror.mpRootKey.clear();
    std::vector<std::pair<uint64_t, ALO*>> aroot;
    CollectGuardRoots(psw, aroot);
    for (const auto& kr : aroot)
    {
        s_mirror.mpkeyRoot[kr.first] = kr.second;
        s_mirror.mpRootKey[kr.second] = kr.first;
        std::vector<ALO*> apalo;
        CollectTree(kr.second, apalo);
        auto& mp = s_mirror.mpRootOid[kr.second];
        for (ALO* pa : apalo)
        {
            mp.emplace(pa->oid, pa);
            s_mirror.setFrozen.insert(pa);
        }
    }
}

void StepMirror(SW* psw)
{
    s_mirror.mpfDrawn.clear();
    s_mirror.amat.clear();
    s_mirror.fActive = false;
    if (s_opt.mode != COOPMODE_Join || !FConnected() || !psw || g_transition.fPending || s_asnap.empty()) return;
    const ACTORSNAP& newest = s_asnap.back();
    if (newest.iwrec != g_transition.iwrecCur || TNow() - newest.tRecv > 1.5) return;   // (another world, or gone quiet)
    if (s_mirror.serialWorld != psw->serial || s_stepLocal - s_mirror.stepBuilt >= 60) BuildMirror(psw);
    s_mirror.fActive = true;
    const double stepNow = StepShown(newest.step, newest.tRecv, c_dstepActors);
    const ACTORSNAP* pa = &newest;
    const ACTORSNAP* pb = nullptr;
    for (size_t i = 0; i + 1 < s_asnap.size(); ++i)
        if (double(s_asnap[i].step) <= stepNow && double(s_asnap[i + 1].step) >= stepNow)
        {
            pa = &s_asnap[i];
            pb = &s_asnap[i + 1];
            break;
        }
    if (pb && pb->iwrec != pa->iwrec) pb = nullptr;
    const float u = pb ? std::clamp(float((stepNow - double(pa->step)) / std::max(1.0, double(pb->step) - double(pa->step))), 0.0f, 1.0f) : 0.0f;
    for (const auto& [key, tree] : pa->mpkey)
    {
        auto itRoot = s_mirror.mpkeyRoot.find(key);
        if (itRoot == s_mirror.mpkeyRoot.end()) continue;
        const TREEPOSE* ptb = nullptr;
        if (pb)
            if (auto itb = pb->mpkey.find(key); itb != pb->mpkey.end()) ptb = &itb->second;
        ApplyTree(tree, ptb, u, s_mirror.mpRootOid[itRoot->second], s_mirror.mpfDrawn, &s_mirror.amat);
    }
    // the guards the host did not send (out of range, or out of its world): hidden
    for (const auto& [key, palo] : s_mirror.mpkeyRoot)
        if (!pa->mpkey.count(key))
            for (const auto& [oid, pc] : s_mirror.mpRootOid[palo]) s_mirror.mpfDrawn[pc] = false;
}

// ------------------------------------------------------------ receiving
void OnHello(ENetPeer* ppeer, READER& r)
{
    const uint32_t magic = r.Get<uint32_t>();
    const uint16_t protocol = r.Get<uint16_t>();
    const int region = r.Get<int32_t>();
    const std::string version = r.GetStr();
    const std::string name = r.GetStr();
    if (!r.fOk || magic != c_magic) { Refuse(ppeer, "not a Sly 2 co-op game"); return; }
    if (protocol != c_protocol || version != SLY2_PORT_VERSION)
    {
        Refuse(ppeer, "different SafeHouse-Coop versions (" + version + " and " + SLY2_PORT_VERSION + ")");
        return;
    }
    if (region != NRegion()) { Refuse(ppeer, "different disc regions: both players need the same region of Sly 2"); return; }
    s_ppeer = ppeer;
    s_fHelloOk = true;
    s_nameRemote = name.empty() ? "Player 2" : name;
    s_aposeRemote.clear();
    ResetRemote();
    ResetMirror();
    s_worldHost = WORLDMSG{};
    s_worldSent = WORLDMSG{};
    s_hashSaveSent = 0;
    s_fHaveSaveHost = false;
    std::printf("co-op: %s connected\n", s_nameRemote.c_str());
    if (s_opt.mode == COOPMODE_Host) SendWorld(true);
}

void OnState(READER& r)
{
    POSE pose;
    pose.step = r.Get<uint32_t>();
    pose.iwrec = r.Get<int32_t>();
    pose.oidPlayer = r.Get<int32_t>();
    int cxfLeft = c_cxfMax;
    if (!FGetTree(r, pose.tree, cxfLeft)) return;
    pose.tRecv = TNow();
    // (unreliable but sequenced: ENet drops late packets, so steps arrive in order)
    if (!s_aposeRemote.empty() && pose.step <= s_aposeRemote.back().step) return;
    s_aposeRemote.push_back(std::move(pose));
    while (s_aposeRemote.size() > 16) s_aposeRemote.pop_front();
}

void OnReceive(ENetPeer* ppeer, const ENetPacket* pkt)
{
    READER r{pkt->data, pkt->dataLength};
    const uint8_t msg = r.Get<uint8_t>();
    if (msg == MSG_Hello) { OnHello(ppeer, r); return; }
    if (msg == MSG_Refuse)
    {
        const std::string reason = r.GetStr();
        std::printf("co-op: the other player refused: %s\n", reason.c_str());
        s_status = "co-op: refused: " + reason;
        return;
    }
    if (ppeer != s_ppeer || !s_fHelloOk) return;
    const bool fJoin = s_opt.mode == COOPMODE_Join;
    switch (msg)
    {
    case MSG_State: OnState(r); break;
    case MSG_World: if (fJoin) OnWorld(r); break;
    case MSG_Save: if (fJoin) OnSave(r); break;
    case MSG_Actors: if (fJoin) OnActors(r); break;
    case MSG_Hit: if (!fJoin) OnHit(r); break;
    default: break;
    }
}

void Connect()
{
    ENetAddress addr{};
    if (enet_address_set_host(&addr, s_opt.host.c_str()) != 0)
    {
        std::printf("co-op: cannot resolve %s\n", s_opt.host.c_str());
        s_status = "co-op: cannot find " + s_opt.host;
        s_tNextRetry = TNow() + 5.0;
        return;
    }
    addr.port = s_opt.port;
    s_ppeerPending = enet_host_connect(s_phost, &addr, c_cchan, 0);
    s_status = "co-op: connecting to " + s_opt.host + ":" + std::to_string(s_opt.port);
    std::printf("%s\n", s_status.c_str());
}

void Service()
{
    if (!s_phost) return;
    if (s_opt.mode == COOPMODE_Join && !s_ppeer && !s_ppeerPending && TNow() >= s_tNextRetry) Connect();
    ENetEvent ev;
    while (enet_host_service(s_phost, &ev, 0) > 0)
    {
        switch (ev.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            if (s_ppeer && ev.peer != s_ppeer) { Refuse(ev.peer, "this game already has a co-op partner"); break; }
            if (ev.peer == s_ppeerPending) s_ppeerPending = nullptr;
            enet_peer_timeout(ev.peer, 0, 5000, 10000);
            SendHello(ev.peer);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            OnReceive(ev.peer, ev.packet);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (ev.peer == s_ppeer)
            {
                std::printf("co-op: %s left\n", s_nameRemote.c_str());
                s_ppeer = nullptr;
                s_fHelloOk = false;
                s_aposeRemote.clear();
                ResetRemote();
                ResetMirror();
            }
            if (ev.peer == s_ppeerPending) s_ppeerPending = nullptr;
            if (s_opt.mode == COOPMODE_Join) s_tNextRetry = TNow() + 2.0;
            break;
        default: break;
        }
    }
}

// ------------------------------------------------------------ sending our pose
void SendState(SW* psw)
{
    if (!FConnected()) return;
    WRITER w;
    w.Put<uint8_t>(MSG_State);
    w.Put<uint32_t>(s_stepLocal);
    w.Put<int32_t>(g_transition.iwrecCur);
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    const bool fPlayer = plo && FCidDerivedFrom(plo->Cid(), 0x30) && !g_transition.fPending;
    if (!fPlayer)
    {
        w.Put<int32_t>(-1);
        w.Put<uint16_t>(0);
        Send(s_ppeer, w, c_chanState, false);
        return;
    }
    w.Put<int32_t>(plo->oid);
    int cxfLeft = c_cxfMax;
    PutTree(w, psw, static_cast<ALO*>(plo), cxfLeft);
    Send(s_ppeer, w, c_chanState, false);
}

// ------------------------------------------------------------ drawing the other player
bool FBindRemote(SW* psw, int iwrec, int oid)
{
    if (s_remote.proot && s_remote.iwrec == iwrec && s_remote.oidPlayer == oid && s_remote.serialWorld == psw->serial) return true;
    if (s_remote.proot) ResetRemote();
    RequestPcKeptWorld(iwrec);
    if (PckwsKeptWorld(iwrec) != PCKWS_Ready) return false;
    LO* plo = PloKeptFind(iwrec, oid, 0x30);
    if (!plo) return false;
    s_remote.iwrec = iwrec;
    s_remote.oidPlayer = oid;
    s_remote.serialWorld = psw->serial;
    s_remote.proot = static_cast<ALO*>(plo);
    CollectTree(s_remote.proot, s_remote.apaloTree);
    for (ALO* palo : s_remote.apaloTree)
    {
        s_remote.mpoid.emplace(palo->oid, palo);
        if (palo->pglobset && !palo->pglobset->aglob.empty()) s_remote.apaloDraw.push_back(palo);
        if (const auto* paabind = PaabindKept(palo)) SetAloBoneBindings(palo, *paabind);
    }
    s_fSceneRebuild = true;
    std::printf("co-op: drawing %s as object %d of world %d (%zu objects, %zu drawn)\n", s_nameRemote.c_str(), oid, iwrec,
                s_remote.apaloTree.size(), s_remote.apaloDraw.size());
    return true;
}

void StepRemote(SW* psw)
{
    s_remote.fShown = false;
    s_mpfDrawnRemote.clear();
    if (!psw || s_aposeRemote.empty()) return;
    const POSE& newest = s_aposeRemote.back();
    // only drawn in the same world, and not while either side changes worlds
    if (newest.iwrec < 0 || newest.oidPlayer < 0 || newest.iwrec != g_transition.iwrecCur || g_transition.fPending) return;
    if (!FBindRemote(psw, newest.iwrec, newest.oidPlayer)) return;
    const double stepNow = StepShown(newest.step, newest.tRecv);
    const POSE* pa = &s_aposeRemote.front();
    const POSE* pb = nullptr;
    for (size_t i = 0; i + 1 < s_aposeRemote.size(); ++i)
        if (double(s_aposeRemote[i].step) <= stepNow && double(s_aposeRemote[i + 1].step) >= stepNow)
        {
            pa = &s_aposeRemote[i];
            pb = &s_aposeRemote[i + 1];
            break;
        }
    if (!pb && stepNow > double(newest.step)) pa = &newest;   // (late: hold the newest)
    if (pa->iwrec != s_remote.iwrec || pa->oidPlayer != s_remote.oidPlayer) pa = &newest, pb = nullptr;
    if (pb && (pb->iwrec != pa->iwrec || pb->oidPlayer != pa->oidPlayer)) pb = nullptr;
    const float u = pb ? float((stepNow - double(pa->step)) / std::max(1.0, double(pb->step) - double(pa->step))) : 0.0f;
    ApplyTree(pa->tree, pb ? &pb->tree : nullptr, std::clamp(u, 0.0f, 1.0f), s_remote.mpoid, s_mpfDrawnRemote);
    s_remote.fShown = true;
}

struct INITCOOP
{
    INITCOOP()
    {
        PCWORLDHOOKS h;
        h.pfnStarted = CoopOnWorldStarted;
        h.pfnStep = [](SW* psw, bool) { CoopStep(psw); };
        RegisterPcWorldHooks(h);
    }
} s_initcoop;
}

// ------------------------------------------------------------ the interface
bool FCoopParseArg(int argc, char** argv, int& i)
{
    const char* sz = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : nullptr; };
    if (!std::strcmp(sz, "--coop-host"))
    {
        s_opt.mode = COOPMODE_Host;
        if (const char* p = next()) s_opt.port = uint16_t(std::atoi(p));
        return true;
    }
    if (!std::strcmp(sz, "--coop-join"))
    {
        s_opt.mode = COOPMODE_Join;
        if (const char* p = next())
        {
            std::string s = p;
            const size_t ic = s.rfind(':');
            if (ic != std::string::npos && s.find(':') == ic) { s_opt.port = uint16_t(std::atoi(s.c_str() + ic + 1)); s.resize(ic); }
            s_opt.host = s;
        }
        return true;
    }
    if (!std::strcmp(sz, "--coop-name"))
    {
        if (const char* p = next()) s_opt.name = p;
        return true;
    }
    return false;
}

void CoopInit()
{
    if (s_opt.mode == COOPMODE_Off)
    {
        if (const char* v = std::getenv("SLY2_COOP_HOST"))
        {
            s_opt.mode = COOPMODE_Host;
            if (*v) s_opt.port = uint16_t(std::atoi(v));
        }
        else if (const char* v2 = std::getenv("SLY2_COOP_JOIN"); v2 && *v2)
        {
            char* argv[] = {const_cast<char*>("--coop-join"), const_cast<char*>(v2)};
            int i = 0;
            FCoopParseArg(2, argv, i);
        }
    }
    if (s_opt.mode == COOPMODE_Off) return;
    if (s_opt.name.empty()) s_opt.name = SzDefaultName();
    if (s_opt.mode == COOPMODE_Join && s_opt.host.empty())
    {
        std::printf("co-op: --coop-join needs the host's address\n");
        s_opt.mode = COOPMODE_Off;
        return;
    }
    if (enet_initialize() != 0)
    {
        std::printf("co-op: networking failed to start\n");
        s_opt.mode = COOPMODE_Off;
        return;
    }
    if (s_opt.mode == COOPMODE_Host)
    {
        ENetAddress addr{};
        addr.host = ENET_HOST_ANY;
        addr.port = s_opt.port;
        s_phost = enet_host_create(&addr, c_cpeerMax + 1, c_cchan, 0, 0);
        s_status = s_phost ? "co-op: waiting for a player on port " + std::to_string(s_opt.port)
                           : "co-op: port " + std::to_string(s_opt.port) + " is in use";
    }
    else
    {
        s_phost = enet_host_create(nullptr, 1, c_cchan, 0, 0);
        if (s_phost) Connect();
        else s_status = "co-op: networking failed to start";
    }
    std::printf("%s (as %s)\n", s_status.c_str(), s_opt.name.c_str());
    if (!s_phost) s_opt.mode = COOPMODE_Off;
}

void CoopShutdown()
{
    if (!s_phost) return;
    if (s_ppeer) enet_peer_disconnect_now(s_ppeer, 0);
    enet_host_flush(s_phost);
    enet_host_destroy(s_phost);
    s_phost = nullptr;
    enet_deinitialize();
}

bool FCoopActive() { return s_opt.mode != COOPMODE_Off; }

void CoopStep(SW* psw)
{
    if (!FCoopActive()) return;
    ++s_stepLocal;
    Service();
    if (FPcKeptWorldsLoading()) PumpPcKeptWorlds(psw);   // (the debug menu pumps them only with Debug keys on)
    if (FConnected())
    {
        if (s_opt.mode == COOPMODE_Host)
        {
            SendWorld(false);
            if (s_stepLocal % 30 == 0) SendSave(false);
            SendActors(psw);
        }
        else FollowHost(psw);
    }
    SendState(psw);
    StepRemote(psw);
    StepMirror(psw);
    if (s_phost) enet_host_flush(s_phost);
    // the status in the window's title (twice a second)
    if (g_gl.window && s_stepLocal % 30 == 0)
    {
        const std::string title = "Sly 2: Band of Thieves  |  " + SzCoopStatus();
        glfwSetWindowTitle(g_gl.window, title.c_str());
    }
}

void CoopOnWorldStarted(SW*)
{
    if (!FCoopActive()) return;
    // (a world's start clears the bone table and frees the running world's objects: bind again on the next step)
    ResetRemote();
    ResetMirror();
}

void CoopAfterWorlds()
{
    if (!s_mirror.fActive) return;
    for (const auto& [pxf, m] : s_mirror.amat)
    {
        pxf->matWorld = m;
        // a guard's root record placed in the world (not relative to another): its local is the world, so its body
        // (collision, activation) stands where the host's is, not where it froze
        if (pxf->palo && !pxf->palo->paloParent && !pxf->pxfRef && !pxf->palo->axf.empty() && pxf->palo->axf[0].get() == pxf)
            pxf->mat = m;
    }
}

bool FCoopSkipUpdate(const LO* plo)
{
    return s_mirror.fActive && plo && s_mirror.setFrozen.count(plo) != 0;
}

bool FCoopForwardGuardHit(ALO* palo, const CTMSG* pctm)
{
    if (s_opt.mode != COOPMODE_Join || !s_mirror.fActive || !palo || !pctm) return false;
    ALO* proot = palo;
    while (proot->paloParent && FCidDerivedFrom(proot->paloParent->Cid(), 0x38)) proot = proot->paloParent;
    auto it = s_mirror.mpRootKey.find(proot);
    if (it == s_mirror.mpRootKey.end()) return false;
    // a frozen guard takes nothing here; only the joiner's own attacks (its character's tree, or what it threw)
    // that would knock a guard about go to the host, at most 4 a second per guard (bumping into one is not a hit)
    const LO* ploPlayer = g_psw ? PloPoCurrent(g_psw) : nullptr;
    const LO* ploSender = pctm->ploSender;
    while (ploSender && ploSender->paloParent) ploSender = ploSender->paloParent;
    GUARD* pg = PguardFromLo(palo);
    const bool fMatters = pg && (FGuardHitMatters(pg, pctm) || pctm->grfAccepted != 0);
    if (!ploPlayer || ploSender != ploPlayer || !fMatters) return true;
    static std::unordered_map<uint64_t, double> s_mptLast;
    double& tLast = s_mptLast[it->second];
    if (TNow() - tLast < 0.25) return true;
    tLast = TNow();
    WRITER w;
    w.Put<uint8_t>(MSG_Hit);
    w.Put<uint64_t>(it->second);
    w.Put<uint32_t>(pctm->grfFilter);
    w.Put<int32_t>(pctm->nState);
    w.Put<float>(pctm->pos.x), w.Put<float>(pctm->pos.y), w.Put<float>(pctm->pos.z);
    w.Put<float>(pctm->vec.x), w.Put<float>(pctm->vec.y), w.Put<float>(pctm->vec.z);
    w.Put<uint32_t>(pctm->grfAccepted);
    Send(s_ppeer, w, c_chanReliable, true);
    return true;
}

void CoopAppendSceneAlos(std::vector<ALO*>& apalo)
{
    for (ALO* palo : s_remote.apaloDraw) apalo.push_back(palo);
}

int NCoopDrawOverride(const ALO* palo)
{
    if (s_mirror.fActive)
        if (auto it = s_mirror.mpfDrawn.find(palo); it != s_mirror.mpfDrawn.end()) return it->second ? 1 : 0;
    if (s_remote.apaloDraw.empty()) return -1;
    if (std::find(s_remote.apaloDraw.begin(), s_remote.apaloDraw.end(), palo) == s_remote.apaloDraw.end()) return -1;
    if (!s_remote.fShown) return 0;
    const auto it = s_mpfDrawnRemote.find(palo);
    return it != s_mpfDrawnRemote.end() && it->second ? 1 : 0;
}

bool FCoopTakeSceneRebuild()
{
    const bool f = s_fSceneRebuild;
    s_fSceneRebuild = false;
    return f;
}

std::string SzCoopStatus()
{
    if (!FCoopActive()) return {};
    if (FConnected())
        return "co-op: " + s_nameRemote + " connected, " + std::to_string(s_ppeer->roundTripTime) + " ms";
    return s_status;
}
