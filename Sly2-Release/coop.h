#pragma once

// Online co-op (SafeHouse-Coop addition, not in the original game).
//
// Milestone 1, "see each other": two copies of the game connect over the internet (ENet, UDP). Each one sends its
// player's pose 60 times a second (the game's own step rate), and draws the other player in its own world as a
// puppet: the same character's model, taken from a kept copy of the current world (pcloader.h), posed with the
// remote player's transform records. Both games still run their own world; sharing the world (guards, loot, jobs)
// is milestone 2 (COOP.md).
//
// Starting it (command line, beside the launcher's --data-dir / --user-dir):
//   --coop-host [PORT]          wait for a player on UDP PORT (default 27015)
//   --coop-join HOST[:PORT]     connect to a host
//   --coop-name NAME            the name shown to the other player (default: the computer's user name)
// The environment variables SLY2_COOP_HOST / SLY2_COOP_JOIN / SLY2_COOP_NAME do the same.
// Both players need the same disc region (USA, Europe or Japan): the handshake refuses a mismatch.

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <unordered_set>
#include <vector>

struct SW;
struct ALO;
struct LO;
struct CTMSG;
struct DETECTOR;
struct XF;

enum COOPMODE
{
    COOPMODE_Off = 0,
    COOPMODE_Host = 1,
    COOPMODE_Join = 2,
};

struct COOPOPTIONS
{
    COOPMODE mode = COOPMODE_Off;
    std::string host;        // join: the host's address
    uint16_t port = 27015;
    std::string name;
};

// main.cpp: one of the --coop-* arguments at argv[i] (advancing i past its value); false if it is not one.
bool FCoopParseArg(int argc, char** argv, int& i);
// main.cpp: after the arguments (the environment variables are read here too); starts networking when asked for.
void CoopInit();
void CoopShutdown();
bool FCoopActive();

// The world hooks (pcloader.h's, registered by coop.cpp): each step after the world's update, and a world's start.
void CoopStep(SW* psw);
void CoopOnWorldStarted(SW* psw);

// The scene (pccharacter.cpp's hooks call these): the remote players' objects drawn in the current world.
void CoopAppendSceneAlos(std::vector<ALO*>& apalo);
int NCoopDrawOverride(const ALO* palo);   // -1: not a co-op object; 0: hidden; 1: drawn
bool FCoopTakeSceneRebuild();

// The joiner's guards: the host's are shown, so the joiner's own do not think (update.cpp skips them), and the hits
// on them go to the host (guardphys.cpp: true when sent there, the hit then not taken here).
bool FCoopSkipUpdate(const LO* plo);
// (pad.cpp) a switch / bell / plate turned on (1) or off (0) here is set in the other game too; the other's set here
void CoopOnPadState(int oid, int iclone, int n);
bool FCoopApplyPadState(SW* psw, int oid, int iclone, int n);
// (task.cpp) the objects of the current job's steps (its characters stay in the host's game)
void CoopCollectJobObjects(SW* psw, std::unordered_set<int>& setoid);
// (detect.cpp) host: a mission character's detector would see the other player (the job then fails as for this one)
bool FCoopMissionSeesRemote(DETECTOR* pdet, LO* pso);
// (detect.cpp) a detector seeing the other game's player at a point (that player has no object in this world)
bool FCoopDetectorSeesPoint(DETECTOR* pdet, const glm::vec3& posAi, const glm::vec3& posDet);
// (task.cpp) a task's state changing here (1: started, 2: done) is sent to the other game; the other's applied here
void CoopOnTaskState(int oid, int nOld, int n);
bool FCoopApplyTaskState(SW* psw, int oid, int n);
// (activate.cpp) the other player's character's place in this world: its surroundings are activated too (guards and
// their pooled instances placed there, objects updated), so both games have the guards around both players
bool FCoopActivationPos(glm::vec3& pos);
// (activate.cpp) a proxy's instance row given back
void CoopDetachProxyRow(SW* psw, LO* pproxy, int irow);
// (game.cpp) after the step's world matrices are rebuilt from the local ones: the mirrored guards' poses written again
void CoopAfterWorlds();
bool FCoopForwardGuardHit(ALO* palo, const CTMSG* pctm);
// (guardphys.cpp) the guards' contact handler, for the hits the joiner sends the host
bool FCoopGuardAcceptContact(ALO* palo, CTMSG* pctm);

// A line for the screen's corner ("co-op: waiting for a player on port 27015", "co-op: Ana connected, 48 ms").
std::string SzCoopStatus();
