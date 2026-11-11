# SafeHouse-Coop: online co-op for Sly 2

A fork of [SafeHouse](https://github.com/FridiNaTor1/SafeHouse) that adds online co-op to **Sly 2: Band of Thieves**.
Online only (no split screen), host-authoritative, like modern co-op games. Sly 1 and Sly 3 are out of scope for now.

## Timing and regions

SafeHouse's Sly 2 runs its game logic at a fixed 59.94 steps a second on every disc region (the original's NTSC step;
the European discs' 50 Hz is not used) and draws extra frames in between above 60 FPS (`frameinterp.h`). Co-op keeps
that: the network runs at the step rate, each player still sees their own high frame rate. The region only matters for
matching: both players must have the same region and the same SafeHouse-Coop build (the handshake checks both).

## Milestones

| | What | Status |
|---|---|---|
| **M1** | **See each other.** Two games connect (ENet over UDP). Each sends its player's pose every step; the other is drawn in your world as a puppet (the same character's model from a kept copy of the world, `pcloader.h`), interpolated 3 steps behind. Each game still runs its own world. Only what the other game draws is drawn (effects and gadgets hidden); only the focused window reads the controller. | done |
| M2 | **Same world.** The joiner follows the host's world changes (warps, level loads, safe house) and job / task, and takes the host's save (progress, coins, items; its own health kept). | testing |
| M3 | **Shared guards.** Each guard runs in the game of the player nearer to it (the host decides, with a margin so it doesn't flip back and forth); the other game freezes its copy and shows the runner's pose (15 a second, compact: about 13 bytes a bone). So guards see, chase and fight whoever is near them, and hits land in the game running the guard (a hit on a mirrored guard is sent there). Knocked-out guards stay down in both. | testing |
| M4 | **Shared progress.** Loot, coins, keys, clue bottles, job steps and saves owned by the host and mirrored to the joiner. | |
| M5 | **Playing it.** Invite codes and a relay for players behind home routers, an in-game menu (host / join / leave), drop-in, reconnect, a launcher button. | |

## Playing M1 (testing)

Both players run the same build. The host opens UDP port 27015 on their router (or both use a virtual LAN such as
Tailscale or ZeroTier until M5's relay).

```
Sly2-Release --data-dir <installation> --user-dir <profile> --coop-host            (port 27015, or --coop-host PORT)
Sly2-Release --data-dir <installation> --user-dir <profile> --coop-join HOST[:PORT]
```

### Over Steam (no addresses, no open ports)

`--coop-steam` (or `SLY2_COOP_STEAM=1`; `tools/Sly2 Co-op (Steam).bat` starts it from the release's `bin` folder)
uses Steam lobbies and Steam's relay network instead of a direct connection (`steamnet.h`). It borrows Spacewar's
app id (480, Valve's public Steamworks sample), so Steam shows you in "Spacewar"; both players need Steam running
and Spacewar in their library (free).

- The game loads `steam_api64.dll` from beside its exe at run time (Steamworks 1.48 or later: copy it from any recent
  64-bit Steam game). Nothing of Valve's is in this repository; without the library the window's title says why.
- Each game starts by hosting a friends-only lobby. **F8** opens Steam's invite dialog; the friend accepts the invite
  or uses "Join Game" in the friends list and becomes the joiner (their game must be running: Steam would otherwise
  start Spacewar itself). `+connect_lobby ID` on the command line joins a lobby directly.
- The lobby carries the mod's name and co-op protocol: other Spacewar lobbies and other builds are turned away.
- When the host leaves, the joiner goes back to hosting its own lobby.

`--coop-name NAME` sets the name the other player sees (with Steam: your Steam name). The window's title shows the connection and its ping. Both
players walking in the same world see each other; in different worlds the other is not drawn.

## Code

- `Sly2-Release/coop.h`, `coop.cpp`: networking, the wire format, sending the pose, drawing the other player.
- `Sly2-Release/steamnet.h`, `steamnet.cpp`: co-op over Steam (lobbies, invites, Steam's relay), the flat Steamworks
  API loaded at run time.
- `Libraries/enet`: ENet 1.3.18 (MIT).
- Hooks: `main.cpp` (arguments, start / stop), `pccharacter.cpp` (the scene: the puppet's objects drawn), the
  `pcloader.h` world hooks (each step, each world start).

## QoL: the walkable safe house

Entering a safe house no longer puts you in the menu: the character it shows takes the pad and walks the hideout
(po.cpp, `FWalkableSafeHouse`). Standing by a gang member or the ThiefNet laptop sets the safe house's choice to it,
so its own banner ("Press X to play as Bentley", ThiefNet) shows and its button does the original action (jump held
back there). Walking out of the hideout (it opens onto the street) leaves the safe house as its exit does.
`SLY2_CLASSIC_SAFEHOUSE=1` brings back the original menu. Tested in Paris (the hideout at -2408, -3225).

Debug switches (coop.cpp, PC only): `SLY2N_PROBE`, `SLY2N_PROBEPOS`, `SLY2N_TELEPORTTO`, `SLY2N_TELEPORTPOS`.
