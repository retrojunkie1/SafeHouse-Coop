// Online co-op, milestone 1: the other player drawn in your world (coop.h, COOP.md).

#include "coop.h"

#include "alo.h"
#include "bone.h"
#include "cls.h"
#include "gl.h"
#include "glob.h"
#include "disc.h"
#include "lo.h"
#include "pcloader.h"
#include "po.h"
#include "sw.h"
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


namespace
{
// ------------------------------------------------------------ the wire format
// Little-endian, packed by hand (both ends are x86-64 / ARM64 builds of this file).
constexpr uint32_t c_magic = 0x534C5932;       // "SLY2"
constexpr uint16_t c_protocol = 1;
constexpr uint8_t c_chanReliable = 0, c_chanState = 1;
constexpr size_t c_cpeerMax = 1;                // milestone 1: one other player
constexpr int c_cxfMax = 512;                   // records per pose (Sly's tree has well under 200)

enum MSG : uint8_t
{
    MSG_Hello = 1,      // reliable: magic, protocol, region, version string, name
    MSG_Refuse = 2,     // reliable: a reason, then the sender disconnects
    MSG_State = 3,      // unreliable sequenced: the sender's step, world, player object and its records' world matrices
};

struct WRITER
{
    std::vector<uint8_t> ab;
    template <class T> void Put(const T& t) { const auto* pb = reinterpret_cast<const uint8_t*>(&t); ab.insert(ab.end(), pb, pb + sizeof(T)); }
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
    std::string GetStr()
    {
        const uint16_t c = Get<uint16_t>();
        if (!fOk || i + c > cb) { fOk = false; return {}; }
        std::string s(reinterpret_cast<const char*>(pb + i), c);
        i += c;
        return s;
    }
};

// ------------------------------------------------------------ a pose (one step of a player)
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
    std::vector<XFPOSE> axf;
};

struct POSE
{
    uint32_t step = 0;
    int iwrec = -1;
    int oidPlayer = -1;
    float gFade = 1.0f;
    std::vector<ALOPOSE> aalo;
    double tRecv = 0.0;
};

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

// The player object's tree: the root and its ALO children, depth first (the order both ends walk).
void CollectTree(ALO* palo, std::vector<ALO*>& apalo)
{
    apalo.push_back(palo);
    for (LO* pc : palo->aploChild)
        if (pc && FCidDerivedFrom(pc->Cid(), 0x30)) CollectTree(static_cast<ALO*>(pc), apalo);
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

// the remote player's received poses (oldest first) and the drawn puppet
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

double TNow()
{
    using C = std::chrono::steady_clock;
    static const C::time_point s_t0 = C::now();
    return std::chrono::duration<double>(C::now() - s_t0).count();
}

int NRegion() { return static_cast<int>(g_disc.Region()); }

std::string SzDefaultName()
{
    for (const char* sz : {"SLY2_COOP_NAME", "USERNAME", "USER"})
        if (const char* v = std::getenv(sz); v && *v) return v;
    return "Player";
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
    std::printf("co-op: %s connected\n", s_nameRemote.c_str());
}

void OnState(READER& r)
{
    POSE pose;
    pose.step = r.Get<uint32_t>();
    pose.iwrec = r.Get<int32_t>();
    pose.oidPlayer = r.Get<int32_t>();
    pose.gFade = r.Get<float>();
    const uint16_t calo = r.Get<uint16_t>();
    int cxf = 0;
    for (int i = 0; i < calo && r.fOk; ++i)
    {
        ALOPOSE ap;
        ap.oid = r.Get<int32_t>();
        const uint16_t c = r.Get<uint16_t>();
        cxf += c;
        if (cxf > c_cxfMax) return;
        ap.axf.resize(c);
        for (XFPOSE& x : ap.axf)
        {
            x.pos = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
            const float qx = r.Get<float>(), qy = r.Get<float>(), qz = r.Get<float>(), qw = r.Get<float>();
            x.rot = glm::quat(qw, qx, qy, qz);
            x.scale = glm::vec3(r.Get<float>(), r.Get<float>(), r.Get<float>());
        }
        pose.aalo.push_back(std::move(ap));
    }
    if (!r.fOk) return;
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
    if (msg == MSG_Hello) OnHello(ppeer, r);
    else if (msg == MSG_Refuse)
    {
        const std::string reason = r.GetStr();
        std::printf("co-op: the other player refused: %s\n", reason.c_str());
        s_status = "co-op: refused: " + reason;
    }
    else if (msg == MSG_State && ppeer == s_ppeer && s_fHelloOk) OnState(r);
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
    s_ppeerPending = enet_host_connect(s_phost, &addr, 2, 0);
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
    if (!s_ppeer || !s_fHelloOk) return;
    WRITER w;
    w.Put<uint8_t>(MSG_State);
    w.Put<uint32_t>(s_stepLocal);
    w.Put<int32_t>(g_transition.iwrecCur);
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    const bool fPlayer = plo && FCidDerivedFrom(plo->Cid(), 0x30) && !g_transition.fPending;
    if (!fPlayer)
    {
        w.Put<int32_t>(-1);
        w.Put<float>(0.0f);
        w.Put<uint16_t>(0);
        Send(s_ppeer, w, c_chanState, false);
        return;
    }
    ALO* palo = static_cast<ALO*>(plo);
    std::vector<ALO*> apalo;
    CollectTree(palo, apalo);
    w.Put<int32_t>(plo->oid);
    w.Put<float>(palo->gFade);
    w.Put<uint16_t>(uint16_t(apalo.size()));
    int cxf = 0;
    for (ALO* pa : apalo)
    {
        const int c = std::min<int>(int(pa->axf.size()), c_cxfMax - cxf);
        cxf += c;
        w.Put<int32_t>(pa->oid);
        w.Put<uint16_t>(uint16_t(c));
        for (int i = 0; i < c; ++i)
        {
            const XFPOSE x = XfposeFromMat(pa->axf[size_t(i)]->matWorld);
            w.Put<float>(x.pos.x), w.Put<float>(x.pos.y), w.Put<float>(x.pos.z);
            w.Put<float>(x.rot.x), w.Put<float>(x.rot.y), w.Put<float>(x.rot.z), w.Put<float>(x.rot.w);
            w.Put<float>(x.scale.x), w.Put<float>(x.scale.y), w.Put<float>(x.scale.z);
        }
    }
    Send(s_ppeer, w, c_chanState, false);
}

// ------------------------------------------------------------ drawing the other player
// The pose drawn: c_dstepDelay steps behind the newest received (the jitter buffer), between the two bracketing poses.
constexpr double c_dstepDelay = 3.0;

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

void ApplyPose(const POSE& a, const POSE* pb, float u)
{
    for (size_t i = 0; i < a.aalo.size(); ++i)
    {
        const ALOPOSE& apa = a.aalo[i];
        auto it = s_remote.mpoid.find(apa.oid);
        if (it == s_remote.mpoid.end()) continue;
        ALO* palo = it->second;
        const ALOPOSE* apb = (pb && i < pb->aalo.size() && pb->aalo[i].oid == apa.oid) ? &pb->aalo[i] : nullptr;
        const size_t c = std::min(apa.axf.size(), palo->axf.size());
        for (size_t k = 0; k < c; ++k)
        {
            const XFPOSE x = (apb && k < apb->axf.size()) ? Lerp(apa.axf[k], apb->axf[k], u) : apa.axf[k];
            palo->axf[k]->matWorld = MatFromXfpose(x);
        }
        palo->gFade = pb ? glm::mix(a.gFade, pb->gFade, u) : a.gFade;
    }
}

void StepRemote(SW* psw)
{
    s_remote.fShown = false;
    if (!psw || s_aposeRemote.empty()) return;
    const POSE& newest = s_aposeRemote.back();
    // only drawn in the same world, and not while either side changes worlds
    if (newest.iwrec < 0 || newest.oidPlayer < 0 || newest.iwrec != g_transition.iwrecCur || g_transition.fPending) return;
    if (!FBindRemote(psw, newest.iwrec, newest.oidPlayer)) return;
    // the step drawn: steps since the newest arrived (at 59.94 a second) added, less the delay
    const double stepNow = double(newest.step) + (TNow() - newest.tRecv) * 59.94 - c_dstepDelay;
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
    ApplyPose(*pa, pb, std::clamp(u, 0.0f, 1.0f));
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
        s_phost = enet_host_create(&addr, c_cpeerMax + 1, 2, 0, 0);
        s_status = s_phost ? "co-op: waiting for a player on port " + std::to_string(s_opt.port)
                           : "co-op: port " + std::to_string(s_opt.port) + " is in use";
    }
    else
    {
        s_phost = enet_host_create(nullptr, 1, 2, 0, 0);
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
    SendState(psw);
    StepRemote(psw);
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
}

void CoopAppendSceneAlos(std::vector<ALO*>& apalo)
{
    for (ALO* palo : s_remote.apaloDraw) apalo.push_back(palo);
}

int NCoopDrawOverride(const ALO* palo)
{
    if (s_remote.apaloDraw.empty()) return -1;
    if (std::find(s_remote.apaloDraw.begin(), s_remote.apaloDraw.end(), palo) == s_remote.apaloDraw.end()) return -1;
    return s_remote.fShown ? 1 : 0;
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
    if (s_ppeer && s_fHelloOk)
        return "co-op: " + s_nameRemote + " connected, " + std::to_string(s_ppeer->roundTripTime) + " ms";
    return s_status;
}
