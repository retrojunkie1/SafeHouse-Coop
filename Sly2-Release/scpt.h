#pragma once

// The scripts (SCPT, cid 0x72; module 0x1DF5B0..0x1E5D80): an object whose load options build a list of 0x50-byte
// commands (the splice-wrapped builders 0x106A80..0x10A438 -> 0x1E0290..0x1E1B10, kinds 0..0x5A), run when the script
// is started (0x1DFD58: an ASEG event, command 0xB, RefreshWorldReference 0x1DFE80) by runners (0xF0 bytes, the pool
// 0x3E1560): a runner walks its block of commands (0 opens a block, 1 closes it), executing each (0x1E1E30) and
// waiting while it is not done (0x1E3890); command 0 makes a sub-runner of the block it opens. The script's update
// (vt slot 16 0x1DFC50) runs its runners each frame; it stops (0x1DFEB8) once none is left.
//
// Native (scpt.cpp): the commands keep the original's words (+0x10.. as int / float slots), the runners their
// fields by name. Ported: the builders used by the worlds' scripts, the runner (0x1E5AE0, 0x1E1C40, 0x1E1D48,
// 0x1E5628), the script's start / update / stop, the executor and the done tests for the flow commands (blocks,
// labels, sub-scripts, waits, alarms, events), the ASEGs (0xA), the placement (0x16), the camera's (0x10, 0x17, 0x32,
// 0x3D..0x41); the others warn once ("scpt: command 0x.. not ported") and finish at once.

#include <cstdint>

struct LO;
class CBinaryInputStream;

// orig: 0x001DFB10's parts around the options (the command buffer and the opening command; the closing size)
void BeginScptLoad(LO* plo);
void EndScptLoad(LO* plo);

// orig: 0x001DFD58 (the script started; plo: the object that started it, the default target)
void StartScpt(LO* pscpt, LO* ploStarter);
// orig: 0x001DFEB8
void StopScpt(LO* pscpt);
// orig: 0x001DFFD8 (running)
bool FScptRunning(LO* pscpt);
// Native skip QoL: the oldest running, paused script whose scene is skippable (SceneSkipKind), or zero.
uint64_t IdScptCutscene();
// (native QoL, skipcut.h) a pausing script's scene class: SKIPK_*
enum { SKIPK_None = 0, SKIPK_Dialog = 1, SKIPK_Cinematic = 2 };
int SceneSkipKind(LO* pscpt);
// (native QoL, the walkable safe house) the script's state machine commands (0xD: object, machine, state), in order
int CScptSmaCommands(LO* pscpt, int (*an)[3], int cMax);
// the scripts' native data forgotten (a new world)
void ResetScpts();

struct PADCTL;
bool FScptActorGoingTo(const void* prun);
// orig: 0x001E46A0 (the actor's controller filled by the runner driving it: actor +0x1084, from the actor's update
// 0x1403D0)
void DriveScptActor(void* prun, PADCTL* ppc);
