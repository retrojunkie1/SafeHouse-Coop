// Co-op over Steam (steamnet.h): the Steamworks flat API, loaded at run time.

#include "steamnet.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace
{
// ------------------------------------------------------------ the flat API (steam_api_flat.h, SDK 1.48 and later)
using HPIPE = int32_t;
using APICALL = uint64_t;

struct CALLBACKMSG               // CallbackMsg_t
{
    int32_t hUser;
    int32_t iCallback;
    uint8_t* pubParam;
    int32_t cubParam;
};
struct APICALLCOMPLETED          // SteamAPICallCompleted_t (703)
{
    uint64_t hCall;
    int32_t iCallback;
    uint32_t cubParam;
};
struct LOBBYCREATED              // LobbyCreated_t (513)
{
    int32_t eResult;
    uint64_t idLobby;
};
struct LOBBYENTER                // LobbyEnter_t (504)
{
    uint64_t idLobby;
    uint32_t grfPermissions;
    bool fLocked;
    uint32_t eResponse;          // 1: success
};
struct LOBBYCHATUPDATE           // LobbyChatUpdate_t (506)
{
    uint64_t idLobby;
    uint64_t idChanged;
    uint64_t idMaker;
    uint32_t grfChange;          // 1 entered, 2 left, 4 disconnected, 8 kicked, 16 banned
};
struct LOBBYJOINREQUESTED        // GameLobbyJoinRequested_t (333)
{
    uint64_t idLobby;
    uint64_t idFriend;
};
struct PRESENCEJOINREQUESTED     // GameRichPresenceJoinRequested_t (337)
{
    uint64_t idFriend;
    char szConnect[256];
};
struct IDENTITY                  // SteamNetworkingIdentity
{
    int32_t eType = 16;          // k_ESteamNetworkingIdentityType_SteamID
    int32_t cbSize = 8;
    uint64_t id = 0;
    uint8_t abRest[120]{};
};
static_assert(sizeof(IDENTITY) == 136, "SteamNetworkingIdentity");
struct NETMESSAGE                // SteamNetworkingMessage_t (the fields read)
{
    void* pData;
    int32_t cbSize;
    uint32_t hConn;
    IDENTITY identityPeer;
};
constexpr int c_cbLobbyCreated = 513, c_cbLobbyEnter = 504, c_cbLobbyChatUpdate = 506, c_cbLobbyJoinRequested = 333,
              c_cbPresenceJoinRequested = 337, c_cbSessionRequest = 1251, c_cbApiCallCompleted = 703;
constexpr int c_nSendReliable = 8, c_nSendAutoRestart = 32;

struct API
{
    void* hlib = nullptr;
    // the library
    int (*InitFlat)(char*) = nullptr;   // SteamAPI_InitFlat (1.58 and later; 0 = OK)
    bool (*Init)() = nullptr;           // SteamAPI_Init (before 1.58)
    void (*Shutdown)() = nullptr;
    HPIPE (*GetHSteamPipe)() = nullptr;
    void (*MdInit)() = nullptr;
    void (*MdRunFrame)(HPIPE) = nullptr;
    bool (*MdGetNextCallback)(HPIPE, CALLBACKMSG*) = nullptr;
    void (*MdFreeLastCallback)(HPIPE) = nullptr;
    bool (*MdGetApiCallResult)(HPIPE, APICALL, void*, int, int, bool*) = nullptr;
    // the interfaces
    void* pmm = nullptr;     // ISteamMatchmaking
    void* pfr = nullptr;     // ISteamFriends
    void* pus = nullptr;     // ISteamUser
    void* pnm = nullptr;     // ISteamNetworkingMessages
    void* pnu = nullptr;     // ISteamNetworkingUtils
    APICALL (*CreateLobby)(void*, int, int) = nullptr;
    APICALL (*JoinLobby)(void*, uint64_t) = nullptr;
    void (*LeaveLobby)(void*, uint64_t) = nullptr;
    bool (*SetLobbyData)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*GetLobbyData)(void*, uint64_t, const char*) = nullptr;
    uint64_t (*GetLobbyOwner)(void*, uint64_t) = nullptr;
    int (*GetNumLobbyMembers)(void*, uint64_t) = nullptr;
    uint64_t (*GetLobbyMemberByIndex)(void*, uint64_t, int) = nullptr;
    void (*InviteDialog)(void*, uint64_t) = nullptr;
    bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
    void (*ClearRichPresence)(void*) = nullptr;
    const char* (*GetPersonaName)(void*) = nullptr;
    const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
    uint64_t (*GetSteamID)(void*) = nullptr;
    int (*SendMessageToUser)(void*, const IDENTITY*, const void*, uint32_t, int, int) = nullptr;
    int (*ReceiveMessagesOnChannel)(void*, int, NETMESSAGE**, int) = nullptr;
    bool (*AcceptSessionWithUser)(void*, const IDENTITY*) = nullptr;
    bool (*CloseSessionWithUser)(void*, const IDENTITY*) = nullptr;
    int (*GetSessionConnectionInfo)(void*, const IDENTITY*, void*, void*) = nullptr;
    void (*InitRelayNetworkAccess)(void*) = nullptr;
    void (*ReleaseMessage)(NETMESSAGE*) = nullptr;
} s_api;

void* PfnSym(const char* sz)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(s_api.hlib), sz));
#else
    return dlsym(s_api.hlib, sz);
#endif
}

template <class F> bool FSym(F& pfn, const char* sz)
{
    pfn = reinterpret_cast<F>(PfnSym(sz));
    return pfn != nullptr;
}

// An interface by its accessor export, the newest version the library has (the library's own flat functions then
// match that version)
void* PvInterface(const char* szBase, std::initializer_list<int> aver)
{
    for (int v : aver)
    {
        char sz[96];
        std::snprintf(sz, sizeof(sz), "%s%03d", szBase, v);
        if (auto pfn = reinterpret_cast<void* (*)()>(PfnSym(sz)))
            if (void* pv = pfn()) return pv;
    }
    return nullptr;
}

std::filesystem::path PathExeDir()
{
#ifdef _WIN32
    wchar_t wsz[4096];
    const DWORD cch = GetModuleFileNameW(nullptr, wsz, 4096);
    return std::filesystem::path(std::wstring(wsz, cch)).parent_path();
#else
    char sz[4096];
    const ssize_t cch = readlink("/proc/self/exe", sz, sizeof(sz) - 1);
    return cch > 0 ? std::filesystem::path(std::string(sz, size_t(cch))).parent_path() : std::filesystem::current_path();
#endif
}

// ------------------------------------------------------------ state
enum ROLE { ROLE_None, ROLE_Creating, ROLE_Hosting, ROLE_Joining, ROLE_Joined };
bool s_fUp = false;
HPIPE s_hpipe = 0;
uint16_t s_protocol = 0;
uint64_t s_idSelf = 0;
ROLE s_role = ROLE_None;
uint64_t s_idLobby = 0;          // the lobby this game is in (its own, or the one it joined)
uint64_t s_idLobbyWanted = 0;    // a lobby to join at the next poll
uint64_t s_idPeer = 0;           // the other player (connected)
double s_tLastRecv = 0.0;
double s_tNextCreate = 0.0;
std::string s_strStatus;
std::string s_strPresence;
std::deque<STEAMEVENT> s_aev;
constexpr const char* c_szMod = "SafeHouse-Coop Sly 2";
constexpr double c_dtTimeout = 15.0;

double TNow()
{
    using C = std::chrono::steady_clock;
    static const C::time_point s_t0 = C::now();
    return std::chrono::duration<double>(C::now() - s_t0).count();
}

IDENTITY IdentityOf(uint64_t id)
{
    IDENTITY ident;
    ident.id = id;
    return ident;
}

std::string StrName(uint64_t id)
{
    const char* sz = s_api.GetFriendPersonaName ? s_api.GetFriendPersonaName(s_api.pfr, id) : nullptr;
    return sz && *sz ? std::string(sz) : std::string("a friend");
}

void UpdatePresence()
{
    if (!s_api.SetRichPresence) return;
    s_api.SetRichPresence(s_api.pfr, "status", s_strPresence.empty() ? "Sly 2 co-op (SafeHouse-Coop)" : s_strPresence.c_str());
    if (s_idLobby)
    {
        const std::string connect = "+connect_lobby " + std::to_string(s_idLobby);
        s_api.SetRichPresence(s_api.pfr, "connect", connect.c_str());
    }
    else s_api.SetRichPresence(s_api.pfr, "connect", "");
}

void Lost(const char* szWhy)
{
    if (s_idPeer)
    {
        std::printf("co-op (Steam): %s (%s)\n", szWhy, StrName(s_idPeer).c_str());
        const IDENTITY ident = IdentityOf(s_idPeer);
        if (s_api.CloseSessionWithUser) s_api.CloseSessionWithUser(s_api.pnm, &ident);
        s_idPeer = 0;
        STEAMEVENT ev;
        ev.kind = STEAMEVENT::Disconnected;
        s_aev.push_back(std::move(ev));
    }
}

void LeaveLobby()
{
    if (s_idLobby) s_api.LeaveLobby(s_api.pmm, s_idLobby);
    s_idLobby = 0;
    s_role = ROLE_None;
}

void CreateLobby()
{
    LeaveLobby();
    s_api.CreateLobby(s_api.pmm, 1 /* friends only */, 2);
    s_role = ROLE_Creating;
    s_strStatus = "co-op (Steam): starting a lobby";
    s_tNextCreate = TNow() + 10.0;
}

// The joiner back to hosting its own lobby (the host left, refused, or went quiet)
void BackToHosting(const char* szWhy)
{
    Lost(szWhy);
    CreateLobby();
}

bool FMember(uint64_t id)
{
    if (!s_idLobby) return false;
    const int c = s_api.GetNumLobbyMembers(s_api.pmm, s_idLobby);
    for (int i = 0; i < c; ++i)
        if (s_api.GetLobbyMemberByIndex(s_api.pmm, s_idLobby, i) == id) return true;
    return false;
}

void OnLobbyCreated(const LOBBYCREATED& lc)
{
    if (s_role != ROLE_Creating) return;
    if (lc.eResult != 1 || !lc.idLobby)
    {
        std::printf("co-op (Steam): the lobby was not created (result %d)\n", lc.eResult);
        s_strStatus = "co-op (Steam): no lobby (Steam said " + std::to_string(lc.eResult) + "), retrying";
        s_role = ROLE_None;
        return;
    }
    s_idLobby = lc.idLobby;
    s_role = ROLE_Hosting;
    s_api.SetLobbyData(s_api.pmm, s_idLobby, "mod", c_szMod);
    s_api.SetLobbyData(s_api.pmm, s_idLobby, "protocol", std::to_string(s_protocol).c_str());
    s_strStatus = "co-op (Steam): hosting, F8 invites a friend";
    std::printf("co-op (Steam): hosting lobby %llu\n", static_cast<unsigned long long>(s_idLobby));
    UpdatePresence();
}

void OnLobbyEnter(const LOBBYENTER& le)
{
    if (s_role != ROLE_Joining || le.idLobby != s_idLobby) return;   // (our own lobby's entry: nothing to do)
    if (le.eResponse != 1)
    {
        std::printf("co-op (Steam): could not enter the lobby (response %u)\n", le.eResponse);
        s_idLobby = 0;
        s_role = ROLE_None;
        CreateLobby();
        s_strStatus = "co-op (Steam): the friend's game could not be joined (full or closed)";
        return;
    }
    auto str = [&](const char* szKey) {   // (copied: the library's string is only good until its next call)
        const char* sz = s_api.GetLobbyData(s_api.pmm, le.idLobby, szKey);
        return std::string(sz ? sz : "");
    };
    const std::string strMod = str("mod"), strProto = str("protocol");
    if (strMod != c_szMod || std::atoi(strProto.c_str()) != int(s_protocol))
    {
        std::printf("co-op (Steam): lobby %llu is not this build's (mod '%s', protocol '%s')\n",
                    static_cast<unsigned long long>(le.idLobby), strMod.c_str(), strProto.c_str());
        CreateLobby();
        s_strStatus = strMod == c_szMod ? "co-op (Steam): your friend has a different SafeHouse-Coop build"
                                        : "co-op (Steam): that lobby is not a Sly 2 co-op game";
        return;
    }
    const uint64_t idOwner = s_api.GetLobbyOwner(s_api.pmm, le.idLobby);
    if (!idOwner || idOwner == s_idSelf)
    {
        s_role = ROLE_Hosting;
        return;
    }
    s_role = ROLE_Joined;
    s_idPeer = idOwner;
    s_tLastRecv = TNow();
    s_strStatus = "co-op (Steam): joining " + StrName(idOwner);
    std::printf("co-op (Steam): joined %s's lobby\n", StrName(idOwner).c_str());
    UpdatePresence();
    STEAMEVENT ev;
    ev.kind = STEAMEVENT::Connected;
    ev.fJoiner = true;
    s_aev.push_back(std::move(ev));
}

void RequestJoin(uint64_t idLobby)
{
    if (!idLobby || idLobby == s_idLobby) return;
    Lost("left for another lobby");
    LeaveLobby();
    s_idLobby = idLobby;
    s_role = ROLE_Joining;
    s_api.JoinLobby(s_api.pmm, idLobby);
    s_strStatus = "co-op (Steam): joining a friend's game";
    std::printf("co-op (Steam): joining lobby %llu\n", static_cast<unsigned long long>(idLobby));
}

void OnCallback(int iCallback, const uint8_t* pb, size_t cb)
{
    switch (iCallback)
    {
    case c_cbLobbyCreated:
        if (cb >= sizeof(LOBBYCREATED)) OnLobbyCreated(*reinterpret_cast<const LOBBYCREATED*>(pb));
        break;
    case c_cbLobbyEnter:
        if (cb >= sizeof(LOBBYENTER)) OnLobbyEnter(*reinterpret_cast<const LOBBYENTER*>(pb));
        break;
    case c_cbLobbyChatUpdate:
        if (cb >= sizeof(LOBBYCHATUPDATE))
        {
            const auto& u = *reinterpret_cast<const LOBBYCHATUPDATE*>(pb);
            if (u.idLobby == s_idLobby && u.idChanged == s_idPeer && (u.grfChange & (2 | 4 | 8 | 16)))
            {
                if (s_role == ROLE_Joined) BackToHosting("the host left");
                else Lost("left");
            }
        }
        break;
    case c_cbLobbyJoinRequested:
        if (cb >= sizeof(LOBBYJOINREQUESTED)) RequestJoin(reinterpret_cast<const LOBBYJOINREQUESTED*>(pb)->idLobby);
        break;
    case c_cbPresenceJoinRequested:
        if (cb >= sizeof(PRESENCEJOINREQUESTED))
        {
            const auto& r = *reinterpret_cast<const PRESENCEJOINREQUESTED*>(pb);
            std::string s(r.szConnect, strnlen(r.szConnect, sizeof(r.szConnect)));
            if (const size_t i = s.find("+connect_lobby"); i != std::string::npos)
                RequestJoin(std::strtoull(s.c_str() + i + 14, nullptr, 10));
        }
        break;
    case c_cbSessionRequest:
        if (cb >= sizeof(IDENTITY))
        {
            const auto& ident = *reinterpret_cast<const IDENTITY*>(pb);
            const uint64_t id = ident.eType == 16 ? ident.id : 0;
            // (only a member of our lobby, and one other player)
            if (s_role == ROLE_Hosting && id && !s_idPeer && FMember(id))
            {
                s_api.AcceptSessionWithUser(s_api.pnm, &ident);
                s_idPeer = id;
                s_tLastRecv = TNow();
                s_strStatus = "co-op (Steam): " + StrName(id) + " is joining";
                std::printf("co-op (Steam): %s joined\n", StrName(id).c_str());
                STEAMEVENT ev;
                ev.kind = STEAMEVENT::Connected;
                ev.fJoiner = false;
                s_aev.push_back(std::move(ev));
            }
            else if (id && id == s_idPeer) s_api.AcceptSessionWithUser(s_api.pnm, &ident);   // (a session restarted)
        }
        break;
    default: break;
    }
}

void RunCallbacks()
{
    s_api.MdRunFrame(s_hpipe);
    CALLBACKMSG msg{};
    while (s_api.MdGetNextCallback(s_hpipe, &msg))
    {
        if (msg.iCallback == c_cbApiCallCompleted && msg.pubParam)
        {
            const auto& cc = *reinterpret_cast<const APICALLCOMPLETED*>(msg.pubParam);
            std::vector<uint8_t> ab(cc.cubParam);
            bool fFailed = false;
            if (!ab.empty() && s_api.MdGetApiCallResult(s_hpipe, cc.hCall, ab.data(), int(ab.size()), cc.iCallback, &fFailed) && !fFailed)
                if (cc.iCallback == c_cbLobbyCreated) OnCallback(cc.iCallback, ab.data(), ab.size());   // (the rest come as callbacks too)
        }
        else if (msg.pubParam) OnCallback(msg.iCallback, msg.pubParam, size_t(msg.cubParam));
        s_api.MdFreeLastCallback(s_hpipe);
    }
}

void ReceiveMessages()
{
    NETMESSAGE* apmsg[32];
    for (int chan = 0; chan < 3; ++chan)
        for (int c; (c = s_api.ReceiveMessagesOnChannel(s_api.pnm, chan, apmsg, 32)) > 0;)
            for (int i = 0; i < c; ++i)
            {
                NETMESSAGE* p = apmsg[i];
                if (s_idPeer && p->identityPeer.id == s_idPeer && p->pData && p->cbSize > 0)
                {
                    STEAMEVENT ev;
                    ev.kind = STEAMEVENT::Received;
                    ev.chan = chan;
                    ev.ab.assign(static_cast<const uint8_t*>(p->pData), static_cast<const uint8_t*>(p->pData) + p->cbSize);
                    s_aev.push_back(std::move(ev));
                    s_tLastRecv = TNow();
                }
                s_api.ReleaseMessage(p);
            }
}
}  // namespace

bool FSteamNetInit(uint16_t protocol, std::string* pstrErr)
{
    s_protocol = protocol;
    auto fail = [&](const std::string& s) {
        *pstrErr = s;
        return false;
    };
    // Spacewar's app id (no steam_appid.txt needed)
#ifdef _WIN32
    SetEnvironmentVariableA("SteamAppId", "480");
    SetEnvironmentVariableA("SteamGameId", "480");
    const std::filesystem::path path = PathExeDir() / "steam_api64.dll";
    s_api.hlib = LoadLibraryW(path.wstring().c_str());
#else
    setenv("SteamAppId", "480", 1);
    setenv("SteamGameId", "480", 1);
    const std::filesystem::path path = PathExeDir() / "libsteam_api.so";
    s_api.hlib = dlopen(path.string().c_str(), RTLD_NOW);
#endif
    if (!s_api.hlib) return fail("put " + path.filename().string() + " beside the game's exe (copy it from any Steam game's folder)");
    FSym(s_api.InitFlat, "SteamAPI_InitFlat");
    FSym(s_api.Init, "SteamAPI_Init");
    bool fOk = FSym(s_api.Shutdown, "SteamAPI_Shutdown") && FSym(s_api.GetHSteamPipe, "SteamAPI_GetHSteamPipe") &&
               FSym(s_api.MdInit, "SteamAPI_ManualDispatch_Init") && FSym(s_api.MdRunFrame, "SteamAPI_ManualDispatch_RunFrame") &&
               FSym(s_api.MdGetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback") &&
               FSym(s_api.MdFreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback") &&
               FSym(s_api.MdGetApiCallResult, "SteamAPI_ManualDispatch_GetAPICallResult") &&
               FSym(s_api.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby") && FSym(s_api.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby") &&
               FSym(s_api.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby") && FSym(s_api.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData") &&
               FSym(s_api.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData") && FSym(s_api.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner") &&
               FSym(s_api.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") &&
               FSym(s_api.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex") &&
               FSym(s_api.InviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog") &&
               FSym(s_api.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence") &&
               FSym(s_api.ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence") &&
               FSym(s_api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName") &&
               FSym(s_api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName") &&
               FSym(s_api.GetSteamID, "SteamAPI_ISteamUser_GetSteamID") &&
               FSym(s_api.SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser") &&
               FSym(s_api.ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel") &&
               FSym(s_api.AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") &&
               FSym(s_api.CloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser") &&
               FSym(s_api.GetSessionConnectionInfo, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo") &&
               FSym(s_api.InitRelayNetworkAccess, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess") &&
               FSym(s_api.ReleaseMessage, "SteamAPI_SteamNetworkingMessage_t_Release") && (s_api.InitFlat || s_api.Init);
    if (!fOk) return fail(path.filename().string() + " is too old (it needs Steamworks 1.48 or later: take it from a newer game)");
    if (s_api.InitFlat)
    {
        char szErr[1024] = {};
        if (const int n = s_api.InitFlat(szErr); n != 0)
            return fail(std::string("Steam did not start (") + (szErr[0] ? szErr : "is Steam running and signed in?") + ")");
    }
    else if (!s_api.Init()) return fail("Steam did not start (is Steam running and signed in?)");
    s_fUp = true;
    s_api.MdInit();
    s_hpipe = s_api.GetHSteamPipe();
    s_api.pmm = PvInterface("SteamAPI_SteamMatchmaking_v", {9});
    s_api.pfr = PvInterface("SteamAPI_SteamFriends_v", {18, 17});
    s_api.pus = PvInterface("SteamAPI_SteamUser_v", {23, 22, 21, 20});
    s_api.pnm = PvInterface("SteamAPI_SteamNetworkingMessages_SteamAPI_v", {2});
    s_api.pnu = PvInterface("SteamAPI_SteamNetworkingUtils_SteamAPI_v", {4, 3});
    if (!s_api.pmm || !s_api.pfr || !s_api.pus || !s_api.pnm || !s_api.pnu)
    {
        SteamNetShutdown();
        return fail(path.filename().string() + " lacks a Steam interface this needs (take it from a newer game)");
    }
    s_idSelf = s_api.GetSteamID(s_api.pus);
    s_api.InitRelayNetworkAccess(s_api.pnu);
    std::printf("co-op (Steam): signed in as %s\n", StrSteamNetName().c_str());
    if (!s_idLobbyWanted) CreateLobby();
    return true;
}

void SteamNetShutdown()
{
    if (!s_fUp) return;
    Lost("shutting down");
    LeaveLobby();
    if (s_api.ClearRichPresence) s_api.ClearRichPresence(s_api.pfr);
    s_api.Shutdown();
    s_fUp = false;
}

void SteamNetJoinLobby(uint64_t idLobby) { s_idLobbyWanted = idLobby; }

bool FSteamNetPoll(STEAMEVENT* pev)
{
    if (!s_fUp) return false;
    if (s_aev.empty())
    {
        if (s_idLobbyWanted)
        {
            RequestJoin(s_idLobbyWanted);
            s_idLobbyWanted = 0;
        }
        RunCallbacks();
        ReceiveMessages();
        if (s_role == ROLE_None && TNow() >= s_tNextCreate) CreateLobby();
        if (s_role == ROLE_Creating && TNow() >= s_tNextCreate) s_role = ROLE_None;   // (no answer: again)
        if (s_idPeer && TNow() - s_tLastRecv > c_dtTimeout)
        {
            if (s_role == ROLE_Joined) BackToHosting("timed out");
            else Lost("timed out");
        }
        if (s_aev.empty()) return false;
    }
    *pev = std::move(s_aev.front());
    s_aev.pop_front();
    return true;
}

bool FSteamNetSend(const void* pv, size_t cb, int chan, bool fReliable)
{
    if (!s_fUp || !s_idPeer) return false;
    const IDENTITY ident = IdentityOf(s_idPeer);
    const int n = s_api.SendMessageToUser(s_api.pnm, &ident, pv, uint32_t(cb), (fReliable ? c_nSendReliable : 0) | c_nSendAutoRestart, chan);
    return n == 1;
}

void SteamNetDropPeer()
{
    if (!s_fUp) return;
    if (s_role == ROLE_Joined) BackToHosting("dropped");
    else Lost("dropped");
}

int NSteamNetPingMs()
{
    if (!s_fUp || !s_idPeer) return 0;
    const IDENTITY ident = IdentityOf(s_idPeer);
    alignas(8) static uint8_t s_abInfo[4096];   // SteamNetConnectionInfo_t
    alignas(8) uint8_t abStatus[1024] = {};      // SteamNetConnectionRealTimeStatus_t: m_eState, m_nPing, ...
    s_api.GetSessionConnectionInfo(s_api.pnm, &ident, s_abInfo, abStatus);
    int32_t nPing = 0;
    std::memcpy(&nPing, abStatus + 4, 4);
    return nPing;
}

void SteamNetOpenInvite()
{
    if (s_fUp && s_idLobby && s_role == ROLE_Hosting) s_api.InviteDialog(s_api.pfr, s_idLobby);
}

std::string StrSteamNetName()
{
    const char* sz = s_fUp ? s_api.GetPersonaName(s_api.pfr) : nullptr;
    return sz && *sz ? sz : "";
}

std::string StrSteamNetStatus() { return s_strStatus; }

void SteamNetSetPresence(const std::string& strStatus)
{
    if (!s_fUp || strStatus == s_strPresence) return;
    s_strPresence = strStatus;
    UpdatePresence();
}

bool FSteamNetHosting() { return s_role != ROLE_Joined && s_role != ROLE_Joining; }
