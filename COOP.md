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

`--coop-name NAME` sets the name the other player sees. The window's title shows the connection and its ping. Both
players walking in the same world see each other; in different worlds the other is not drawn.

## Code

- `Sly2-Release/coop.h`, `coop.cpp`: networking, the wire format, sending the pose, drawing the other player.
- `Libraries/enet`: ENet 1.3.18 (MIT).
- Hooks: `main.cpp` (arguments, start / stop), `pccharacter.cpp` (the scene: the puppet's objects drawn), the
  `pcloader.h` world hooks (each step, each world start).
