#pragma once

// Co-op over Steam (SafeHouse-Coop, coop.h): lobbies, invites and Steam's relay network, so friends connect without
// addresses or open ports. It borrows Spacewar's app id (480, Valve's public Steamworks sample), as many mods do, so
// Steam shows the player in "Spacewar".
//
// Nothing of Valve's is built in or committed: the game loads steam_api64.dll (Windows; libsteam_api.so on Linux)
// from beside its executable at run time and uses its flat C API with manual callback dispatch. Without the library
// or a running Steam, Steam co-op reports why and the game plays alone.
//
// The roles: the game starts by hosting a friends-only lobby; a friend joins it from an invite (F8 opens Steam's
// invite dialog), from "Join Game" in the friends list, or with +connect_lobby ID on the command line, and so becomes
// the joiner. The lobby carries the mod's name and co-op protocol, so other Spacewar lobbies are never joined.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct STEAMEVENT
{
    enum KIND { Connected, Received, Disconnected };
    KIND kind = Received;
    bool fJoiner = false;          // Connected: this game joined the other's lobby (else the other joined ours)
    int chan = 0;                  // Received: the channel
    std::vector<uint8_t> ab;       // Received: the message
};

// Loads the library and starts Steam (the protocol is the co-op protocol, kept in the lobby). False with a reason.
bool FSteamNetInit(uint16_t protocol, std::string* pstrErr);
void SteamNetShutdown();
// Joins a lobby (+connect_lobby ID), at the next poll.
void SteamNetJoinLobby(uint64_t idLobby);
// Steam's callbacks run and the next event taken (call until false, once a step).
bool FSteamNetPoll(STEAMEVENT* pev);
// To the other player (channel 0..2; unreliable messages may be dropped or come out of order).
bool FSteamNetSend(const void* pv, size_t cb, int chan, bool fReliable);
// The other player dropped (refused): the joiner goes back to hosting its own lobby.
void SteamNetDropPeer();
int NSteamNetPingMs();
void SteamNetOpenInvite();
std::string StrSteamNetName();      // this player's Steam name
std::string StrSteamNetStatus();    // "hosting: F8 invites a friend", "joining ..."
void SteamNetSetPresence(const std::string& strStatus);
bool FSteamNetHosting();            // hosting (not joined to another's lobby)
